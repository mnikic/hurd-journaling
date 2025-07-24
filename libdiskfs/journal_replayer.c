/* journal_replayer.c - Journal replayer for GNU Hurd journaling

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

#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_globals.h>
#include <libdiskfs/journal_util.h>
#include <libdiskfs/crc32.h>
#include <libdiskfs/diskfs.h>
#include <libdiskfs/journal_graph.h>
#include <libdiskfs/journal_arena.h>
#include <libdiskfs/journal_io.h>
#include <libdiskfs/journal_fs_helper.h>
#include <libdiskfs/journal_apply.h>
#include <libdiskfs/journal_inode_scanner.h>
#include <libdiskfs/journal_inode_denylist.h>
#include "priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))
#define ENTRY_SIZE       sizeof (journal_entry_bin_t)
#define PAYLOAD_PTR_SIZE   sizeof (journal_payload_bin_t *)
#define GRAPH_NODE_SIZE    sizeof (inode_graph_node_t)
#define REPLAY_STATE_SIZE  sizeof (inode_replay_state_t *)
#define AVG_STRING_SIZE    128
#define AVG_STRINGS_PER_ENTRY 2
#define ARENA_SIZE (ALIGN_UP (\
                          (JOURNAL_NUM_ENTRIES * (\
                              ENTRY_SIZE\
                            + PAYLOAD_PTR_SIZE\
                            + GRAPH_NODE_SIZE\
                            + REPLAY_STATE_SIZE\
                            + (AVG_STRINGS_PER_ENTRY * AVG_STRING_SIZE))), 8) * 1.5)

static journal_inode_denylist_t denylist_instance;
const journal_inode_denylist_t *journal_denylist = &denylist_instance;
journal_ino_t journal_raw_ino;

struct journal_entries
{
  struct journal_payload_bin **entries;
  size_t count;
  size_t capacity;
};

static bool
add_event_to_list (struct journal_entries *list,
		   struct journal_payload_bin *entry)
{
  if (list->count == list->capacity)
    {
      JOURNAL_LOG_ERROR ("Too many journal entries. Parsed %zu entries.",
			 list->count);
      return false;
    }
  list->entries[list->count++] = entry;
  return true;
}

static int
compare_entries_by_time_then_txid (const void *a, const void *b)
{
  const struct journal_payload_bin *entry_a =
    *(const struct journal_payload_bin **) a;
  const struct journal_payload_bin *entry_b =
    *(const struct journal_payload_bin **) b;

  if (entry_a->timestamp_ms < entry_b->timestamp_ms)
    return -1;
  if (entry_a->timestamp_ms > entry_b->timestamp_ms)
    return 1;
  if (entry_a->tx_id < entry_b->tx_id)
    return -1;
  if (entry_a->tx_id > entry_b->tx_id)
    return 1;
  return 0;
}

static void
sort_entries (struct journal_entries *list)
{
  qsort (list->entries, list->count,
	 PAYLOAD_PTR_SIZE, compare_entries_by_time_then_txid);
}

/*
 * fetch_and_validate_journal - Reads journal from disk, validates, and loads entries.
 * Returns true on success, false on error. Uses arena for memory.
 */
static bool
fetch_and_validate_journal (struct journal_arena *arena,
			    struct journal_entries *out_entries)
{
  journal_header_t *hdr = journal_arena_alloc (arena, sizeof (journal_header_t));
  if (!hdr)
    return false;
  memset(hdr, 0, sizeof(*hdr));

  if (!journal_read_and_validate_header (hdr))
    {
      return false;
    }

  JOURNAL_LOG_DEBUG ("Header: start index %llu, end index %llu",
		     hdr->start_index, hdr->end_index);

  out_entries->count = 0;
  out_entries->entries =
    journal_arena_alloc (arena, JOURNAL_NUM_ENTRIES * PAYLOAD_PTR_SIZE);
  out_entries->capacity = JOURNAL_NUM_ENTRIES;
  if (!out_entries->entries)
    {
      JOURNAL_LOG_ERROR ("Failed to allocate journal entry list");
      return false;
    }

  uint64_t index = hdr->start_index;
  uint64_t end_index = hdr->end_index;

  while (index != end_index)
    {
      journal_entry_bin_t *entry =
	journal_arena_alloc (arena, ENTRY_SIZE);
      if (!entry)
	{
	  JOURNAL_LOG_ERROR ("Out of memory allocating payload at index %llu",
			     index);
	  return false;
	}
      if (!journal_read_and_validate_entry (index, entry))
	{
	  JOURNAL_LOG_ERROR
	    ("CRC check failed or corrupted payload at index %llu", index);
	  return false;
	}
      journal_payload_bin_t *payload = &entry->payload;
      if (payload->action == JOURNAL_ACTION_UNKNOWN || payload->ino == 0)
	{
	  JOURNAL_LOG_ERROR
	    ("Invalid entry: action=%u ino=%u at index %llu (tx_id %llu)",
	     payload->action, payload->ino, index, payload->tx_id);
	  return false;
	}
      if (!add_event_to_list (out_entries, payload))
	{
	  return false;
	}
      index = (index + 1) % JOURNAL_NUM_ENTRIES;
    }

  return true;
}

static void
journal_init_state (void)
{
  journal_inode_denylist_builder_t builder =
    journal_inode_denylist_builder_init ();

  journal_scan_path_for_inos ("/dev", &builder);
  journal_scan_path_for_inos ("/var/log", &builder);
  // Finalize into global denylist instance
  denylist_instance = journal_inode_denylist_finalize (&builder);
}

/*
 * journal_replay_from_file - Main entry point for replaying the journal.
 * Reconstructs inode graph and applies metadata changes in early boot.
 */
void
journal_replay_from_file (const char *path)
{
  (void) path;
  JOURNAL_LOG_DEBUG ("Starting journal validation.");
  journal_enabled = false;

  journal_init_state ();
  struct journal_arena *arena = journal_arena_create (ARENA_SIZE);
  if (!arena)
    {
      JOURNAL_LOG_ERROR
	("Unable to allocate enough memory for journal replay. Aborting!");
      // Even if replay fails, enable journaling to start capturing future metadata
      journal_enabled = true;
      return;
    }

  struct journal_entries list = { 0 };
  bool success = fetch_and_validate_journal (arena, &list);
  if (!success)
    {
      JOURNAL_LOG_ERROR
	("Aborting replay due to validation failure. No entries replayed.");
      goto CLEANUP;
    }

  JOURNAL_LOG_DEBUG ("Validation completed successfully.");

  sort_entries (&list);
  for (size_t i = 0; i < list.count; ++i)
    journal_graph_add_event (list.entries[i]);

  inode_replay_state_t **entries;
  size_t count = journal_graph_get_all (&entries, arena);

  JOURNAL_LOG_DEBUG ("Starting restoration of metadata");
  if (pthread_rwlock_trywrlock (&diskfs_fsys_lock) == 0)
    {
      JOURNAL_LOG_DEBUG ("unlock :)");
      // Lock acquired
      diskfs_set_readonly (0);

      JOURNAL_LOG_DEBUG ("Filesystem NOT in readonly mode now!");

      for (size_t i = 0; i < count; ++i)
	apply_node_replay (entries[i]);

      diskfs_sync_everything (1);
      diskfs_set_hypermetadata (1, 1);
      _diskfs_diskdirty = 0;
      error_t err = diskfs_set_readonly (1);
      if (err)
	JOURNAL_LOG_ERROR ("Failed to restore diskfs_readonly = 1: %s (%d)",
			   strerror (err), err);
      else
	JOURNAL_LOG_DEBUG ("Filesystem set back to readonly");

      pthread_rwlock_unlock (&diskfs_fsys_lock);
    }
  else
    {
      JOURNAL_LOG_DEBUG ("didnt unlock :(");
    }

  JOURNAL_LOG_DEBUG ("Done with restoration.");

CLEANUP:
  journal_graph_free ();
  journal_arena_destroy (arena);
  journal_enabled = true;
}

