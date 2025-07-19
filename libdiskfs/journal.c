/* journal.c - Core metadata logger/coordinator for journaling in GNU Hurd

   Copyright (C) 2025 Free Software Foundation, Inc.

   Written by Milos Nikic.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   The GNU Hurd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with the GNU Hurd; if not, see <https://www.gnu.org/licenses/>.  */

#include <libdiskfs/journal.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_queue.h>
#include <libdiskfs/journal_writer.h>
#include <libdiskfs/journal_globals.h>
#include <libdiskfs/journal_replayer.h>
#include <libdiskfs/journal_util.h>
#include <libdiskfs/diskfs.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <hurd/fshelp.h>

#define MAX_REASONABLE_TIME 16725229200	/* Jan 1, 2500 */
#define MIN_REASONABLE_TIME 315536400		/* Jan 1, 1980 */

static volatile uint64_t journal_tx_id = 1;
static volatile bool journal_shutting_down;
static pthread_t journal_flusher_tid;
static pthread_t monitor_tid;

volatile bool journal_enabled = false;

static uint64_t
current_time_ms (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return ((uint64_t) tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

static void *
journal_device_monitor_thread (void *arg)
{
  (void) arg;
  while (1)
    {
      int fd = open (RAW_DEVICE_PATH, O_RDWR);
      if (fd >= 0)
	{
	  if (!journal_device_ready)
	    {
	      fsync (fd);
	      char test_buf[1];
	      ssize_t n = pread (fd, test_buf, sizeof (test_buf), 0);

	      if (n == 1)
		{
		  journal_device_ready = true;
		  JOURNAL_LOG_DEBUG ("All checks worked. Journal device is ready!");
		  pthread_mutex_lock (&queue_lock);
		  pthread_cond_signal (&queue_cond);	// Wake queue flusher
		  pthread_mutex_unlock (&queue_lock);
		}
	      else
		{
		  JOURNAL_LOG_DEBUG ("pread returned %zd, still not ready", n);
		}
	    }
	}
      else
	{
	  if (journal_device_ready)
	    {
	      journal_device_ready = false;
	      JOURNAL_LOG_DEBUG ("Journal device is not ready.");
	    }
	}

      if (fd >= 0)
	close (fd);

      int sleep_ms = journal_device_ready ? 1000 : 100;	// 1s if ready, 100ms if not
      usleep (sleep_ms * 1000);
    }
  return NULL;
}

void
journal_init (void)
{
  JOURNAL_LOG_DEBUG ("journal_init() called.");

  journal_queue_init ();

  if (pthread_create (&journal_flusher_tid, NULL, journal_flusher_thread, NULL) != 0)
    {
      JOURNAL_LOG_ERROR ("Failed to create a flusher thread.");
      journal_shutting_down = true;
    }

  if (pthread_create (&monitor_tid, NULL, journal_device_monitor_thread, NULL) != 0)
    {
      JOURNAL_LOG_ERROR ("Failed to start journal device monitor thread.");
    }
  else
    {
      JOURNAL_LOG_DEBUG ("Started journal device monitor thread.");
    }

  JOURNAL_LOG_DEBUG ("Done initializing.");
}

void
journal_shutdown (void)
{
  JOURNAL_LOG_DEBUG ("journal_shutdown() called.");
  journal_shutting_down = true;
  journal_queue_shutdown ();
  pthread_join (journal_flusher_tid, NULL);
  pthread_join (monitor_tid, NULL);
}

void
journal_restore (void)
{
  journal_replay_from_file (RAW_DEVICE_PATH);
}

void
journal_log_metadata (void *node_ptr, const struct journal_entry_info *info,
		      journal_durability_t durability)
{
  if (!node_ptr)
    {
      JOURNAL_LOG_ERROR ("NULL node_ptr received in journal_log_metadata, skipping.");
      return;
    }

  if (!info)
    {
      JOURNAL_LOG_ERROR ("NULL info pointer received in journal_log_metadata, skipping.");
      return;
    }

  const struct stat *st = &((struct node *) node_ptr)->dn_stat;

  if (journal_is_ino_denied ((journal_ino_t) st->st_ino))
    {
      return;
    }

  if (!journal_is_safe_stat (st))
    {
      JOURNAL_LOG_DEBUG ("Skipped inode %llu (mode %o) as unsafe.",
                         st->st_ino, st->st_mode);
      return;
    }

  const char *name     = info->name     ? info->name     : "";
  const char *extra    = info->extra    ? info->extra    : "";
  const char *old_name = info->old_name ? info->old_name : "";
  const char *new_name = info->new_name ? info->new_name : "";
  const char *target   = info->target   ? info->target   : "";

  size_t total_size = sizeof (struct journal_payload_bin);
  if (total_size > JOURNAL_ENTRY_SIZE)
    {
      JOURNAL_LOG_ERROR ("Entry too large, dropped.");
      return;
    }

  char *buf = calloc (1, total_size);
  if (!buf)
    return;

  struct journal_payload_bin *entry = (struct journal_payload_bin *) buf;

  entry->tx_id = __atomic_add_fetch (&journal_tx_id, 1, __ATOMIC_SEQ_CST);
  entry->timestamp_ms = current_time_ms ();

  entry->parent_ino     = (journal_ino_t) info->parent_ino;
  entry->src_parent_ino = (journal_ino_t) info->src_parent_ino;
  entry->dst_parent_ino = (journal_ino_t) info->dst_parent_ino;
  entry->ino            = (journal_ino_t) st->st_ino;

  entry->st_mode   = st->st_mode;
  entry->st_size   = st->st_size;
  entry->st_nlink  = st->st_nlink;
  entry->st_blocks = st->st_blocks;

  if (st->st_mtime > MIN_REASONABLE_TIME && st->st_mtime < MAX_REASONABLE_TIME)
    {
      entry->mtime = st->st_mtime;
      entry->has_mtime = true;
    }

  if (st->st_ctime > MIN_REASONABLE_TIME && st->st_ctime < MAX_REASONABLE_TIME)
    {
      entry->ctime = st->st_ctime;
      entry->has_ctime = true;
    }

  if (st->st_atime > MIN_REASONABLE_TIME && st->st_atime < MAX_REASONABLE_TIME)
    {
      entry->atime = st->st_atime;
      entry->has_atime = true;
    }

  if (info->has_mode)
    {
      entry->st_mode = info->mode;
      entry->has_mode = true;
    }

  if (info->has_size)
    {
      entry->st_size = info->size;
      entry->has_size = true;
    }

  if (info->has_uid)
    {
      entry->uid = (journal_uid_t) info->uid;
      entry->has_uid = true;
    }

  if (info->has_gid)
    {
      entry->gid = (journal_uid_t) info->gid;
      entry->has_gid = true;
    }

  if (info->has_flags)
    {
      entry->flags = info->flags;
      entry->has_flags = true;
    }

  entry->action = info->action;

  strncpy (entry->name,     name,     sizeof (entry->name)     - 1);
  strncpy (entry->extra,    extra,    sizeof (entry->extra)    - 1);
  strncpy (entry->old_name, old_name, sizeof (entry->old_name) - 1);
  strncpy (entry->new_name, new_name, sizeof (entry->new_name) - 1);
  strncpy (entry->target,   target,   sizeof (entry->target)   - 1);

  // Null-terminate just to be safe
  entry->name[sizeof (entry->name) - 1] = '\0';
  entry->extra[sizeof (entry->extra) - 1] = '\0';
  entry->old_name[sizeof (entry->old_name) - 1] = '\0';
  entry->new_name[sizeof (entry->new_name) - 1] = '\0';
  entry->target[sizeof (entry->target) - 1] = '\0';

  JOURNAL_LOG_DEBUG ("Logging inode: %u", entry->ino);

  if (journal_enabled && journal_device_ready &&
      durability == JOURNAL_DURABILITY_SYNC)
    {
      if (!journal_write_raw_sync (entry))
	JOURNAL_LOG_ERROR ("Failed to write sync.");
    }
  else
    {
      /* journal_enqueue copies the buffer internally, so it is safe to free here */
      journal_enqueue (buf, total_size);
    }

  free (buf);
}

