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
#define RAW_DEVICE_SIZE (8 * 1024 * 1024) /* 8MB */
#define JOURNAL_RESERVED_SPACE 4096 /* Leave room for future header growth */
#define JOURNAL_DATA_CAPACITY (RAW_DEVICE_SIZE - JOURNAL_RESERVED_SPACE)
#define JOURNAL_NUM_ENTRIES (JOURNAL_DATA_CAPACITY / JOURNAL_ENTRY_SIZE)

struct __attribute__ ((__packed__)) journal_header
{
  uint32_t magic;
  uint32_t version;
  uint64_t start_index;
  uint64_t end_index;
  uint32_t crc32;
};

static size_t dropped_txs = 0;
static bool replayed_once = false;

static off_t
index_to_offset (uint64_t index)
{
  return JOURNAL_RESERVED_SPACE + (index % JOURNAL_NUM_ENTRIES) * JOURNAL_ENTRY_SIZE;
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
      || hdr.magic != JOURNAL_MAGIC
      || hdr.version != JOURNAL_VERSION)
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

      if (crc32 (buf, JOURNAL_ENTRY_SIZE) != stored_crc)
        {
          fprintf (stderr, "journal replay: CRC mismatch at offset %ld\n",
                   (long) offset);
          all_good = false;
          break;
        }

      if (entry->timestamp_ms < last_timestamp)
        {
          fprintf (stderr,
                   "journal replay: decreasing timestamp at offset %ld (index=%" PRIu64 "): current=%" PRIu64 ", previous=%" PRIu64 "\n",
                   (long) offset, index, entry->timestamp_ms, last_timestamp);
          all_good = false;
          break;
        }

      if ((entry->timestamp_ms > last_timestamp && entry->tx_id <= last_tx_id)
          || (entry->timestamp_ms < last_timestamp
              && entry->tx_id >= last_tx_id))
        {
          if (llabs ((int64_t) (entry->timestamp_ms - last_timestamp)) > 10000)
            {
              fprintf (stderr,
                       "journal replay: timestamp skew too large at offset %ld (index=%" PRIu64 "): tx_id=%" PRIu64 ", previous=%" PRIu64 ", timestamp=%" PRIu64 ", previous_timestamp=%" PRIu64 "\n",
                       (long) offset, index, entry->tx_id, last_tx_id,
                       entry->timestamp_ms, last_timestamp);
              all_good = false;
              break;
            }
          else
            {
              fprintf (stderr,
                       "journal replay: WARNING: non-monotonic tx_id or timestamp at offset %ld (index=%" PRIu64 "): tx_id=%" PRIu64 ", previous=%" PRIu64 ", timestamp=%" PRIu64 ", previous_timestamp=%" PRIu64 "\n",
                       (long) offset, index, entry->tx_id, last_tx_id,
                       entry->timestamp_ms, last_timestamp);
            }
        }

      last_tx_id = entry->tx_id;
      last_timestamp = entry->timestamp_ms;

      fprintf (stderr, "journal replay: tx_id=%" PRIu64 ", timestamp=%" PRIu64 "\n",
               entry->tx_id, entry->timestamp_ms);

      index = (index + 1) % JOURNAL_NUM_ENTRIES;
    }

  if (all_good)
    fprintf (stderr, "Toy journaling: Succesful validation of journal entries.\n");
  else
    fprintf (stderr, "Toy journaling: Validation completed with errors.\n");

  close (fd);
}

static bool
persist_header_with_retry (int fd, uint64_t start_index,
                           uint64_t end_index, int retries)
{
  struct journal_header hdr =
    {
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
initialize_indices (int fd, uint64_t *start_index, uint64_t *end_index)
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
      fprintf (stderr,
               "journal_write_raw: header read failed or missing\n");
      *start_index = 0;
      *end_index = 0;
      return true;
    }

  uint32_t expected_crc = hdr.crc32;
  hdr.crc32 = 0;
  uint32_t actual_crc = crc32 ((const void *) &hdr, sizeof (hdr));

  if (actual_crc != expected_crc
      || hdr.magic != JOURNAL_MAGIC
      || hdr.version != JOURNAL_VERSION)
    {
      fprintf (stderr,
               "journal_write_raw: header CRC mismatch or invalid\n");
      *start_index = 0;
      *end_index = 0;
      return true;
    }

  if (hdr.start_index >= JOURNAL_NUM_ENTRIES
      || hdr.end_index >= JOURNAL_NUM_ENTRIES)
    {
      fprintf (stderr,
               "journal_write_raw: header indices out of bounds\n");
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
                       uint64_t *end_index, uint64_t *start_index)
{
  if (len > JOURNAL_ENTRY_SIZE)
    {
      fprintf (stderr,
               "journal_write_indexed: entry too large: %zu bytes\n", len);
      return false;
    }

  uint64_t next = (*end_index + 1) % JOURNAL_NUM_ENTRIES;
  if (next == *start_index)
    *start_index = (*start_index + 1) % JOURNAL_NUM_ENTRIES;

  char buf[JOURNAL_ENTRY_SIZE] = { 0 };
  memcpy (buf, data, len);

  struct journal_entry_bin *entry = (struct journal_entry_bin *) buf;
  entry->crc32 = 0;
  entry->crc32 = crc32 (buf, JOURNAL_ENTRY_SIZE);

  off_t offset = index_to_offset (*end_index);
  if (lseek (fd, offset, SEEK_SET) == (off_t) -1)
    {
      fprintf (stderr,
               "journal_write_indexed: lseek failed: %s\n",
               strerror (errno));
      return false;
    }

  ssize_t written = write (fd, buf, JOURNAL_ENTRY_SIZE);
  if (written != JOURNAL_ENTRY_SIZE)
    {
      fprintf (stderr,
               "journal_write_indexed: write failed: %s\n",
               strerror (errno));
      return false;
    }

  *end_index = next;
  return true;
}

bool
journal_write_raw (const struct journal_payload *entries, size_t count)
{
  static uint64_t end_index = 0;
  static uint64_t start_index = 0;
  static bool offset_initialized = false;

  int fd = open (RAW_DEVICE_PATH, O_RDWR);
  if (fd < 0)
    {
      dropped_txs += count;
      fprintf (stderr,
               "journal_write_raw: open failed: %s. Dropped %zu txs now and %zu since the start.\n",
               strerror (errno), count, dropped_txs);
      return false;
    }

  if (!offset_initialized)
    {
      if (!initialize_indices (fd, &start_index, &end_index))
        {
          dropped_txs += count;
          fprintf (stderr,
                   "journal_write_raw: initialization failed. Dropped %zu txs now and %zu since the start.\n",
                   count, dropped_txs);
          close (fd);
          return false;
        }

      //offset_initialized = true;

      if (!replayed_once)
        {
          journal_replay_and_validate ();
          replayed_once = true;
        }
    }

  for (size_t i = 0; i < count; ++i)
    {
      if (!journal_write_indexed (fd, entries[i].data, entries[i].len,
                                   &end_index, &start_index))
        {
          dropped_txs += count;
          fprintf (stderr,
                   "journal_write_raw: failed to write entry. Dropped %zu txs now and %zu since the start.\n",
                   count, dropped_txs);
          close (fd);
          return false;
        }
    }

  if (!persist_header_with_retry (fd, start_index, end_index, 3))
    fprintf (stderr,
             "journal_write_raw: failed to persist updated header after retries.\n");

  fprintf (stderr, "Toy journaling: wrote %zu entries to raw disk.\n", count);
  close (fd);
  return true;
}

