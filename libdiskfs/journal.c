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
#include <libdiskfs/journal_io.h>
#include <libdiskfs/journal_format.h>
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
#include <hurd/store.h>

#define MAX_REASONABLE_TIME 16725229200	/* Jan 1, 2500 */
#define MIN_REASONABLE_TIME 315536400	/* Jan 1, 1980 */

static volatile uint64_t journal_tx_id = 1;
static volatile bool journal_shutting_down;
volatile bool journal_enabled = false;

static uint64_t
current_time_ms (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return ((uint64_t) tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

void
journal_init (struct store *store)
{
  JOURNAL_LOG_DEBUG ("journal_init() called.");
  journal_io_set_store (store);
  JOURNAL_LOG_DEBUG ("Done initializing.");
}

void
journal_shutdown (void)
{
  JOURNAL_LOG_DEBUG ("journal_shutdown() called.");
  journal_shutting_down = true;
}

void
journal_restore (void)
{
  journal_replay ();
}

static inline bool
should_log_time (time_t value, int flag_set)
{
  return flag_set || (value > MIN_REASONABLE_TIME
		      && value < MAX_REASONABLE_TIME);
}

void
journal_log_metadata (void *node_ptr, const struct journal_entry_info *info,
		      journal_durability_t durability)
{
  if (!node_ptr)
    {
      JOURNAL_LOG_ERROR
	("NULL node_ptr received in journal_log_metadata, skipping.");
      return;
    }

  if (!info)
    {
      JOURNAL_LOG_ERROR
	("NULL info pointer received in journal_log_metadata, skipping.");
      return;
    }
  const struct node *np = (struct node *) node_ptr;
  const struct stat *st = &np->dn_stat;

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

  const char *name = info->name ? info->name : "";
  const char *extra = info->extra ? info->extra : "";
  const char *old_name = info->old_name ? info->old_name : "";
  const char *new_name = info->new_name ? info->new_name : "";
  const char *target = info->target ? info->target : "";

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

  entry->parent_ino = (journal_ino_t) info->parent_ino;
  entry->src_parent_ino = (journal_ino_t) info->src_parent_ino;
  entry->dst_parent_ino = (journal_ino_t) info->dst_parent_ino;
  entry->ino = (journal_ino_t) st->st_ino;

  entry->st_mode = st->st_mode;
  entry->st_size = st->st_size;
  entry->st_nlink = st->st_nlink;
  entry->st_blocks = st->st_blocks;

  if (should_log_time (st->st_mtime, np->dn_set_mtime))
    {
      entry->mtime = st->st_mtime;
      entry->has_mtime = true;
    }
  if (should_log_time (st->st_ctime, np->dn_set_ctime))
    {
      entry->ctime = st->st_ctime;
      entry->has_ctime = true;
    }
  if (should_log_time (st->st_atime, np->dn_set_atime))
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

  strncpy (entry->name, name, sizeof (entry->name) - 1);
  strncpy (entry->extra, extra, sizeof (entry->extra) - 1);
  strncpy (entry->old_name, old_name, sizeof (entry->old_name) - 1);
  strncpy (entry->new_name, new_name, sizeof (entry->new_name) - 1);
  strncpy (entry->target, target, sizeof (entry->target) - 1);

  // Null-terminate just to be safe
  entry->name[sizeof (entry->name) - 1] = '\0';
  entry->extra[sizeof (entry->extra) - 1] = '\0';
  entry->old_name[sizeof (entry->old_name) - 1] = '\0';
  entry->new_name[sizeof (entry->new_name) - 1] = '\0';
  entry->target[sizeof (entry->target) - 1] = '\0';

  JOURNAL_LOG_DEBUG ("Logging inode: %u tx_id=%llu action=%u", entry->ino,
		     entry->tx_id, entry->action);

  if (journal_enabled && durability == JOURNAL_DURABILITY_SYNC)
    {
      if (!journal_write_raw_sync (entry))
	JOURNAL_LOG_ERROR ("Failed to write sync.");
    }

  free (buf);
}
