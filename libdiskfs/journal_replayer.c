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
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>

struct journal_entries
{
  struct journal_payload_bin **entries;
  size_t count;
  size_t capacity;
};

static void
add_event_to_global_list (struct journal_entries *list,
			  struct journal_payload_bin *entry)
{
  if (list->count == list->capacity)
    {
      size_t new_capacity = list->capacity == 0 ? 128 : list->capacity * 2;
      list->entries =
	realloc (list->entries, new_capacity * sizeof (*list->entries));
      list->capacity = new_capacity;
    }
  list->entries[list->count++] = entry;
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
sort_global_entries (struct journal_entries *list)
{
  qsort (list->entries, list->count,
	 sizeof (struct journal_payload_bin *),
	 compare_entries_by_time_then_txid);
}

void
journal_replay_from_file (const char *path)
{
  fprintf (stderr, "Toy journaling: Starting validation.\n");
  int fd = open (path, O_RDONLY);
  if (fd < 0)
    {
      fprintf (stderr, "journal_replay_and_validate: open failed: %s\n",
	       strerror (errno));
      return;
    }

  struct journal_header hdr = { 0 };
  ssize_t n = pread (fd, &hdr, sizeof (hdr), 0);
  if (n != sizeof (hdr))
    {
      fprintf (stderr, "journal replay: could not read journal header\n");
      close (fd);
      return;
    }

  uint32_t expected_crc = hdr.crc32;
  hdr.crc32 = 0;
  uint32_t actual_crc = crc32 ((const void *) &hdr, sizeof (hdr));
  if (actual_crc != expected_crc
      || hdr.magic != JOURNAL_MAGIC || hdr.version != JOURNAL_VERSION)
    {
      fprintf (stderr, "journal replay: header invalid\n");
      close (fd);
      return;
    }

  if (hdr.start_index >= JOURNAL_NUM_ENTRIES
      || hdr.end_index >= JOURNAL_NUM_ENTRIES)
    {
      fprintf (stderr, "journal_write_raw: header indices out of bounds\n");
      close (fd);
      return;
    }

  uint64_t index = hdr.start_index;
  uint64_t end_index = hdr.end_index;
  LOG_DEBUG ("header start index %llu and end index %llu", index, end_index);
  char buf[JOURNAL_ENTRY_SIZE] = { 0 };
  bool all_good = true;
  struct journal_entries list = { 0 };
  while (index != end_index)
    {
      uint64_t offset = index_to_offset (index);
      if (pread (fd, buf, JOURNAL_ENTRY_SIZE, (off_t) offset) !=
	  JOURNAL_ENTRY_SIZE)
	{
	  fprintf (stderr,
		   "journal replay: incomplete read at offset %ld\n",
		   (long) offset);
	  break;
	}

      struct journal_entry_bin *entry = (struct journal_entry_bin *) buf;

      if (entry->magic != JOURNAL_MAGIC)
	{
	  fprintf (stderr, "journal replay: bad magic at offset %ld\n",
		   (long) offset);
	  all_good = false;
	  break;
	}

      if (entry->version != JOURNAL_VERSION)
	{
	  fprintf (stderr, "journal replay: version mismatch at offset %ld\n",
		   (long) offset);
	  all_good = false;
	  break;
	}
      uint32_t stored_crc = entry->crc32;
      entry->crc32 = 0;
      uint32_t actual_entry_crc = crc32 ((const char *) &entry->payload,
					 sizeof (struct journal_payload_bin));
      if (actual_entry_crc != stored_crc)
	{
	  fprintf (stderr, "journal replay: CRC mismatch at offset %ld\n",
		   (long) offset);
	  all_good = false;
	  break;
	}

      struct journal_payload_bin *payload = &entry->payload;
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
      struct journal_payload_bin *payload_copy =
	malloc (sizeof *payload_copy);
      if (!payload_copy)
	{
	  LOG_DEBUG ("Out of memory");
	  goto CLEANUP;
	}
      memcpy (payload_copy, payload, sizeof *payload_copy);
      LOG_DEBUG ("index: %" PRIu64 ", tx_id: %" PRIu64 ", timestamp: %"
		 PRIu64 ", ino: %u, action: %s", index, payload->tx_id,
		 payload->timestamp_ms, payload->ino, payload->action);
      add_event_to_global_list (&list, payload_copy);
      index = (index + 1) % JOURNAL_NUM_ENTRIES;
    }
  if (!all_good)
    {
      LOG_DEBUG ("Validation completed with errors.");
      goto CLEANUP;
    }
  LOG_DEBUG ("Validation completed successfully.");
  sort_global_entries (&list);

CLEANUP:
  for (size_t i = 0; i < list.count; i++)
    free (list.entries[i]);
  if (list.entries)
    free (list.entries);
  close (fd);
}
