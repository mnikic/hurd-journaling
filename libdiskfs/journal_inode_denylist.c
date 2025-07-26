/* journal_inode_denylist.c Denylist implementation for inodes.

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

#include <libdiskfs/journal_inode_denylist.h>
#include <libdiskfs/journal_format.h>
#include <stdatomic.h>
#include <string.h>

typedef journal_inode_denylist_builder_t builder_t;

builder_t
journal_inode_denylist_builder_init (void)
{
  builder_t builder = { 0 };
  return builder;
}

void
journal_inode_denylist_builder_add (builder_t * builder, journal_ino_t ino)
{
  if (ino >= MAX_INODE_VALUE)
    return;
  builder->bits[ino / 8] |= (1 << (ino % 8));
}

journal_inode_denylist_t
journal_inode_denylist_finalize (builder_t * builder)
{
  atomic_thread_fence (memory_order_seq_cst);
  journal_inode_denylist_t result = { 0 };
  memcpy (result.bits, builder->bits, sizeof (result.bits));
  return result;
}

bool
journal_inode_denylist_contains (const journal_inode_denylist_t * set,
				 journal_ino_t ino)
{
  if (ino >= MAX_INODE_VALUE)
    return false;
  return (set->bits[ino / 8] & (1 << (ino % 8))) != 0;
}
