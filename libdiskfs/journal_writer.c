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
#include <libdiskfs/journal_globals.h>
#include <libdiskfs/journal_replayer.h>
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

volatile size_t dropped_events = 0;
volatile bool journal_device_ready = false;
static pthread_mutex_t sync_write_lock = PTHREAD_MUTEX_INITIALIZER;
static int sync_fd = -1;

static int
get_sync_fd (void)
{
  if (sync_fd >= 0)
    {
      // Check if fd is still valid
      if (fcntl (sync_fd, F_GETFL) != -1)
	return sync_fd;

      // Stale or broken fd, close and reset
      close (sync_fd);
      sync_fd = -1;
    }

  sync_fd = open (RAW_DEVICE_PATH, O_RDWR);
  if (sync_fd < 0)
    LOG_ERROR ("get_sync_fd: open failed: %s", strerror (errno));

  return sync_fd;
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

      LOG_ERROR ("journal: header write failed, retrying (%d left): %s",
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
      LOG_ERROR ("journal_write_raw: cannot read journal file: %s",
		 strerror (errno));
      return false;
    }

  if (n != sizeof (hdr))
    {
      LOG_ERROR ("journal_write_raw: header read failed or missing");
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
      LOG_ERROR ("journal_write_raw: header CRC mismatch or invalid");
      *start_index = 0;
      *end_index = 0;
      return true;
    }

  if (hdr.start_index >= JOURNAL_NUM_ENTRIES
      || hdr.end_index >= JOURNAL_NUM_ENTRIES)
    {
      LOG_ERROR ("journal_write_raw: header indices out of bounds");
      *start_index = 0;
      *end_index = 0;
      return true;
    }

  *start_index = hdr.start_index;
  *end_index = hdr.end_index;

  LOG_DEBUG ("journal_write_raw: start_index=%llu, end_index=%llu",
	     *start_index, *end_index);

  return true;
}

static bool
journal_write_indexed (int fd, const char *data, size_t len,
		       uint64_t * end_index, uint64_t * start_index)
{
  if (len > sizeof (struct journal_payload_bin))
    {
      LOG_ERROR ("journal_write_indexed: payload too large: %zu bytes", len);
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
      LOG_ERROR ("journal_write_indexed: lseek failed: %s", strerror (errno));
      return false;
    }

  ssize_t written = write (fd, buf, JOURNAL_ENTRY_SIZE);
  if (written != JOURNAL_ENTRY_SIZE)
    {
      LOG_ERROR ("journal_write_indexed: write failed: %s", strerror (errno));
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
  if (!journal_device_ready)
    {
      LOG_ERROR ("Device not ready yet. Aborting.");
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
      LOG_ERROR ("journal_write_direct_sync: lseek failed: %s",
		 strerror (errno));
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }

  if (write (fd, buf, JOURNAL_ENTRY_SIZE) != JOURNAL_ENTRY_SIZE)
    {
      LOG_ERROR ("journal_write_direct_sync: write failed: %s",
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
      LOG_ERROR ("journal_write_direct_sync: failed to persist header");
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
      dropped_events += count;
      LOG_ERROR
	("journal_write_raw: failed to get fd. Dropped %zu txs now and %zu since the start.",
	 count, dropped_events);
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }
  if (!initialize_indices (fd, &start_index, &end_index))
    {
      dropped_events += count;
      LOG_ERROR
	("journal_write_raw: initialization failed. Dropped %zu txs now and %zu since the start.",
	 count, dropped_events);
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }

  static bool validation_done = false;
  if (!validation_done)
    {
      journal_replay_from_file (RAW_DEVICE_PATH);
      validation_done = true;
    }

  for (size_t i = 0; i < count; ++i)
    {
      if (entries[i].len != expected_len)
	{
	  LOG_ERROR ("journal_write_raw: unexpected payload size %zu",
		     entries[i].len);
	  dropped_events += count;
	  pthread_mutex_unlock (&sync_write_lock);
	  return false;
	}

      if (!journal_write_indexed (fd, entries[i].data, expected_len,
				  &end_index, &start_index))
	{
	  dropped_events += count;
	  LOG_ERROR
	    ("journal_write_raw: failed to write entry. Dropped %zu txs now and %zu since the start.",
	     count, dropped_events);
	  pthread_mutex_unlock (&sync_write_lock);
	  return false;
	}
    }

  if (!persist_header_with_retry (fd, start_index, end_index, 3))
    LOG_ERROR
      ("journal_write_raw: failed to persist updated header after retries.");

  LOG_ERROR ("Toy journaling: wrote %zu entries to raw disk.", count);

  pthread_mutex_unlock (&sync_write_lock);
  return true;
}
