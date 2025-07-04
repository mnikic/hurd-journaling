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

#include <libdiskfs/journal_writer.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_queue.h>
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
  uint64_t last_tx_id = 0;
  uint64_t last_timestamp = 0;
  char buf[JOURNAL_ENTRY_SIZE] = { 0 };
  bool all_good = true;

  while (index != end_index)
    {
      off_t offset = index_to_offset (index);
      if (pread (fd, buf, JOURNAL_ENTRY_SIZE, offset) != JOURNAL_ENTRY_SIZE)
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

      const struct journal_payload_bin *payload = &entry->payload;

      if (payload->timestamp_ms < last_timestamp)
	{
	  fprintf (stderr,
		   "journal replay: decreasing timestamp at offset %ld (index=%"
		   PRIu64 "): current=%" PRIu64 ", previous=%" PRIu64 "\n",
		   (long) offset, index, payload->timestamp_ms,
		   last_timestamp);
	  all_good = false;
	  break;
	}

      if ((payload->timestamp_ms > last_timestamp
	   && payload->tx_id <= last_tx_id)
	  || (payload->timestamp_ms < last_timestamp
	      && payload->tx_id >= last_tx_id))
	{
	  if (llabs ((int64_t) (payload->timestamp_ms - last_timestamp)) >
	      10000)
	    {
	      fprintf (stderr,
		       "journal replay: timestamp skew too large at offset %ld (index=%"
		       PRIu64 "): tx_id=%" PRIu64 ", previous=%" PRIu64
		       ", timestamp=%" PRIu64 ", previous_timestamp=%" PRIu64
		       "\n", (long) offset, index, payload->tx_id, last_tx_id,
		       payload->timestamp_ms, last_timestamp);
	      all_good = false;
	      break;
	    }
	  else
	    {
	      fprintf (stderr,
		       "journal replay: WARNING: non-monotonic tx_id or timestamp at offset %ld (index=%"
		       PRIu64 "): tx_id=%" PRIu64 ", previous=%" PRIu64
		       ", timestamp=%" PRIu64 ", previous_timestamp=%" PRIu64
		       "\n", (long) offset, index, payload->tx_id, last_tx_id,
		       payload->timestamp_ms, last_timestamp);
	    }
	}

      last_tx_id = payload->tx_id;
      last_timestamp = payload->timestamp_ms;

      fprintf (stderr,
	       "journal replay: tx_id=%" PRIu64 ", timestamp=%" PRIu64 "\n",
	       payload->tx_id, payload->timestamp_ms);

      index = (index + 1) % JOURNAL_NUM_ENTRIES;
    }

  if (all_good)
    fprintf (stderr,
	     "Toy journaling: Succesful validation of journal entries.\n");
  else
    fprintf (stderr, "Toy journaling: Validation completed with errors.\n");

  close (fd);
}
