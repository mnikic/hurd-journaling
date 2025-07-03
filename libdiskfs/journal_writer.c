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

#include <libdiskfs/journal_writer.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_queue.h>
#include <libdiskfs/crc32.h>
#include <hurd/fshelp.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

#define RAW_DEVICE_PATH "/tmp/journal-pipe"
#define RAW_DEVICE_SIZE (8 * 1024 * 1024)	/* 8MB */
#define JOURNAL_RESERVED_SPACE 4096	/* Leave room for future header growth */
#define JOURNAL_DATA_CAPACITY (RAW_DEVICE_SIZE - JOURNAL_RESERVED_SPACE)
#define JOURNAL_NUM_ENTRIES (JOURNAL_DATA_CAPACITY / JOURNAL_ENTRY_SIZE)

struct __attribute__((__packed__)) journal_header
{
  uint32_t magic;
  uint32_t version;
  uint64_t start_index;
  uint64_t end_index;
  uint32_t crc32;
};

struct __attribute__((__packed__)) journal_entry_bin
{
  uint32_t magic;
  uint32_t version;
  struct journal_payload_bin payload;
  uint8_t padding[JOURNAL_ENTRY_SIZE - sizeof (uint32_t) - sizeof (uint32_t) -
		  sizeof (struct journal_payload_bin) - sizeof (uint32_t)];
  uint32_t crc32;
};

static size_t dropped_txs = 0;
static bool replayed_once = false;
static pthread_mutex_t sync_write_lock = PTHREAD_MUTEX_INITIALIZER;
static int sync_fd = -1;

static int
get_sync_fd (void)
{
  fprintf (stderr, "In get_sync_id\n");
  if (sync_fd >= 0)
    {
      // Check if fd is still valid
      if (fcntl (sync_fd, F_GETFL) != -1)
	return sync_fd;

      // Stale or broken fd, close and reset
      close (sync_fd);
      sync_fd = -1;
    }

  fprintf (stderr, "About to open raw.\n");
  sync_fd = open (RAW_DEVICE_PATH, O_RDWR);
  fprintf (stderr, "Got %i as fd.\n", sync_fd);
  if (sync_fd < 0)
    fprintf (stderr, "get_sync_fd: open failed: %s\n", strerror (errno));

  return sync_fd;
}

static off_t
index_to_offset (uint64_t index)
{
  return JOURNAL_RESERVED_SPACE +
    (index % JOURNAL_NUM_ENTRIES) * JOURNAL_ENTRY_SIZE;
}

static void
journal_replay_and_validate (void)
{
  fprintf (stderr, "Toy journaling: Starting validation.\n");
  int fd = open (RAW_DEVICE_PATH, O_RDONLY);
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
	  fprintf (stderr,
		   "journal replay: version mismatch at offset %ld\n",
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
		       ", timestamp=%" PRIu64 ", previous_timestamp=%"
		       PRIu64 "\n", (long) offset, index, payload->tx_id,
		       last_tx_id, payload->timestamp_ms, last_timestamp);
	      all_good = false;
	      break;
	    }
	  else
	    {
	      fprintf (stderr,
		       "journal replay: WARNING: non-monotonic tx_id or timestamp at offset %ld (index=%"
		       PRIu64 "): tx_id=%" PRIu64 ", previous=%" PRIu64
		       ", timestamp=%" PRIu64 ", previous_timestamp=%"
		       PRIu64 "\n", (long) offset, index, payload->tx_id,
		       last_tx_id, payload->timestamp_ms, last_timestamp);
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

static bool
persist_header_with_retry (int fd, uint64_t start_index,
			   uint64_t end_index, int retries)
{
  struct journal_header hdr = {
    .magic = JOURNAL_MAGIC,
    .version = JOURNAL_VERSION,
    .start_index = start_index,
    .end_index = end_index,
    .crc32 = 0,
  };

  hdr.crc32 = crc32 ((const void *) &hdr, sizeof (hdr));

  while (retries-- > 0)
    {
      if (pwrite (fd, &hdr, sizeof (hdr), 0) == sizeof (hdr))
	{
	  fsync (fd);
	  return true;
	}

      fprintf (stderr,
	       "journal: header write failed, retrying (%d left): %s\n",
	       retries, strerror (errno));
      usleep (1000);
    }

  return false;
}

static bool
initialize_indices (int fd, uint64_t * start_index, uint64_t * end_index)
{
  struct journal_header hdr = { 0 };
  ssize_t n = pread (fd, &hdr, sizeof (hdr), 0);

  if (n == -1 && errno == EIO)
    {
      fprintf (stderr,
	       "journal_write_raw: cannot read journal file: %s\n",
	       strerror (errno));
      return false;
    }

  if (n != sizeof (hdr))
    {
      fprintf (stderr, "journal_write_raw: header read failed or missing\n");
      *start_index = 0;
      *end_index = 0;
      return true;
    }

  uint32_t expected_crc = hdr.crc32;
  hdr.crc32 = 0;
  uint32_t actual_crc = crc32 ((const void *) &hdr, sizeof (hdr));

  if (actual_crc != expected_crc
      || hdr.magic != JOURNAL_MAGIC || hdr.version != JOURNAL_VERSION)
    {
      fprintf (stderr, "journal_write_raw: header CRC mismatch or invalid\n");
      *start_index = 0;
      *end_index = 0;
      return true;
    }

  if (hdr.start_index >= JOURNAL_NUM_ENTRIES
      || hdr.end_index >= JOURNAL_NUM_ENTRIES)
    {
      fprintf (stderr, "journal_write_raw: header indices out of bounds\n");
      *start_index = 0;
      *end_index = 0;
      return true;
    }

  *start_index = hdr.start_index;
  *end_index = hdr.end_index;

  fprintf (stderr,
	   "journal_write_raw: start_index=%llu, end_index=%llu\n",
	   *start_index, *end_index);

  return true;
}

static bool
journal_write_indexed (int fd, const char *data, size_t len,
		       uint64_t * end_index, uint64_t * start_index)
{
  if (len > sizeof (struct journal_payload_bin))
    {
      fprintf (stderr,
	       "journal_write_indexed: payload too large: %zu bytes\n", len);
      return false;
    }

  uint64_t next = (*end_index + 1) % JOURNAL_NUM_ENTRIES;
  if (next == *start_index)
    *start_index = (*start_index + 1) % JOURNAL_NUM_ENTRIES;

  char buf[JOURNAL_ENTRY_SIZE] = { 0 };
  struct journal_entry_bin *entry = (struct journal_entry_bin *) buf;

  entry->magic = JOURNAL_MAGIC;
  entry->version = JOURNAL_VERSION;

  // Copy only into the payload section
  memcpy (&entry->payload, data, len);

  entry->crc32 = 0;		// Ensure zero before calculating
  entry->crc32 =
    crc32 ((const char *) &entry->payload,
	   sizeof (struct journal_payload_bin));

  off_t offset = index_to_offset (*end_index);
  if (lseek (fd, offset, SEEK_SET) == (off_t) - 1)
    {
      fprintf (stderr,
	       "journal_write_indexed: lseek failed: %s\n", strerror (errno));
      return false;
    }

  ssize_t written = write (fd, buf, JOURNAL_ENTRY_SIZE);
  if (written != JOURNAL_ENTRY_SIZE)
    {
      fprintf (stderr,
	       "journal_write_indexed: write failed: %s\n", strerror (errno));
      return false;
    }

  *end_index = next;
  return true;
}

bool
journal_write_raw_sync (struct journal_payload_bin *payload)
{
  pthread_mutex_lock (&sync_write_lock);
  // Dirty hack to avoid blocking in the early boot
  if (!replayed_once)
    {
      fprintf (stderr, "Device not ready yet. Aborting.\n");
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }
  int fd = get_sync_fd ();
  if (fd < 0)
    {
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }

  uint64_t start_index = 0, end_index = 0;
  if (!initialize_indices (fd, &start_index, &end_index))
    {
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }

  char buf[JOURNAL_ENTRY_SIZE] = { 0 };
  struct journal_entry_bin *entry = (struct journal_entry_bin *) buf;

  entry->magic = JOURNAL_MAGIC;
  entry->version = JOURNAL_VERSION;
  memcpy (&entry->payload, payload, sizeof (struct journal_payload_bin));
  entry->crc32 = 0;
  entry->crc32 =
    crc32 ((const char *) &entry->payload,
	   sizeof (struct journal_payload_bin));

  off_t offset = index_to_offset (end_index);
  if (lseek (fd, offset, SEEK_SET) == (off_t) - 1)
    {
      fprintf (stderr, "journal_write_direct_sync: lseek failed: %s",
	       strerror (errno));
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }

  if (write (fd, buf, JOURNAL_ENTRY_SIZE) != JOURNAL_ENTRY_SIZE)
    {
      fprintf (stderr, "journal_write_direct_sync: write failed: %s",
	       strerror (errno));
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }

  fsync (fd);

  uint64_t next_index = (end_index + 1) % JOURNAL_NUM_ENTRIES;
  if (next_index == start_index)
    start_index = (start_index + 1) % JOURNAL_NUM_ENTRIES;
  if (!persist_header_with_retry (fd, start_index, next_index, 3))
    {
      fprintf (stderr, "journal_write_direct_sync: failed to persist header");
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }
  fsync (fd);

  pthread_mutex_unlock (&sync_write_lock);
  return true;
}

bool
journal_write_raw (const struct journal_payload *entries, size_t count)
{
  pthread_mutex_lock (&sync_write_lock);

  uint64_t end_index = 0;
  uint64_t start_index = 0;
  const size_t expected_len = sizeof (struct journal_payload_bin);

  int fd = get_sync_fd ();
  if (fd < 0)
    {
      dropped_txs += count;
      fprintf (stderr,
	       "journal_write_raw: failed to get fd. Dropped %zu txs now and %zu since the start.",
	       count, dropped_txs);
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }
  if (!initialize_indices (fd, &start_index, &end_index))
    {
      dropped_txs += count;
      fprintf (stderr,
	       "journal_write_raw: initialization failed. Dropped %zu txs now and %zu since the start.",
	       count, dropped_txs);
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }

  if (!replayed_once)
    {
      journal_replay_and_validate ();
      replayed_once = true;
    }

  for (size_t i = 0; i < count; ++i)
    {
      if (entries[i].len != expected_len)
	{
	  fprintf (stderr, "journal_write_raw: unexpected payload size %zu",
		   entries[i].len);
	  dropped_txs += count;
	  pthread_mutex_unlock (&sync_write_lock);
	  return false;
	}

      if (!journal_write_indexed (fd, entries[i].data, expected_len,
				  &end_index, &start_index))
	{
	  dropped_txs += count;
	  fprintf (stderr,
		   "journal_write_raw: failed to write entry. Dropped %zu txs now and %zu since the start.",
		   count, dropped_txs);
	  pthread_mutex_unlock (&sync_write_lock);
	  return false;
	}
    }

  if (!persist_header_with_retry (fd, start_index, end_index, 3))
    fprintf (stderr,
	     "journal_write_raw: failed to persist updated header after retries.");

  fprintf (stderr, "Toy journaling: wrote %zu entries to raw disk.", count);

  pthread_mutex_unlock (&sync_write_lock);
  return true;
}
