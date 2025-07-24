/* journal_io.c - Low-level journal read and validation logic

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

#include <libdiskfs/journal_io.h>
#include <libdiskfs/journal_globals.h>
#include <libdiskfs/journal_util.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/crc32.h>
#include <libdiskfs/diskfs.h>
#include <libdiskfs/priv.h>
#include <hurd/store.h>
#include <hurd/fshelp.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>


// New "raw" journal location
#define JOURNAL_BLOCK_OFFSET 1033728ULL 
#define JOURNAL_BLOCK_SIZE_BYTES 4096ULL
#define JOURNAL_BASE_OFFSET_BYTES (JOURNAL_BLOCK_OFFSET * JOURNAL_BLOCK_SIZE_BYTES)

static struct store *journal_store = NULL;

void
journal_io_set_store (struct store *store)
{
  journal_store = store;

  JOURNAL_LOG_DEBUG ("journal io store set.");
}

static error_t
journal_store_write(const void *buf, size_t size, off_t relative_offset)
{
  if (!journal_store)
    return EIO;

  if (!buf || (off_t) relative_offset < 0 || relative_offset + size > RAW_DEVICE_SIZE)
    {
      JOURNAL_LOG_DEBUG(
        "Invalid write args: buf=%p, relative_offset=%lld, size=%zu (max=%zu)",
        buf, (long long)relative_offset, size, (size_t)RAW_DEVICE_SIZE);
      return EINVAL;
    }

  off_t absolute_offset = JOURNAL_BASE_OFFSET_BYTES + relative_offset;

  if (absolute_offset % journal_store->block_size != 0)
    {
      JOURNAL_LOG_DEBUG(
        "Offset %lld not aligned to store block size %u",
        (long long)absolute_offset, journal_store->block_size);
      return EINVAL;
    }

  store_offset_t block_offset = absolute_offset / journal_store->block_size;
  size_t amount = 0;

  JOURNAL_LOG_DEBUG(
    "Writing %zu bytes to block offset %llu (byte offset %lld)",
    size, (unsigned long long)block_offset, (long long)absolute_offset);

  error_t err = store_write(journal_store, block_offset, buf, size, &amount);

  if (err || amount != size)
    {
      JOURNAL_LOG_DEBUG(
        "store_write failed or incomplete: err=%d, written=%zu",
        err, amount);
      return err ? err : EIO;
    }

  return 0;
}

error_t
journal_write_entry(const journal_entry_bin_t *entry, size_t block_index)
{
  if (!entry)
    return EINVAL;

  // Entry index 0 starts *after* the header.
  off_t relative_offset = JOURNAL_RESERVED_SPACE + (block_index % JOURNAL_NUM_ENTRIES) * JOURNAL_ENTRY_SIZE;
  return journal_store_write(entry, sizeof(journal_entry_bin_t), relative_offset);
}

error_t
journal_write_header(const journal_header_t *hdr)
{
  if (!journal_store || !hdr)
    return EINVAL;

  // Header always at relative offset 0
  return journal_store_write(hdr, JOURNAL_HEADER_SIZE, 0);
}

error_t
journal_read_entry(journal_entry_bin_t *out_entry, size_t block_index)
{
  if (!journal_store || !out_entry)
    return EINVAL;

  off_t offset = JOURNAL_BASE_OFFSET_BYTES + JOURNAL_RESERVED_SPACE +
                 (block_index % JOURNAL_NUM_ENTRIES) * JOURNAL_ENTRY_SIZE;

  void *buf = NULL;
  size_t len = 0;
  error_t err = store_read(journal_store,
                           offset / journal_store->block_size,
                           JOURNAL_ENTRY_SIZE, &buf, &len);
  if (err)
    return err;

  if (len < sizeof(journal_entry_bin_t))
    {
      vm_deallocate(mach_task_self(), (vm_address_t)buf, len);
      return EIO;
    }

  memcpy(out_entry, buf, sizeof(journal_entry_bin_t));
  vm_deallocate(mach_task_self(), (vm_address_t)buf, len);
  return 0;
}

error_t
journal_read_header(journal_header_t *out_hdr)
{
  if (!journal_store || !out_hdr)
    return EINVAL;

  off_t offset = JOURNAL_BASE_OFFSET_BYTES; // Header is always at the beginning of the journal area

  void *buf = NULL;
  size_t len = 0;
  error_t err = store_read(journal_store,
                           offset / journal_store->block_size,
                           JOURNAL_RESERVED_SPACE,
                           &buf, &len);
  if (err)
    return err;

  if (len < sizeof(journal_header_t))
    {
      vm_deallocate(mach_task_self(), (vm_address_t)buf, len);
      return EIO;
    }

  memcpy(out_hdr, buf, sizeof(journal_header_t));
  vm_deallocate(mach_task_self(), (vm_address_t)buf, len);
  return 0;
}

/*
 * journal_node_read - Reads a range of bytes from a node at a specified offset.
 *
 * This function expects the caller to hold the node lock if concurrent access
 * is possible. It performs bounds checking and safely reads up to the requested
 * length or remaining file size.
 */
error_t
journal_node_read (struct node *np, off_t offset, void *buf, size_t len)
{
  error_t err;
  mach_msg_type_number_t rdlen = len;

  if (offset < 0 || offset > np->dn_stat.st_size)
    return EINVAL;

  if (offset + len > np->dn_stat.st_size)
    rdlen = np->dn_stat.st_size - offset;

  if (rdlen == 0)
    err = 0;
  else
    err = _diskfs_rdwr_internal (np, buf, offset, &rdlen, 0, 0);

  return err;
}

error_t
journal_node_write (struct node *np, off_t offset, const void *buf,
		    size_t len)
{
  error_t err;
  mach_msg_type_number_t wrlen = len;

  if (offset < 0)
    return EINVAL;

  if (offset >= np->dn_stat.st_size)
    return EFBIG;		// Offset beyond EOF — disallow write

  if (offset + len > np->dn_stat.st_size)
    wrlen = np->dn_stat.st_size - offset;	// Clamp to EOF

  if (wrlen == 0)
    return 0;

  err = _diskfs_rdwr_internal (np, (void *) buf, offset, &wrlen, 1, 0);

  if (!err && wrlen != len)
    return EIO;			// Unexpected short write

  return err;
}

/*
 * journal_node_read_and_validate_header - Reads and validates the journal header.
 *
 * Performs CRC and magic/version checks. Returns true if valid.
 * out may point to garbage in case of error. Do not use in that case.
 */
bool
journal_node_read_and_validate_header (journal_header_t * out)
{
  error_t err = journal_read_header (out);
  if (err)
    {
      JOURNAL_LOG_ERROR ("journal_node_read failed reading header: %d", err);
      return false;
    }

  if (out->crc32 != journal_compute_header_crc32 (out) || out->magic != JOURNAL_MAGIC
      || out->version != JOURNAL_VERSION)
    {
      JOURNAL_LOG_DEBUG ("journal replay: node header invalid.");
      return false;
    }

  if (out->start_index >= JOURNAL_NUM_ENTRIES
      || out->end_index >= JOURNAL_NUM_ENTRIES)
    {
      JOURNAL_LOG_DEBUG ("journal_node_read: header indices out of bounds.");
      return false;
    }

  return true;
}

/*
 * journal_node_read_and_validate_entry - Reads and validates a journal entry.
 *
 * Performs CRC, magic, and version checks. Returns true if valid.
 */
bool
journal_node_read_and_validate_entry (uint64_t index,
				      journal_entry_bin_t *out)
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
      JOURNAL_LOG_DEBUG ("Journal entry version mismatch at index %llu", index);
      return false;
    }

  uint32_t actual_entry_crc = crc32 ((const char *) &out->payload,
				     sizeof (journal_payload_bin_t));
  if (actual_entry_crc != out->crc32)
    {
      JOURNAL_LOG_DEBUG ("Journal entry CRC mismatch at index %llu.", index);
      return false;
    }

  return true;
}

