/* journal_io.h - Journal read/validate helpers for raw disk access

   Copyright (C) 2025 Free Software Foundation, Inc.

   Written by Milos Nikic.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   The GNU Hurd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with the GNU Hurd.  If not, see <https://www.gnu.org/licenses/>.  */

#ifndef LIBDISKFS_JOURNAL_IO_H
#define LIBDISKFS_JOURNAL_IO_H

#include <libdiskfs/journal_format.h>
#include <libdiskfs/diskfs.h>
#include <hurd/store.h>

/* Set the underlying store to use for journal reads and writes.
 *
 * Must be called before any read or write operation. Typically set during
 * journal initialization using the store derived from the raw journal device.
 */
void journal_io_set_store (struct store *store);

/* Write the journal header (metadata describing the journal layout).
 *
 * This writes the header to the beginning of the journal region on disk.
 *
 * Returns 0 on success or an error code on failure.
 */
error_t journal_write_header (const journal_header_t * hdr);

/* Read the journal header from disk.
 *
 * Populates `out_hdr` with the layout information currently stored on disk.
 *
 * Returns 0 on success or an error code on failure.
 */
error_t journal_read_header (journal_header_t * out_hdr);

/* Write a journal entry to a specific index in the journal ring buffer.
 *
 * The `index` is relative to the ring buffer layout (not a byte offset).
 *
 * Returns 0 on success or an error code on failure.
 */
error_t journal_write_entry (const journal_entry_bin_t * entry, size_t index);

/* Read a journal entry from a specific index in the journal ring buffer.
 *
 * Populates `out_entry` with the decoded journal entry.
 *
 * Returns 0 on success or an error code on failure.
 */
error_t journal_read_entry (journal_entry_bin_t * out_entry, size_t index);

#endif /* LIBDISKFS_JOURNAL_IO_H */
