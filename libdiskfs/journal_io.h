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

/* Read and validate the journal header from the given node. */
bool journal_node_read_and_validate_header (struct node *n,
					    struct journal_header *out);

/* Read and validate a journal entry at the given index from the journal node. */
bool journal_node_read_and_validate_entry (struct node *n,
					   uint64_t index,
					   struct journal_payload_bin *out);

#endif /* LIBDISKFS_JOURNAL_IO_H */
