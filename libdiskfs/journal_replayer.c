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
#include <libdiskfs/journal_util.h>
#include <libdiskfs/crc32.h>
#include <libdiskfs/diskfs.h>
#include <libdiskfs/journal_writer.h>
#include <libdiskfs/journal_graph.h>
#include <libdiskfs/journal_arena.h>
#include <libdiskfs/journal_io.h>
#include <libdiskfs/journal_fs_helper.h>
#include <libdiskfs/journal_apply.h>
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

static inline size_t
arena_size (void)
{
  return ALIGN_UP (journal_layout.num_entries *
		   (ENTRY_SIZE + PAYLOAD_PTR_SIZE + GRAPH_NODE_SIZE +
		    REPLAY_STATE_SIZE +
		    (AVG_STRINGS_PER_ENTRY * AVG_STRING_SIZE)), 8) * 1.5;
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
 * out may point to garbage in case of error. Do not use in that case.
 */
static bool
fetch_and_validate_header (journal_header_t * out)
{
  error_t err = journal_read_header (out);
  if (err)
    {
      JOURNAL_LOG_ERROR ("journal_node_read failed reading header: %d", err);
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
      JOURNAL_LOG_DEBUG ("journal_node_read: header indices out of bounds.");
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
fetch_and_validate_entry (uint64_t index, journal_entry_bin_t * out)
{
  error_t err = journal_read_entry (out, index);
  if (err)
    {
      JOURNAL_LOG_DEBUG ("journal_node_read failed at index %llu.", index);
      return false;
    }
  if (out->magic != JOURNAL_MAGIC)
    {
      JOURNAL_LOG_DEBUG ("Bad journal entry magic at index %llu", index);
      return false;
    }

  if (out->version != JOURNAL_VERSION)
    {
      JOURNAL_LOG_DEBUG ("Journal entry version mismatch at index %llu",
			 index);
      return false;
    }

  uint32_t actual_entry_crc = journal_compute_payload_crc32 (&out->payload);
  if (actual_entry_crc != out->crc32)
    {
      JOURNAL_LOG_DEBUG ("Journal entry CRC mismatch at index %llu.", index);
      return false;
    }

  return true;
}

static bool
add_event_to_list (struct journal_entries *list,
		   journal_payload_bin_t * payload)
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
  journal_header_t *hdr =
    journal_arena_alloc (arena, sizeof (journal_header_t));

  if (!hdr)
    return false;
  memset (hdr, 0, sizeof (*hdr));

  if (!fetch_and_validate_header (hdr))
    return false;

  JOURNAL_LOG_DEBUG ("Header: start index %llu, end index %llu",
		     hdr->start_index, hdr->end_index);

  out_entries->count = 0;
  out_entries->entries =
    journal_arena_alloc (arena,
			 journal_layout.num_entries * PAYLOAD_PTR_SIZE);
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
      journal_entry_bin_t *entry = journal_arena_alloc (arena, ENTRY_SIZE);
      if (!entry)
	{
	  JOURNAL_LOG_ERROR ("Out of memory allocating payload at index %llu",
			     index);
	  return false;
	}
      if (!fetch_and_validate_entry (index, entry))
	{
	  JOURNAL_LOG_ERROR
	    ("CRC check failed or corrupted payload at index %llu", index);
	  return false;
	}
      journal_payload_bin_t *payload = &entry->payload;
      if (payload->action == JOURNAL_ACTION_UNKNOWN || payload->ino == 0
	  || payload->tx_id == 0 || payload->timestamp_ms == 0
	  || !(payload->has_mtime || payload->has_atime
	       || payload->has_ctime))
	{
	  JOURNAL_LOG_ERROR
	    ("Invalid entry: action=%u ino=%u at index %llu (tx_id %llu)",
	     payload->action, payload->ino, index, payload->tx_id);
	  return false;
	}
      JOURNAL_LOG_DEBUG ("Entry: ino=%u, action=%u, tx_id = %llu, name=%s, path=%s", payload->ino, payload->action, payload->tx_id, payload->name, payload->path);
      if (!add_event_to_list (out_entries, payload))
	{
	  return false;
	}
      index = (index + 1) % journal_layout.num_entries;
    }

  return true;
}

static void
test (struct journal_arena *arena)
{
  JOURNAL_LOG_DEBUG ("TESTING: Starting.");
  journal_payload_bin_t *payload =
    journal_arena_alloc (arena, sizeof (journal_payload_bin_t));
  payload->ino = 300001;
  payload->mtime = 1788211200;
  payload->has_mtime = true;
  payload->ctime = 1788211200;
  payload->has_ctime = true;
  payload->st_mode = 0100755;
  payload->has_mode = true;
  payload->tx_id = 7113;
  payload->timestamp_ms = time (NULL) + 60;
  payload->uid = 0;
  payload->has_uid = true;
  payload->gid = 0;
  payload->has_gid = true;
  payload->action = JOURNAL_ACTION_CHOWN;
  payload->st_nlink = 12;
  strncpy (payload->path,
	   "/home/loshmi/nonexisting/dir/andanewfile123.txt",
	   sizeof (payload->path));
  payload->path[sizeof (payload->path) - 1] = '\0';

  journal_payload_bin_t *payload1 =
    journal_arena_alloc (arena, sizeof (journal_payload_bin_t));
  payload1->ino = 300001;
  payload1->mtime = 1788211210;
  payload1->has_mtime = true;
  payload1->ctime = 1788211210;
  payload1->has_ctime = true;
  payload1->st_mode = 0100644;
  payload1->has_mode = true;
  payload1->tx_id = 7115;
  payload1->timestamp_ms = time (NULL) + 90;
  payload1->uid = 0;
  payload1->has_uid = true;
  payload1->gid = 0;
  payload1->has_gid = true;
  payload1->action = JOURNAL_ACTION_CHOWN;

  // crudical piece of data!!!!!
  payload1->st_nlink = 0;

  strncpy (payload1->path,
	   "/home/loshmi/nonexisting/dir/andanewfile123.txt",
	   sizeof (payload->path));
  payload1->path[sizeof (payload1->path) - 1] = '\0';

  if (!journal_write_raw_sync (payload))
    JOURNAL_LOG_DEBUG ("TESTING: Didn't manage to write for some reason");
  else
    JOURNAL_LOG_DEBUG ("TESTING: Payload inserted.");
  if (!journal_write_raw_sync (payload1))
    JOURNAL_LOG_DEBUG ("TESTING: Didn't manage to write for some reason");
  else
    JOURNAL_LOG_DEBUG ("TESTING: Payload 1 inserted.");

}

/*
 * journal_replay_from_file - Main entry point for replaying the journal.
 * Reconstructs inode graph and applies metadata changes in early boot.
 */
void
journal_replay (void)
{
  JOURNAL_LOG_DEBUG ("Starting journal validation.");
  struct journal_arena *arena = journal_arena_create (arena_size ());
  if (!arena)
    {
      JOURNAL_LOG_ERROR
	("Unable to allocate enough memory for journal replay. Aborting!");
      // Even if replay fails, enable journaling to start capturing future metadata
      return;
    }

  if (pthread_rwlock_trywrlock (&diskfs_fsys_lock) == 0)
    {
      error_t err = diskfs_set_readonly (0);
      if (err)
	JOURNAL_LOG_ERROR ("Failed to set diskfs_readonly = 0: %s (%d)",
			   strerror (err), err);
      else
	JOURNAL_LOG_DEBUG ("Filesystem NOT in readonly mode now!");
      //test (arena);
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
	journal_graph_add_event (list.entries[i], arena);

      inode_replay_state_t **entries;
      size_t count = journal_graph_get_all (&entries, arena);

      JOURNAL_LOG_DEBUG ("Starting restoration of metadata.");

      struct protid *cred = NULL;
      struct node *root = diskfs_root_node;
      diskfs_nref (root);

      err = diskfs_create_creds (root, O_READ | O_EXEC | O_WRITE, &cred);
      if (err)
	{
	  JOURNAL_LOG_ERROR
	    ("Aborting replay. Couldn't create root credentials due to an error: %s. No entries replayed.",
	     strerror (err));
	  goto DEREF;
	}

      JOURNAL_LOG_DEBUG ("Got %u entries to relay.", count);

      for (size_t i = 0; i < count; ++i)
	{
	  err = apply_node_replay (entries[i], root, cred);
	  if (err)
	    JOURNAL_LOG_ERROR ("Error while restoring node: %u. Error: %s.",
			       entries[i]->ino, strerror (err));
	}
      JOURNAL_LOG_DEBUG ("Done with restoration.");

      ports_port_deref (cred);
    DEREF:
      diskfs_nput (root);
    CLEANUP:
      diskfs_sync_everything (1);
      diskfs_set_hypermetadata (1, 1);
      _diskfs_diskdirty = 0;
      err = diskfs_set_readonly (1);
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
  journal_graph_free ();
  journal_arena_destroy (arena);
}
