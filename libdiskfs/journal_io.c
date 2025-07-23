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
#include <libdiskfs/crc32.h>
#include <libdiskfs/diskfs.h>
#include <libdiskfs/priv.h>
#include <hurd/fshelp.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

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
 */
bool
journal_node_read_and_validate_header (struct node *np,
				       struct journal_header *out)
{
  char buf[sizeof (struct journal_header)] = { 0 };
  error_t err =
    journal_node_read (np, 0, buf, sizeof (struct journal_header));
  if (err)
    {
      JOURNAL_LOG_ERROR ("journal_node_read failed reading header: %d", err);
      return false;
    }

  struct journal_header *hdr = (struct journal_header *) buf;
  uint32_t expected_crc = hdr->crc32;
  hdr->crc32 = 0;		// Zero before CRC computation, as expected during write
  uint32_t actual_crc =
    crc32 ((const void *) hdr, sizeof (struct journal_header));
  if (actual_crc != expected_crc || hdr->magic != JOURNAL_MAGIC
      || hdr->version != JOURNAL_VERSION)
    {
      JOURNAL_LOG_DEBUG ("journal replay: node header invalid.");
      return false;
    }

  if (hdr->start_index >= JOURNAL_NUM_ENTRIES
      || hdr->end_index >= JOURNAL_NUM_ENTRIES)
    {
      JOURNAL_LOG_DEBUG ("journal_node_read: header indices out of bounds.");
      return false;
    }

  *out = *hdr;
  return true;
}

/*
 * journal_node_read_and_validate_entry - Reads and validates a journal entry.
 *
 * Performs CRC, magic, and version checks. Returns true if valid.
 */
bool
journal_node_read_and_validate_entry (struct node *np, uint64_t index,
				      struct journal_payload_bin *out)
{
  char buf[JOURNAL_ENTRY_SIZE] = { 0 };
  uint64_t offset = index_to_offset (index);
  error_t err =
    journal_node_read (np, (off_t) offset, buf, JOURNAL_ENTRY_SIZE);
  if (err)
    {
      JOURNAL_LOG_DEBUG ("journal_node_read failed at offset %ld.",
			 (long) offset);
      return false;
    }

  struct journal_entry_bin *entry = (struct journal_entry_bin *) buf;
  if (entry->magic != JOURNAL_MAGIC)
    {
      JOURNAL_LOG_DEBUG ("Bad journal entry magic at offset %ld.",
			 (long) offset);
      return false;
    }

  if (entry->version != JOURNAL_VERSION)
    {
      JOURNAL_LOG_DEBUG ("Journal entry version mismatch at offset %ld",
			 (long) offset);
      return false;
    }

  uint32_t stored_crc = entry->crc32;
  entry->crc32 = 0;		// Zero before CRC computation, as expected during write
  uint32_t actual_entry_crc = crc32 ((const char *) &entry->payload,
				     sizeof (struct journal_payload_bin));

  if (actual_entry_crc != stored_crc)
    {
      JOURNAL_LOG_DEBUG ("Journal entry CRC mismatch at offset %ld.",
			 (long) offset);
      return false;
    }

  *out = entry->payload;
  return true;
}
