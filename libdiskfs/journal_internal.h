/* journal_internal.h - Internal journal struct definitions

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

#ifndef LIBDISKFS_JOURNAL_INTERNAL_H
#define LIBDISKFS_JOURNAL_INTERNAL_H

#include <stddef.h>
#include <sys/types.h>

typedef struct journal_layout
{
  size_t reserved_space;
  size_t num_entries;
  size_t header_size;
  size_t entry_size;
  size_t device_start_block;
  off_t device_start_byte;
  size_t device_block_count;
  size_t device_block_size;
  size_t device_span_bytes;
} journal_layout_t;

#endif /* LIBDISKFS_JOURNAL_INTERNAL_H */
