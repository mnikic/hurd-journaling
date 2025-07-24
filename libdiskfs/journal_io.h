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

void
journal_io_set_store(struct store *store);

error_t journal_read_header(journal_header_t *out_hdr);
error_t journal_write_header(const journal_header_t *hdr);

error_t journal_write_entry(const journal_entry_bin_t *entry, size_t block_index);
error_t journal_read_entry(journal_entry_bin_t *out_entry, size_t block_index);

error_t
journal_node_read (struct node *np, off_t offset, void *buf, size_t len);

error_t
journal_node_write (struct node *np, off_t offset, const void *buf,
		    size_t len);

/* Read and validate the journal header from the given node. */
bool journal_node_read_and_validate_header (journal_header_t *out);

/* Read and validate a journal entry at the given index from the journal node. */
bool journal_node_read_and_validate_entry (struct node *n,
					   uint64_t index,
					   journal_payload_bin_t *out);

#endif /* LIBDISKFS_JOURNAL_IO_H */

