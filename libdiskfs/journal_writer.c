/* journal_writer.c - Raw journal writer for GNU Hurd journaling

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

#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libdiskfs/journal_internal.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_globals.h>
#include <libdiskfs/journal_replayer.h>
#include <libdiskfs/journal_util.h>
#include <libdiskfs/journal_io.h>
#include <libdiskfs/journal_writer.h>

volatile size_t journal_dropped_events = 0;
static pthread_mutex_t sync_write_lock = PTHREAD_MUTEX_INITIALIZER;

static bool
persist_header_with_retry (uint64_t start_index,
			   uint64_t end_index, int retries)
{
  struct journal_header hdr = {
    .magic = JOURNAL_MAGIC,
    .version = JOURNAL_VERSION,
    .start_index = start_index,
    .end_index = end_index,
    .crc32 = 0,
  };
  hdr.crc32 = journal_compute_header_crc32 (&hdr);

  while (retries-- > 0)
    {
      error_t err = journal_write_header (&hdr);
      if (!err)
	return true;

      JOURNAL_LOG_ERROR
	("journal: header write failed, retrying (%d left): %s", retries,
	 strerror (err));
      usleep (1000);
    }

  return false;
}

static bool
initialize_indices (uint64_t * start_index, uint64_t * end_index)
{
  journal_header_t hdr = { 0 };
  error_t err = journal_read_header (&hdr);
  if (err != 0)
    {
      JOURNAL_LOG_ERROR ("journal_write_raw: header read failed or missing");
      *start_index = 0;
      *end_index = 0;
      return true;		// Allow system to start fresh
    }

  if (hdr.crc32 != journal_compute_header_crc32 (&hdr) ||
      hdr.magic != JOURNAL_MAGIC || hdr.version != JOURNAL_VERSION)
    {
      JOURNAL_LOG_ERROR ("journal_write_raw: header CRC mismatch or invalid");
      *start_index = 0;
      *end_index = 0;
      return true;
    }

  if (hdr.start_index >= journal_layout.num_entries ||
      hdr.end_index >= journal_layout.num_entries)
    {
      JOURNAL_LOG_ERROR ("journal_write_raw: header indices out of bounds");
      *start_index = 0;
      *end_index = 0;
      return true;
    }

  *start_index = hdr.start_index;
  *end_index = hdr.end_index;

  JOURNAL_LOG_DEBUG ("journal_write_raw: start_index=%" PRIu64 ", end_index=%"
		     PRIu64, *start_index, *end_index);
  return true;
}

bool
journal_write (const journal_payload_bin_t * payload_bin)
{
  pthread_mutex_lock (&sync_write_lock);

  uint64_t start_index = 0, end_index = 0;
  if (!initialize_indices (&start_index, &end_index))
    {
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }

  const journal_entry_bin_t entry = {
    .magic = JOURNAL_MAGIC,
    .version = JOURNAL_VERSION,
    .payload = *payload_bin,
    .crc32 = journal_compute_payload_crc32 (payload_bin)
  };
  error_t err = journal_write_entry (&entry, end_index);
  if (err)
    {
      JOURNAL_LOG_ERROR
	("journal_write_raw_sync: outside of the fs entry write failed: %s",
	 strerror (err));
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }

  uint64_t next_index = (end_index + 1) % journal_layout.num_entries;
  if (next_index == start_index)
    start_index = (start_index + 1) % journal_layout.num_entries;

  if (!persist_header_with_retry (start_index, next_index, 3))
    {
      JOURNAL_LOG_ERROR ("journal_write_raw_sync: failed to persist header");
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }

  pthread_mutex_unlock (&sync_write_lock);
  return true;
}
