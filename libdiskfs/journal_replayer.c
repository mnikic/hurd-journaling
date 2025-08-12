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

#include <libdiskfs/journal_internal.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_globals.h>
#include <libdiskfs/journal_path_util.h>
#include <libdiskfs/journal_util.h>
#include <libdiskfs/crc32.h>
#include <libdiskfs/diskfs.h>
#include <libdiskfs/journal_writer.h>
#include <libdiskfs/journal_inode_denylist.h>
#include <libdiskfs/journal_graph.h>
#include <libdiskfs/journal_arena.h>
#include <libdiskfs/journal_io.h>
#include <libdiskfs/journal_diskfs_helper.h>
#include <libdiskfs/journal_apply.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#define ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))
#define POINTER_SIZE   sizeof (void *)
#define GRAPH_NODE_SIZE    sizeof (inode_graph_node_t)
#define AVG_STRING_SIZE    256
#define AVG_STRINGS_PER_ENTRY 2
#define AVG_STRING_MEMORY_PER_ENTRY (AVG_STRINGS_PER_ENTRY * AVG_STRING_SIZE)

#define MEMORY_NEEDED_PER_ENTRY \
  (JOURNAL_ENTRY_SIZE + GRAPH_NODE_SIZE + POINTER_SIZE + AVG_STRING_MEMORY_PER_ENTRY)

#define ARENA_OVERPROVISION_PERCENT 50

#define ARENA_MEMORY(num_entries) \
  ALIGN_UP(((num_entries) * MEMORY_NEEDED_PER_ENTRY * (100 + ARENA_OVERPROVISION_PERCENT)) / 100, 8)

static inline size_t
arena_size (void)
{
  return ARENA_MEMORY (journal_layout.num_entries);
}

struct journal_entries
{
  struct journal_payload_bin **entries;
  size_t count;
  size_t capacity;
};

/*
 * fetch_and_validate_header - Reads and validates the journal header.
 *
 * Performs CRC and magic/version checks. Returns true if valid.
 */
static bool
fetch_and_validate_header (journal_header_t *out)
{
  error_t err = journal_read_header (out);
  if (err)
    {
      JOURNAL_LOG_ERROR ("journal replay: failed reading header: %d", err);
      return false;
    }

  if (out->crc32 != journal_compute_header_crc32 (out)
      || out->magic != JOURNAL_MAGIC || out->version != JOURNAL_VERSION)
    {
      JOURNAL_LOG_DEBUG ("journal replay: node header invalid.");
      return false;
    }

  if (out->start_index >= journal_layout.num_entries
      || out->end_index >= journal_layout.num_entries)
    {
      JOURNAL_LOG_DEBUG ("journal replay: header indices out of bounds.");
      return false;
    }

  return true;
}

/*
 * fetch_and_validate_entry - Reads and validates a journal entry.
 *
 * Performs CRC, magic, and version checks. Returns true if valid.
 */
static bool
fetch_and_validate_entry (uint64_t index, journal_entry_bin_t *out)
{
  error_t err = journal_read_entry (out, index);
  if (err)
    {
      JOURNAL_LOG_DEBUG ("journal_node_read failed at index %" PRIu64 ".",
			 index);
      return false;
    }
  if (out->magic != JOURNAL_MAGIC)
    {
      JOURNAL_LOG_DEBUG ("Bad journal entry magic at index %" PRIu64 ".",
			 index);
      return false;
    }

  if (out->version != JOURNAL_VERSION)
    {
      JOURNAL_LOG_DEBUG ("Journal entry version mismatch at index %" PRIu64
			 ".", index);
      return false;
    }

  uint32_t actual_entry_crc = journal_compute_payload_crc32 (&out->payload);
  if (actual_entry_crc != out->crc32)
    {
      JOURNAL_LOG_DEBUG ("Journal entry CRC mismatch at index %" PRIu64 ".",
			 index);
      return false;
    }

  return true;
}

static bool
add_event_to_list (struct journal_entries *list,
		   journal_payload_bin_t *payload)
{
  if (list->count == list->capacity)
    {
      JOURNAL_LOG_ERROR ("Too many journal entries. Parsed %zu entries.",
			 list->count);
      return false;
    }
  list->entries[list->count++] = payload;
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
	 POINTER_SIZE, compare_entries_by_time_then_txid);
}

/*
 * fetch_and_validate_journal - Reads journal from disk, validates, and loads entries.
 * Returns true on success, false on error. Uses arena for memory.
 */
static bool
fetch_and_validate_journal (struct journal_arena *arena,
			    const journal_inode_denylist_t *denylist,
			    struct journal_entries *out_entries)
{
  journal_header_t *hdr =
    journal_arena_alloc (arena, sizeof (journal_header_t));

  if (!hdr)
    return false;

  memset (hdr, 0, sizeof (*hdr));

  if (!fetch_and_validate_header (hdr))
    return false;

  JOURNAL_LOG_DEBUG ("Header: start index %" PRIu64 ", end index %" PRIu64 "",
		     hdr->start_index, hdr->end_index);

  out_entries->count = 0;
  out_entries->entries =
    journal_arena_alloc (arena, journal_layout.num_entries * POINTER_SIZE);
  out_entries->capacity = journal_layout.num_entries;
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
	journal_arena_alloc (arena, JOURNAL_ENTRY_SIZE);
      if (!entry)
	{
	  JOURNAL_LOG_ERROR ("Out of memory allocating payload at index %"
			     PRIu64 ".", index);
	  return false;
	}
      if (!fetch_and_validate_entry (index, entry))
	{
	  JOURNAL_LOG_ERROR
	    ("CRC check failed or corrupted payload at index %" PRIu64 "",
	     index);
	  return false;
	}
      journal_payload_bin_t *payload = &entry->payload;
      if (!journal_is_structural_action (payload->action))
	{
	  if (journal_inode_denylist_contains (denylist, payload->ino))
	    {
	      JOURNAL_LOG_DEBUG
		("Ino %u is in a deny list. Skipping tx %" PRIu64 ".",
		 payload->ino, payload->tx_id);
	      goto NEXT;
	    }
	  if (!journal_is_safe_stat (payload->st_mode))
	    {
	      JOURNAL_LOG_ERROR
		("Invalid mode on a journal entry ino=%u mode=%o. Aborting.",
		 payload->ino, payload->st_mode);
	      return false;
	    }
	  if (payload->action == JOURNAL_ACTION_UNKNOWN || payload->ino == 0
	      || payload->tx_id == 0 || payload->timestamp_ms == 0
	      || !(payload->has_mtime || payload->has_atime
		   || payload->has_ctime))
	    {
	      JOURNAL_LOG_ERROR
		("Invalid entry: action=%u ino=%u at index %" PRIu64
		 " (tx_id %" PRIu64 ")", payload->action, payload->ino, index,
		 payload->tx_id);
	      return false;
	    }
	}
      if (!add_event_to_list (out_entries, payload))
	{
	  return false;
	}
    NEXT:
      index = (index + 1) % journal_layout.num_entries;
    }

  return true;
}

/**
 * replay_apply_graph - Applies all reconstructed journal state to diskfs.
 *
 * Iterates over all nodes in the graph and applies replay changes.
 * Sets up root credentials and creates a restore directory,
 * then looks it up as a node and passes it down.
 */
static void
replay_apply_graph (struct journal_arena *arena)
{
  inode_replay_state_t **entries;
  size_t count = journal_graph_get_all (&entries, arena);

  struct node *root = diskfs_root_node;
  pthread_mutex_lock (&root->lock);
  diskfs_nref (root);

  struct protid *cred = NULL;
  if (diskfs_create_creds (root, O_READ | O_EXEC | O_WRITE, &cred))
    {
      JOURNAL_LOG_ERROR ("Could not create credentials. Aborting replay.");
      diskfs_nput (root);
      return;
    }

  char restore_prefix[MAX_FIELD_LEN];
  int written =
    snprintf (restore_prefix, sizeof (restore_prefix), "%s/%" PRIu64,
	      JOURNAL_RESTORE_ROOT,
	      journal_current_time_ms ());
  if (written < 0 || written >= sizeof (restore_prefix))
    safe_strncpy (restore_prefix, "/restore", sizeof (restore_prefix));

  error_t err = diskfs_mkdir_p (root, restore_prefix, cred);
  if (err)
    {
      JOURNAL_LOG_ERROR ("Failed to create restore directory: %s",
			 strerror (err));
      goto CLEANUP;
    }

  struct node *restore_root = NULL;
  err = diskfs_lookup_path (root, restore_prefix, cred, &restore_root);
  if (err || !restore_root)
    {
      JOURNAL_LOG_ERROR ("Failed to lookup restore directory: %s",
			 strerror (err));
      goto CLEANUP;
    }
  JOURNAL_LOG_DEBUG
    ("Starting restoration. Have %zu entries and restore path is '%s'", count,
     restore_prefix);
  size_t with_paths = 0;
  size_t with_shadow_path = 0;
  for (size_t i = 0; i < count; ++i)
    {
      inode_replay_state_t *state = entries[i];
      if (is_path_usable (state->resolved_path))
	with_paths++;
      if (is_path_usable (state->shadow_path))
	with_shadow_path++;
      err = apply_node_replay (state, root, restore_root, cred);
      if (err)
	JOURNAL_LOG_ERROR ("Restore error: ino=%u name=%s path=%s err=%s",
			   state->ino,
			   state->name, state->resolved_path, strerror (err));
    }
  JOURNAL_LOG_DEBUG
    ("Out of %zu graph entries, %zu of them had paths and %zu had shadow paths.",
     count, with_paths, with_shadow_path);

  diskfs_nput (restore_root);
CLEANUP:
  ports_port_deref (cred);
  diskfs_nput (root);
}

/**
 * replay_main_pass - Main core of the journal replay pipeline.
 *
 * This function loads and validates the journal entries,
 * sorts them, and builds the in-memory graph structure.
 * After successful graph construction, it triggers the replay logic.
 */
static void
replay_main_pass (struct journal_arena *arena,
		  journal_inode_denylist_t *denylist)
{
  struct journal_entries list = { 0 };
  if (!fetch_and_validate_journal (arena, denylist, &list))
    {
      JOURNAL_LOG_ERROR ("Replay failed: could not validate journal.");
      return;
    }

  sort_entries (&list);
  size_t with_path = 0;
  size_t with_shadow_path = 0;
  for (size_t i = 0; i < list.count; ++i)
    {
      if (is_path_usable (list.entries[i]->path))
	with_path++;
      if (is_path_usable (list.entries[i]->shadow_path))
	with_shadow_path++;
      if (!journal_graph_add_event (list.entries[i], arena))
	{
	  JOURNAL_LOG_ERROR ("Graph construction failed for tx=%" PRIu64,
			     list.entries[i]->tx_id);
	  return;
	}
    }
  JOURNAL_LOG_DEBUG
    ("Out of %zu there are %zu entries with paths and %zu with shadow paths straight from journal.",
     list.count, with_path, with_shadow_path);

  replay_apply_graph (arena);
}

/**
 * journal_replay - Top-level entry point for journal replay.
 * 
 * This function initializes the arena, acquires fsys lock,
 * disables readonly mode, calls the core replay logic,
 * and finally restores the system state.
 */
void
journal_replay (journal_inode_denylist_t *denylist)
{
  struct journal_arena *arena = journal_arena_create (arena_size ());
  if (!arena)
    {
      JOURNAL_LOG_ERROR ("Failed to allocate replay arena. Replay aborted.");
      return;
    }

  if (pthread_rwlock_trywrlock (&diskfs_fsys_lock) != 0)
    {
      JOURNAL_LOG_ERROR ("Couldn't acquire fsys lock. Replay aborted.");
      journal_arena_destroy (arena);
      return;
    }

  if (diskfs_set_readonly (0))
    {
      JOURNAL_LOG_ERROR ("Failed to disable readonly mode. Replay aborted.");
      goto UNLOCK;
    }

  replay_main_pass (arena, denylist);

  if (diskfs_set_readonly (1))
    JOURNAL_LOG_ERROR ("Failed to enable readonly mode.");
  else
    JOURNAL_LOG_DEBUG ("Filesystem set back to readonly.");

UNLOCK:
  pthread_rwlock_unlock (&diskfs_fsys_lock);
  journal_arena_destroy (arena);
}
