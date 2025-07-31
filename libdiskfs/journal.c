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
#include <libdiskfs/journal_internal.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_writer.h>
#include <libdiskfs/journal_policy.h>
#include <libdiskfs/journal_globals.h>
#include <libdiskfs/journal_replayer.h>
#include <libdiskfs/journal_util.h>
#include <libdiskfs/journal_inode_scanner.h>
#include <libdiskfs/journal_inode_denylist.h>
#include <libdiskfs/diskfs.h>

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
#include <string.h>
#include <stdio.h>


static volatile uint64_t journal_tx_id = 1;
static volatile bool journal_enabled = false;
static journal_inode_denylist_t ino_denylist;
journal_layout_t journal_layout;

static void
denylist_init (void)
{
  journal_inode_denylist_builder_t builder =
    journal_inode_denylist_builder_init ();

//  for (int i = 0; journal_excluded_prefixes[i]; i++)
  //  journal_scan_path_for_inos (journal_excluded_prefixes[i], &builder);

  ino_denylist = journal_inode_denylist_finalize (&builder);
}

void
journal_init (struct store *store, journal_config_t config)
{
  JOURNAL_LOG_DEBUG ("journal_init() called.");

  journal_layout.reserved_space = JOURNAL_HEADER_SIZE;
  journal_layout.header_size = JOURNAL_HEADER_SIZE;
  journal_layout.device_block_size = store->block_size;
  journal_layout.device_block_count = config.block_count;
  journal_layout.device_start_block = config.start_block;
  journal_layout.device_start_byte = config.start_block * store->block_size;
  journal_layout.entry_size = JOURNAL_ENTRY_SIZE;
  journal_layout.device_span_bytes =
    journal_layout.device_block_count * journal_layout.device_block_size;
  journal_layout.num_entries =
    (journal_layout.device_span_bytes - journal_layout.reserved_space)
    / journal_layout.entry_size;
  if (journal_layout.num_entries < 100)
    {
      JOURNAL_LOG_ERROR ("Not enough space for journaling!");
      return;
    }
  JOURNAL_LOG_DEBUG ("Computed %u spaces in the journal",
		     journal_layout.num_entries);
  denylist_init ();
  journal_io_set_store (store);
  journal_replay (&ino_denylist);
  journal_enabled = true;
  JOURNAL_LOG_DEBUG ("Done initializing.");
}

void
journal_shutdown (void)
{
  JOURNAL_LOG_DEBUG ("journal_shutdown() called.");
  journal_enabled = false;
}

static inline bool
should_log_time (time_t value, int flag_set)
{
  return flag_set || (value > JOURNAL_MIN_REASONABLE_TIME
		      && value < JOURNAL_MAX_REASONABLE_TIME);
}

static inline char *
toString (journal_action_t action)
{
  switch (action)
    {
    case JOURNAL_ACTION_CREATE:
      return "CREATE";
    case JOURNAL_ACTION_MKDIR:
      return "MKDIR";
    case JOURNAL_ACTION_MKFILE:
      return "MKFILE";
    case JOURNAL_ACTION_SYMLINK:
      return "SYMLINK";
    case JOURNAL_ACTION_LINK:
      return "LINK";
    case JOURNAL_ACTION_UNLINK:
      return "UNLINK";
    case JOURNAL_ACTION_RENAME:
      return "RENAME";
    case JOURNAL_ACTION_RMDIR:
      return "RMDIR";
    case JOURNAL_ACTION_CHMOD:
      return "CHMOD";
    case JOURNAL_ACTION_CHOWN:
      return "CHOWN";
    case JOURNAL_ACTION_UTIME:
      return "UTIME";
    case JOURNAL_ACTION_TRUNCATE:
      return "TRUNCATE";
    case JOURNAL_ACTION_GROW:
      return "GROW";
    case JOURNAL_ACTION_CHAUTHOR:
      return "CHAUTHOR";
    case JOURNAL_ACTION_CHFLAGS:
      return "FLASGS";
    case JOURNAL_ACTION_ATIME:
      return "ATIME";
    case JOURNAL_ACTION_WRITE:
      return "WRITE";
    }
  return "UNKNOWN";
}

void
journal_log_metadata (void *node_ptr, const struct journal_entry_info *info)
{
  if (!journal_enabled)
    {
      return;
    }
  const struct node *np = (struct node *) node_ptr;
  const char *normalized_path = journal_normalize_path (info->path);
  char full_path[JOURNAL_NORMALIZED_PATH_MAX];
  journal_combine_path_name (normalized_path, info->name, full_path,
			     sizeof (full_path));

//  if (np->dn_stat.st_ino != 48803 && strncmp(full_path, "/dev", strlen("/dev")) != 0)
  //  JOURNAL_LOG_ERROR ("ino: %llu action: %s name: %s path: %s normalized: %s full: %s", np->dn_stat.st_ino, toString(info->action), info->name, info->path, normalized_path, full_path);

  if (!journal_should_log_event (np, info, &ino_denylist, full_path))
    return;

  const char *name = info->name ? info->name : "";
  const char *extra = info->extra ? info->extra : "";
  const char *old_name = info->old_name ? info->old_name : "";
  const char *new_name = info->new_name ? info->new_name : "";
  const char *target = info->target ? info->target : "";

  size_t total_size = sizeof (journal_payload_bin_t);
  if (total_size > JOURNAL_ENTRY_SIZE)
    {
      JOURNAL_LOG_ERROR ("Entry too large, dropped.");
      return;
    }

  char *buf = calloc (1, total_size);
  if (!buf)
    return;

  journal_payload_bin_t *entry = (journal_payload_bin_t *) buf;

  entry->tx_id = __atomic_add_fetch (&journal_tx_id, 1, __ATOMIC_SEQ_CST);
  entry->timestamp_ms = journal_current_time_ms ();

  const struct stat *st = &np->dn_stat;
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

  strncpy (entry->path, full_path, sizeof (entry->path));

  JOURNAL_LOG_DEBUG ("Logging inode: %u tx_id=%llu action=%u name=%s path=%s",
		     entry->ino, entry->tx_id, entry->action, entry->name,
		     full_path);

  if (journal_enabled)
    {
      if (!journal_write_raw_sync (entry))
	JOURNAL_LOG_ERROR ("Failed to write sync.");
    }

  free (buf);
}
