/**
 *  TV headend - Timeshift
 *  Copyright (C) 2012 Adam Sutton
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tvheadend.h"
#include "streaming.h"
#include "timeshift.h"
#include "timeshift/private.h"
#include "config.h"
#include "settings.h"
#include "atomic.h"
#include "access.h"
#include "atomic.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

static int timeshift_index = 0;

struct timeshift_conf timeshift_conf;

/*
 * Safe values for RAM configuration
 */
static void timeshift_fixup ( void )
{
  if (timeshift_conf.ram_only)
    timeshift_conf.max_size = timeshift_conf.ram_size;
}

/*
 * Intialise global file manager
 */
void timeshift_init ( void )
{
  htsmsg_t *m;

  timeshift_filemgr_init();

  /* Defaults */
  memset(&timeshift_conf, 0, sizeof(timeshift_conf));
  timeshift_conf.idnode.in_class = &timeshift_conf_class;
  timeshift_conf.max_period       = 60;                      // Hr (60mins)
  timeshift_conf.max_size         = 10000 * (size_t)1048576; // 10G

  /* Load settings */
  if ((m = hts_settings_load("timeshift/config"))) {
    idnode_load(&timeshift_conf.idnode, m);
    htsmsg_destroy(m);
    timeshift_fixup();
  }
}

/*
 * Terminate global file manager
 */
void timeshift_term ( void )
{
  timeshift_filemgr_term();
  free(timeshift_conf.path);
  timeshift_conf.path = NULL;
}

/*
 * Save settings
 */
static void timeshift_conf_class_save ( idnode_t *self )
{
  htsmsg_t *m;

  timeshift_fixup();

  m = htsmsg_create_map();
  idnode_save(&timeshift_conf.idnode, m);
  hts_settings_save(m, "timeshift/config");
  htsmsg_destroy(m);
}

/*
 * Class
 */
static const void *
timeshift_conf_class_max_size_get ( void *o )
{
  static uint64_t r;
  r = timeshift_conf.max_size / 1048576LL;
  return &r;
}

static int
timeshift_conf_class_max_size_set ( void *o, const void *v )
{
  uint64_t u64 = *(uint64_t *)v * 1048576LL;
  if (u64 != timeshift_conf.max_size) {
    timeshift_conf.max_size = u64;
    return 1;
  }
  return 0;
}

static const void *
timeshift_conf_class_ram_size_get ( void *o )
{
  static uint64_t r;
  r = timeshift_conf.ram_size / 1048576LL;
  return &r;
}

static int
timeshift_conf_class_ram_size_set ( void *o, const void *v )
{
  uint64_t u64 = *(uint64_t *)v * 1048576LL;
  timeshift_conf.ram_segment_size = u64 / 10;
  if (u64 != timeshift_conf.ram_size) {
    timeshift_conf.ram_size = u64;
    return 1;
  }
  return 0;
}

const idclass_t timeshift_conf_class = {
  .ic_snode      = &timeshift_conf.idnode,
  .ic_class      = "timeshift",
  .ic_caption    = N_("Timeshift"),
  .ic_event      = "timeshift",
  .ic_perm_def   = ACCESS_ADMIN,
  .ic_save       = timeshift_conf_class_save,
  .ic_properties = (const property_t[]){
    {
      .type   = PT_BOOL,
      .id     = "enabled",
      .name   = N_("Enabled"),
      .off    = offsetof(timeshift_conf_t, enabled),
    },
    {
      .type   = PT_BOOL,
      .id     = "ondemand",
      .name   = N_("On-demand (no rewind)"),
      .off    = offsetof(timeshift_conf_t, ondemand),
    },
    {
      .type   = PT_STR,
      .id     = "path",
      .name   = N_("Storage path"),
      .off    = offsetof(timeshift_conf_t, path),
    },
    {
      .type   = PT_U32,
      .id     = "max_period",
      .name   = N_("Maximum period (mins)"),
      .off    = offsetof(timeshift_conf_t, max_period),
    },
    {
      .type   = PT_BOOL,
      .id     = "unlimited_period",
      .name   = N_("Unlimited time"),
      .off    = offsetof(timeshift_conf_t, unlimited_period),
    },
    {
      .type   = PT_S64,
      .id     = "max_size",
      .name   = N_("Maximum size (MB)"),
      .set    = timeshift_conf_class_max_size_set,
      .get    = timeshift_conf_class_max_size_get,
    },
    {
      .type   = PT_S64,
      .id     = "ram_size",
      .name   = N_("Maximum RAM size (MB)"),
      .set    = timeshift_conf_class_ram_size_set,
      .get    = timeshift_conf_class_ram_size_get,
    },
    {
      .type   = PT_BOOL,
      .id     = "unlimited_size",
      .name   = N_("Unlimited size"),
      .off    = offsetof(timeshift_conf_t, unlimited_size),
    },
    {
      .type   = PT_BOOL,
      .id     = "ram_only",
      .name   = N_("RAM only"),
      .off    = offsetof(timeshift_conf_t, ram_only),
    },
    {}
  }
};

/*
 * Process a packet with time sorting
 */

#define MAX_TIME_DELTA (2*1000000) /* 2 seconds */
#define BACKLOG_COUNT ARRAY_SIZE(timeshift_t->backlog)

static void
timeshift_packet_deliver ( timeshift_t *ts, streaming_message_t *sm )
{
  th_pkt_t *pkt = sm->sm_data;
  tvhtrace("timeshift",
           "ts %d pkt buf - stream %d type %c pts %10"PRId64
           " dts %10"PRId64" dur %10d len %6zu time %14"PRId64,
           ts->id,
           pkt->pkt_componentindex,
           pkt_frametype_to_char(pkt->pkt_frametype),
           ts_rescale(pkt->pkt_pts, 1000000),
           ts_rescale(pkt->pkt_dts, 1000000),
           pkt->pkt_duration,
           pktbuf_len(pkt->pkt_payload),
           sm->sm_time);
  streaming_target_deliver2(&ts->wr_queue.sq_st, sm);
}

static void
timeshift_packet_flush ( timeshift_t *ts, int64_t time )
{
  streaming_message_t *lowest, *sm;
  struct streaming_message_queue *sq;
  int i;

  while (1) {
    lowest = NULL;
    for (i = 0; i < ts->backlog_max; i++) {
      sq = &ts->backlog[i];
      sm = TAILQ_FIRST(sq);
      if (sm && sm->sm_time + MAX_TIME_DELTA < time)
        if (lowest == NULL || lowest->sm_time > sm->sm_time)
          lowest = sm;
    }
    if (!lowest)
      break;
    TAILQ_REMOVE(sq, lowest, sm_link);
    timeshift_packet_deliver(ts, lowest);
  }
}

static void
timeshift_packet( timeshift_t *ts, th_pkt_t *pkt )
{
  streaming_message_t *sm;
  int64_t time;

  if (pkt->pkt_componentindex >= TIMESHIFT_BACKLOG_MAX) {
    pkt_ref_dec(pkt);
    return;
  }

  sm = streaming_msg_create_pkt(pkt);

  time = ts_rescale(pkt->pkt_pts, 1000000);
  if (time > ts->last_time) {
    atomic_exchange_s64(&ts->last_time, time);
    timeshift_packet_flush(ts, time);
  }

  sm->sm_time = time;
  if (time + MAX_TIME_DELTA < ts->last_time) {
    timeshift_packet_deliver(ts, sm);
  } else {
    if (pkt->pkt_componentindex >= ts->backlog_max)
      ts->backlog_max = pkt->pkt_componentindex + 1;
    TAILQ_INSERT_TAIL(&ts->backlog[pkt->pkt_componentindex], sm, sm_link);
  }
}

void
timeshift_packets_clone( timeshift_t *ts, struct streaming_message_queue *dst )
{
  streaming_message_t *lowest, *sm;
  struct streaming_message_queue *sq, *backlogs;
  int i;

  lock_assert(&ts->buffering_mutex);

  /* init temporary queues */
  backlogs = alloca(ts->backlog_max * sizeof(*backlogs));
  for (i = 0; i < ts->backlog_max; i++)
    TAILQ_INIT(&backlogs[i]);
  /* push to destination (pts sorted) */
  while (1) {
    lowest = NULL;
    for (i = 0; i < ts->backlog_max; i++) {
      sq = &backlogs[i];
      sm = TAILQ_FIRST(sq);
      if (sm && (lowest == NULL || lowest->sm_time > sm->sm_time))
        lowest = sm;
    }
    if (!lowest)
      break;
    TAILQ_REMOVE(sq, lowest, sm_link);
    TAILQ_INSERT_TAIL(dst, lowest, sm_link);
  }
}

/*
 * Receive data
 */
static void timeshift_input
  ( void *opaque, streaming_message_t *sm )
{
  int exit = 0, type = sm->sm_type;
  timeshift_t *ts = opaque;
  th_pkt_t *pkt = sm->sm_data, *pkt2;

  pthread_mutex_lock(&ts->state_mutex);

  /* Control */
  if (type == SMT_SKIP) {
    if (ts->state >= TS_LIVE)
      timeshift_write_skip(ts->rd_pipe.wr, sm->sm_data);
    streaming_msg_free(sm);
  } else if (type == SMT_SPEED) {
    if (ts->state >= TS_LIVE)
      timeshift_write_speed(ts->rd_pipe.wr, sm->sm_code);
    streaming_msg_free(sm);
  }

  else {

    /* Start */
    if (type == SMT_START && ts->state == TS_INIT)
      ts->state = TS_LIVE;

    pthread_mutex_lock(&ts->buffering_mutex);

    /* Change PTS/DTS offsets */
    if (ts->packet_mode && ts->start_pts && type == SMT_PACKET) {
      pkt2 = pkt_copy_shallow(pkt);
      pkt_ref_dec(pkt);
      sm->sm_data = pkt2;
      pkt2->pkt_pts += ts->start_pts;
      pkt2->pkt_dts += ts->start_pts;
    }

    /* Pass-thru */
    if (ts->state <= TS_LIVE) {
      if (type == SMT_START) {
        if (ts->smt_start)
          streaming_start_unref(ts->smt_start);
        ts->smt_start = sm->sm_data;
        atomic_add(&ts->smt_start->ss_refcount, 1);
        if (ts->packet_mode) {
          timeshift_packet_flush(ts, ts->last_time + MAX_TIME_DELTA + 1000);
          ts->start_pts = ts->last_time + 1000;
        }
      }
      streaming_target_deliver2(ts->output, streaming_msg_clone(sm));
    }

    /* Check for exit */
    if (type == SMT_EXIT ||
        (type == SMT_STOP && sm->sm_code != SM_CODE_SOURCE_RECONFIGURED))
      exit = 1;

    if (type == SMT_MPEGTS)
      ts->packet_mode = 0;

    /* Buffer to disk */
    if ((ts->state > TS_LIVE) || (!ts->ondemand && (ts->state == TS_LIVE))) {
      if (ts->packet_mode) {
        sm->sm_time = ts->last_time;
        if (type == SMT_PACKET) {
          timeshift_packet(ts, pkt);
          sm->sm_data = NULL;
          streaming_msg_free(sm);
          goto pktcont;
        }
      } else {
        if (ts->ref_time == 0) {
          ts->ref_time = getmonoclock();
          sm->sm_time = 0;
        } else {
          sm->sm_time = getmonoclock() - ts->ref_time;
        }
      }
      streaming_target_deliver2(&ts->wr_queue.sq_st, sm);
    } else {
      if (type == SMT_PACKET) {
        tvhtrace("timeshift",
                 "ts %d pkt in  - stream %d type %c pts %10"PRId64
                 " dts %10"PRId64" dur %10d len %6zu",
                 ts->id,
                 pkt->pkt_componentindex,
                 pkt_frametype_to_char(pkt->pkt_frametype),
                 ts_rescale(pkt->pkt_pts, 1000000),
                 ts_rescale(pkt->pkt_dts, 1000000),
                 pkt->pkt_duration,
                 pktbuf_len(pkt->pkt_payload));
      }
      streaming_msg_free(sm);
    }
pktcont:

    pthread_mutex_unlock(&ts->buffering_mutex);

    /* Exit/Stop */
    if (exit) {
      timeshift_write_exit(ts->rd_pipe.wr);
      ts->state = TS_EXIT;
    }
  }

  pthread_mutex_unlock(&ts->state_mutex);
}

/**
 *
 */
void
timeshift_destroy(streaming_target_t *pad)
{
  timeshift_t *ts = (timeshift_t*)pad;
  streaming_message_t *sm;
  int i;

  /* Must hold global lock */
  lock_assert(&global_lock);

  /* Ensure the threads exits */
  // Note: this is a workaround for the fact the Q might have been flushed
  //       in reader thread (VERY unlikely)
  pthread_mutex_lock(&ts->state_mutex);
  sm = streaming_msg_create(SMT_EXIT);
  streaming_target_deliver2(&ts->wr_queue.sq_st, sm);
  timeshift_write_exit(ts->rd_pipe.wr);
  pthread_mutex_unlock(&ts->state_mutex);

  /* Wait for all threads */
  pthread_join(ts->rd_thread, NULL);
  pthread_join(ts->wr_thread, NULL);

  /* Shut stuff down */
  streaming_queue_deinit(&ts->wr_queue);
  for (i = 0; i < TIMESHIFT_BACKLOG_MAX; i++)
    streaming_queue_clear(&ts->backlog[i]);

  close(ts->rd_pipe.rd);
  close(ts->rd_pipe.wr);

  /* Flush files */
  timeshift_filemgr_flush(ts, NULL);

  /* Release SMT_START index */
  if (ts->smt_start)
    streaming_start_unref(ts->smt_start);

  if (ts->path)
    free(ts->path);
  free(ts);
}

/**
 * Create timeshift buffer
 *
 * max_period of buffer in seconds (0 = unlimited)
 * max_size   of buffer in bytes   (0 = unlimited)
 */
streaming_target_t *timeshift_create
  (streaming_target_t *out, time_t max_time)
{
  timeshift_t *ts = calloc(1, sizeof(timeshift_t));
  int i;

  /* Must hold global lock */
  lock_assert(&global_lock);

  /* Setup structure */
  TAILQ_INIT(&ts->files);
  ts->output     = out;
  ts->path       = NULL;
  ts->max_time   = max_time;
  ts->state      = TS_INIT;
  ts->full       = 0;
  ts->vididx     = -1;
  ts->id         = timeshift_index;
  ts->ondemand   = timeshift_conf.ondemand;
  ts->packet_mode= 1;
  ts->last_time  = 0;
  ts->start_pts  = 0;
  ts->ref_time   = 0;
  for (i = 0; i < TIMESHIFT_BACKLOG_MAX; i++)
    TAILQ_INIT(&ts->backlog[i]);
  pthread_mutex_init(&ts->rdwr_mutex, NULL);
  pthread_mutex_init(&ts->state_mutex, NULL);
  pthread_mutex_init(&ts->buffering_mutex, NULL);

  /* Initialise output */
  tvh_pipe(O_NONBLOCK, &ts->rd_pipe);

  /* Initialise input */
  streaming_queue_init(&ts->wr_queue, 0, 0);
  streaming_target_init(&ts->input, timeshift_input, ts, 0);
  tvhthread_create(&ts->wr_thread, NULL, timeshift_writer, ts, "tshift-wr");
  tvhthread_create(&ts->rd_thread, NULL, timeshift_reader, ts, "tshift-rd");

  /* Update index */
  timeshift_index++;

  return &ts->input;
}
