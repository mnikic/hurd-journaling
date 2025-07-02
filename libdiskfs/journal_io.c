/* journal_io.c - Low-level journal read and write logic

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

#include <libdiskfs/journal.h>
#include <libdiskfs/journal_io.h>
#include <libdiskfs/journal_globals.h>
#include <libdiskfs/journal_util.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_internal.h>
#include <libdiskfs/diskfs.h>

#include <hurd/store.h>
#include <hurd/fshelp.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>


static struct store *journal_store = NULL;

void
journal_io_set_store (struct store *store)
{
  journal_store = store;
  JOURNAL_LOG_DEBUG ("start byte offset: %llu",
		     journal_layout.device_start_byte);
  JOURNAL_LOG_DEBUG ("start block offset: %u",
		     journal_layout.device_start_block);
  JOURNAL_LOG_DEBUG ("start block count: %u",
		     journal_layout.device_block_count);
  JOURNAL_LOG_DEBUG ("start num entries: %u", journal_layout.num_entries);
  JOURNAL_LOG_DEBUG ("journal io store set.");
}

static inline error_t
journal_store_write (const void *buf, size_t size, off_t relative_offset)
{
  if (!journal_store)
    return EIO;

  if (!buf || (off_t) relative_offset < 0
      || relative_offset + size > journal_layout.device_span_bytes)
    {
      JOURNAL_LOG_DEBUG
	("Invalid write args: buf=%p, relative_offset=%lld, size=%zu (max=%zu)",
	 buf, (long long) relative_offset, size,
	 journal_layout.device_span_bytes);
      return EINVAL;
    }

  off_t absolute_offset = journal_layout.device_start_byte + relative_offset;

  if (absolute_offset % journal_layout.device_block_size != 0)
    {
      JOURNAL_LOG_DEBUG ("Offset %lld not aligned to store block size %u",
			 (long long) absolute_offset,
			 journal_layout.device_block_size);
      return EINVAL;
    }

  store_offset_t block_offset =
    absolute_offset / journal_layout.device_block_size;
  size_t amount = 0;
  error_t err = store_write (journal_store, block_offset, buf, size, &amount);

  if (err || amount != size)
    {
      JOURNAL_LOG_DEBUG
	("store_write failed or incomplete: err=%d, written=%zu", err,
	 amount);
      return err ? err : EIO;
    }

  return 0;
}

error_t
journal_write_entry (const journal_entry_bin_t * entry, size_t index)
{
  if (!entry)
    return EINVAL;

  off_t relative_offset = journal_layout.reserved_space +
    (index % journal_layout.num_entries) * journal_layout.entry_size;

  return journal_store_write (entry, sizeof (journal_entry_bin_t),
			      relative_offset);
}

error_t
journal_write_header (const journal_header_t * hdr)
{
  if (!journal_store || !hdr)
    return EINVAL;

  return journal_store_write (hdr, JOURNAL_HEADER_SIZE, 0);
}

static inline error_t
journal_store_read (void *out_buf, size_t size, off_t relative_offset)
{
  if (!journal_store || !out_buf
      || relative_offset + size > journal_layout.device_span_bytes)
    return EINVAL;

  off_t absolute_offset = journal_layout.device_start_byte + relative_offset;
  if (absolute_offset % journal_layout.device_block_size != 0)
    return EINVAL;

  void *buf = NULL;
  size_t len = 0;

  error_t err = store_read (journal_store,
			    absolute_offset /
			    journal_layout.device_block_size,
			    size, &buf, &len);

  if (err)
    {
      if (buf)
	vm_deallocate (mach_task_self (), (vm_address_t) buf, len);
      return err;
    }

  if (len < size)
    {
      JOURNAL_LOG_ERROR ("Partial read: requested %zu, got %zu", size, len);
      memset (out_buf, 0, size);	// optional: zero to avoid using junk
      memcpy (out_buf, buf, len);	// copy what we got
      vm_deallocate (mach_task_self (), (vm_address_t) buf, len);
      return EIO;		// or return 0 if you're OK with partial reads
    }

  memcpy (out_buf, buf, size);
  vm_deallocate (mach_task_self (), (vm_address_t) buf, len);
  return 0;
}

error_t
journal_read_entry (journal_entry_bin_t * out_entry, size_t block_index)
{
  if (!journal_store || !out_entry)
    return EINVAL;

  off_t relative_offset = journal_layout.reserved_space +
    (block_index % journal_layout.num_entries) * journal_layout.entry_size;

  return journal_store_read (out_entry, sizeof (journal_entry_bin_t),
			     relative_offset);
}

error_t
journal_read_header (journal_header_t * out_hdr)
{
  if (!journal_store || !out_hdr)
    return EINVAL;
  return journal_store_read (out_hdr, sizeof (journal_header_t), 0);
}
