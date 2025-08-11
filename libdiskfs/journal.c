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
#include <libdiskfs/journal_shadow_fs.h>
#include <libdiskfs/journal_arena.h>
#include <libdiskfs/journal_policy.h>
#include <libdiskfs/journal_globals.h>
#include <libdiskfs/journal_replayer.h>
#include <libdiskfs/journal_path_util.h>
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
static struct journal_arena *sfs_arena;
journal_layout_t journal_layout;

static inline bool
layout_init (struct store *store, journal_config_t config)
{
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
      return false;
    }
  JOURNAL_LOG_DEBUG ("Computed %zu spaces in the journal",
		     journal_layout.num_entries);
  return true;
}

static void
denylist_init (void)
{
  journal_inode_denylist_builder_t builder =
    journal_inode_denylist_builder_init ();

  for (int i = 0; journal_excluded_prefixes[i]; i++)
    journal_scan_path_for_inos (journal_excluded_prefixes[i], &builder);

  ino_denylist = journal_inode_denylist_finalize (&builder);
}

static void
shadowfs_init (void)
{
  sfs_arena = journal_arena_create (16 * 1024 * 1024);
  journal_sfs_init (sfs_arena);
  journal_seed_shadow_fs ();
}

void
journal_init (struct store *store, journal_config_t config)
{
  JOURNAL_LOG_DEBUG ("journal_init() called.");
  if (!layout_init (store, config))
    return;

  denylist_init ();
  shadowfs_init ();
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
  journal_arena_destroy (sfs_arena);
}

static inline bool
should_log_time (time_t value, int flag_set)
{
  return flag_set || (value > JOURNAL_MIN_REASONABLE_TIME
		      && value < JOURNAL_MAX_REASONABLE_TIME);
}

void
journal_log_metadata (void *node_ptr, const journal_entry_info_t *info)
{
  if (!journal_enabled)
    {
      return;
    }
  const struct node *np = (struct node *) node_ptr;
  const char *normalized_path = journal_normalize_path (info->path);
  if (!journal_should_log_event (np, info, &ino_denylist, normalized_path))
    return;

  const char *name = info->name ? info->name : "";
  const char *extra = info->extra ? info->extra : "";
  const char *old_name = info->old_name ? info->old_name : "";
  const char *new_name = info->new_name ? info->new_name : "";
  const char *target = info->target ? info->target : "";

  size_t total_size = sizeof (journal_payload_bin_t);
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
  entry->st_gen = st->st_gen;

  entry->uid = st->st_uid;
  entry->gid = st->st_gid;
  entry->flags = st->st_flags;
  entry->action = info->action;
  entry->author = st->st_author;

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

  safe_strncpy (entry->name, name, sizeof (entry->name));
  safe_strncpy (entry->extra, extra, sizeof (entry->extra));
  safe_strncpy (entry->old_name, old_name, sizeof (entry->old_name));
  safe_strncpy (entry->new_name, new_name, sizeof (entry->new_name));
  safe_strncpy (entry->target, target, sizeof (entry->target));

  snprintf (entry->path, sizeof (entry->path), "%s", normalized_path);
  char shadow_path[1024];
  error_t r =
    journal_sfs_resolve_path (st->st_ino, shadow_path, sizeof shadow_path);
  if (r != 0)
    {
      JOURNAL_LOG_DEBUG ("Couldn't resolve shadow path (ino=%" PRIu64
			 ", err=%s)", (uint64_t) st->st_ino, strerror (r));
      shadow_path[0] = '\0';	// already ensured by resolver, but harmless
      shadowfs_stats_t st;
      journal_sfs_get_stats (&st);

      JOURNAL_LOG_DEBUG
	("ShadowFS stats: entries=%zu, arena_used=%zu, updates=%zu, renames=%zu, "
	 "victims=%zu, deletes=%zu, resolve_ok=%zu, resolve_fail=%zu, degraded=%d",
	 (size_t) st.entries, (size_t) st.arena_used, (size_t) st.updates,
	 (size_t) st.renames, (size_t) st.victims, (size_t) st.deletes,
	 (size_t) st.resolve_ok, (size_t) st.resolve_fail, (int) st.degraded);
    }
  JOURNAL_LOG_DEBUG ("Logging inode: %u tx_id=%" PRIu64
		     " action=%u name=%s path=%s shadow path=%s", entry->ino,
		     entry->tx_id, entry->action, entry->name,
		     normalized_path, shadow_path);

  if (journal_enabled)
    {
      if (!journal_write (entry))
	JOURNAL_LOG_ERROR ("Failed to write to journal.");
    }

  free (buf);
}
