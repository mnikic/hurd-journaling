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
#include <libdiskfs/crc32.h>
#include <libdiskfs/journal_graph.h>
#include <libdiskfs/journal_arena.h>
#include <libdiskfs/journal_io.h>
#include <libdiskfs/journal_inode_apply.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>

#define PAYLOAD_SIZE      (sizeof(struct journal_payload_bin))
#define PAYLOAD_PTR_SIZE  (sizeof(struct journal_payload_bin *))
#define ARENA_SIZE ((JOURNAL_NUM_ENTRIES * (PAYLOAD_SIZE + PAYLOAD_PTR_SIZE)) + 64)

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
      LOG_DEBUG ("Too many journal entries. Parsed %zu entries.",
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
  // Tie-breaker: lower tx_id wins
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

static void
test_inode_replay (void)
{
  inode_state_t test = {
    .ino = 123,
    .parent_ino = 0,
    .last_tx = 1,
    .last_seen = (uint64_t) time (NULL) * 1000,
    .link_count = 1,
    .is_deleted = false,
    .st_mode = S_IFREG | 0644,
    .st_size = 12345,
    .mtime = time (NULL),
    .ctime = time (NULL),
    .uid = 0,
    .gid = 0,
  };

  LOG_DEBUG ("About to sprintf,");
  snprintf (test.name, MAX_FIELD_LEN, "example.txt");
  test.resolved_path = strdup ("/tmp/example.txt");	// this will be written to /restore/tmp/example.txt

  LOG_DEBUG ("About to call apply state,");
  error_t err = apply_inode_state_hurd (&test);
  if (err)
    LOG_DEBUG ("apply_inode_state_hurd failed: %s", strerror (err));
  else
    LOG_DEBUG ("apply_inode_state_hurd succeeded");

  if (test.resolved_path)
    free (test.resolved_path);
}

/*
 * journal_replay_from_file - Loads and validates the journal,
 *                            sorts events, and builds the state graph.
 */
void
journal_replay_from_file (const char *path)
{
  LOG_DEBUG ("Toy journaling: Starting validation.");
  journal_enabled = false;
  test_inode_replay ();
  LOG_DEBUG ("Done testing");
  int fd = open (path, O_RDONLY);
  if (fd < 0)
    {
      LOG_DEBUG ("journal_replay_and_validate: open failed: %s",
		 strerror (errno));
      journal_enabled = true;
      return;
    }

  struct journal_header hdr = { 0 };
  if (!journal_read_and_validate_header (fd, &hdr))
    {
      close (fd);
      journal_enabled = true;
      return;
    }
  uint64_t index = hdr.start_index;
  uint64_t end_index = hdr.end_index;
  LOG_DEBUG ("header start index %llu and end index %llu", index, end_index);
  bool all_good = true;
  struct journal_arena *arena = journal_arena_create (ARENA_SIZE);
  if (!arena)
    {
      LOG_ERROR ("Out of memory!");
      journal_enabled = true;
      return;
    }
  struct journal_entries list = { 0 };
  list.entries =
    journal_arena_alloc (arena, JOURNAL_NUM_ENTRIES * PAYLOAD_PTR_SIZE);
  list.capacity = JOURNAL_NUM_ENTRIES;
  while (index != end_index)
    {
      struct journal_payload_bin *payload =
	journal_arena_alloc (arena, PAYLOAD_SIZE);
      if (!payload)
	{
	  LOG_ERROR ("Out of memory allocating payload at journal index %llu",
		     index);
	  goto CLEANUP;
	}
      if (!journal_read_and_validate_entry (fd, index, payload))
	{
	  all_good = false;
	  break;
	}
      if (strnlen (payload->action, sizeof (payload->action)) == 0)
	{
	  LOG_DEBUG ("action not valid on index %llu tx_id %llu action %s",
		     index, payload->tx_id, payload->action);
	  all_good = false;
	  break;
	}
      if (payload->ino == 0)
	{
	  LOG_DEBUG ("ino not valid on index %llu tx_id %llu ino = 0",
		     index, payload->tx_id);
	  all_good = false;
	  break;
	}
      if (!add_event_to_list (&list, payload))
	{
	  all_good = false;
	  break;
	}
      index = (index + 1) % JOURNAL_NUM_ENTRIES;
    }
  if (!all_good)
    {
      LOG_DEBUG ("Validation completed with errors.");
      goto CLEANUP;
    }
  // We have to sort by time for the graph!
  sort_entries (&list);
  LOG_DEBUG ("Validation completed successfully.");
  for (int i = 0; i < list.count; ++i)
    journal_graph_add_event (list.entries[i]);

  //journal_graph_print ();

  scan_directory_and_update_paths ();
  LOG_DEBUG ("Done filling things up. Ola la");
  LOG_DEBUG ("Done testing.");

  //LOG_DEBUG("Journaling reconstruct script:\n%s", journal_graph_emit_restore_script ());
  //TODO make the actual restore set of commands.
CLEANUP:
  journal_graph_free ();
  journal_arena_destroy (arena);
  close (fd);
  journal_enabled = true;
}
