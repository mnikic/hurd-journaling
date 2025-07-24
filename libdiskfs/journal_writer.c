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

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_globals.h>
#include <libdiskfs/journal_queue.h>
#include <libdiskfs/journal_replayer.h>
#include <libdiskfs/journal_util.h>
#include <libdiskfs/journal_io.h>
#include <libdiskfs/journal_writer.h>
#include <libdiskfs/crc32.h>

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
initialize_indices (uint64_t * start_index,  uint64_t * end_index)
{
  struct journal_header hdr = { 0 };
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

  if (hdr.start_index >= JOURNAL_NUM_ENTRIES ||
      hdr.end_index >= JOURNAL_NUM_ENTRIES)
    {
      JOURNAL_LOG_ERROR ("journal_write_raw: header indices out of bounds");
      *start_index = 0;
      *end_index = 0;
      return true;
    }

  *start_index = hdr.start_index;
  *end_index = hdr.end_index;

  JOURNAL_LOG_DEBUG ("journal_write_raw: start_index=%llu, end_index=%llu",
		     *start_index, *end_index);

  return true;
}

static bool
journal_write_indexed (struct node *np, const char *data, size_t len,
		       uint64_t * end_index, uint64_t * start_index)
{
  if (len > sizeof (journal_payload_bin_t))
    {
      JOURNAL_LOG_ERROR
	("journal_write_indexed: payload too large: %zu bytes", len);
      return false;
    }

  uint64_t next = (*end_index + 1) % JOURNAL_NUM_ENTRIES;
  if (next == *start_index)
    *start_index = (*start_index + 1) % JOURNAL_NUM_ENTRIES;

  char buf[JOURNAL_ENTRY_SIZE] = { 0 };
  struct journal_entry_bin *entry = (struct journal_entry_bin *) buf;

  entry->magic = JOURNAL_MAGIC;
  entry->version = JOURNAL_VERSION;
  memcpy (&entry->payload, data, len);

  entry->crc32 = 0;
  entry->crc32 = crc32 ((const char *) &entry->payload,
			sizeof (journal_payload_bin_t));
  JOURNAL_LOG_DEBUG ("Got to here");
  off_t offset = index_to_offset (*end_index);
  error_t err = journal_node_write (np, offset, buf, JOURNAL_ENTRY_SIZE);

  if (err)
    {
      JOURNAL_LOG_ERROR
	("journal_write_indexed: journal_node_write failed: %s (%d)",
	 strerror (err), err);
      return false;
    }

  *end_index = next;
  return true;
}

bool
journal_write_raw_sync (journal_payload_bin_t *payload)
{
  pthread_mutex_lock (&sync_write_lock);
  struct node *journal_node = NULL;
  error_t err = diskfs_cached_lookup (journal_raw_ino, &journal_node);
  if (err || !journal_node)
    {
      JOURNAL_LOG_ERROR ("Not able to open journal for writing. %s",
			 strerror (err));
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }

  uint64_t start_index = 0, end_index = 0;
  if (!initialize_indices (&start_index, &end_index))
    {
      diskfs_nput (journal_node);
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }

  char buf[JOURNAL_ENTRY_SIZE] = { 0 };
  struct journal_entry_bin *entry = (journal_entry_bin_t *) buf;

  entry->magic = JOURNAL_MAGIC;
  entry->version = JOURNAL_VERSION;
  memcpy (&entry->payload, payload, sizeof (journal_payload_bin_t));
  entry->crc32 = crc32 ((const char *) &entry->payload,
			sizeof (journal_payload_bin_t));

  off_t offset = index_to_offset (end_index);
  err = journal_node_write (journal_node, offset, buf, JOURNAL_ENTRY_SIZE);
  if (err)
    {
      JOURNAL_LOG_ERROR ("journal_write_raw_sync: node write failed: %s",
			 strerror (err));
      diskfs_nput (journal_node);
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }
  err = journal_write_entry(entry, end_index);
  if (err)
    {
      JOURNAL_LOG_ERROR("journal_write_raw_sync: outside of the fs entry write failed: %s", strerror(err));
      pthread_mutex_unlock(&sync_write_lock);
      return false;
    } else {
	journal_entry_bin_t verify = { 0 };
	err = journal_read_entry(&verify, end_index);
   	if (err)
	  JOURNAL_LOG_DEBUG("Didn't manage to read the item from out FS. %s", strerror(err));
	else {
	  if (verify.payload.ino != entry->payload.ino || verify.payload.action != entry->payload.action || verify.payload.tx_id != entry->payload.tx_id) 
	    JOURNAL_LOG_DEBUG("Something fishy is going on, read/write not the same");
	}
    }
  JOURNAL_LOG_DEBUG
    ("journal_write_raw_sync: completed node write tx_id=%llu",
     payload->tx_id);

  uint64_t next_index = (end_index + 1) % JOURNAL_NUM_ENTRIES;
  if (next_index == start_index)
    start_index = (start_index + 1) % JOURNAL_NUM_ENTRIES;

  if (!persist_header_with_retry (start_index, next_index, 3))
    {
      JOURNAL_LOG_ERROR ("journal_write_raw_sync: failed to persist header");
      diskfs_nput (journal_node);
      pthread_mutex_unlock (&sync_write_lock);
      return false;
    }

  diskfs_nput (journal_node);
  pthread_mutex_unlock (&sync_write_lock);
  return true;
}

bool
journal_write_raw (const struct journal_payload *entries, size_t count)
{
  pthread_mutex_lock (&sync_write_lock);

  struct node *journal_node = NULL;
  error_t err = diskfs_cached_lookup (journal_raw_ino, &journal_node);
  if (err || !journal_node)
    {
      JOURNAL_LOG_ERROR ("Not able to open journal for writing. %s",
			 strerror (err));
      goto drop;
    }
  uint64_t start_index = 0;
  uint64_t end_index = 0;

  const size_t expected_len = sizeof (journal_payload_bin_t);

  if (!initialize_indices (&start_index, &end_index))
    {
      goto drop;
    }
  for (size_t i = 0; i < count; ++i)
    {
      if (entries[i].len != expected_len)
	{
	  JOURNAL_LOG_ERROR ("journal_write_raw: unexpected payload size %zu",
			     entries[i].len);
	  goto drop;
	}

      if (!journal_write_indexed (journal_node, entries[i].data,
				  expected_len, &end_index, &start_index))
	goto drop;
    }

  if (!persist_header_with_retry (start_index, end_index, 3))
    {
      JOURNAL_LOG_ERROR
	("journal_write_raw: failed to persist updated header after retries.");
    }

  pthread_mutex_unlock (&sync_write_lock);
  diskfs_nput (journal_node);
  return true;

drop:
  __atomic_add_fetch (&journal_dropped_events, count, __ATOMIC_RELAXED);
  JOURNAL_LOG_ERROR
    ("journal_write_raw: dropping %zu txs, total dropped so far: %zu.",
     count, journal_dropped_events);

  diskfs_nput (journal_node);
  pthread_mutex_unlock (&sync_write_lock);
  return false;
}

