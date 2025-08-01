/* journal_inode_denylist.h Denylist implementation for inodes.

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

#ifndef LIBDISKFS_JOURNAL_INODE_DENYLIST_H
#define LIBDISKFS_JOURNAL_INODE_DENYLIST_H

#include <stdint.h>
#include <stdbool.h>
#include <libdiskfs/journal_format.h>

#define MAX_INODE_VALUE 131072 * 2
#define INODE_BITSET_SIZE (MAX_INODE_VALUE / 8)

typedef struct
{
  uint8_t bits[INODE_BITSET_SIZE];
} journal_inode_denylist_builder_t;

typedef struct
{
  uint8_t bits[(MAX_INODE_VALUE + 7) / 8];
} journal_inode_denylist_t;

/** Initialize a new denylist builder (zeroed). */
journal_inode_denylist_builder_t journal_inode_denylist_builder_init (void);

/** Add an inode to the denylist builder. */
bool journal_inode_denylist_builder_add (journal_inode_denylist_builder_t *
					 builder, journal_ino_t ino);

/** Finalize the builder into a read-only denylist view. */
journal_inode_denylist_t
journal_inode_denylist_finalize (journal_inode_denylist_builder_t * builder);

/** Check if the given inode is in the denylist. */
bool journal_inode_denylist_contains (const journal_inode_denylist_t * set,
				      journal_ino_t ino);

#endif // LIBDISKFS_JOURNAL_INODE_DENYLIST_H
