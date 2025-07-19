/* journal_inode_set.h - Simple set for journaling needs.

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

#ifndef LIBDISKFS_JOURNAL_INODE_SET_H
#define LIBDISKFS_JOURNAL_INODE_SET_H

#include <stdint.h>
#include <stdbool.h>
#include <libdiskfs/journal_format.h>

#define JOURNAL_MAX_DENY_INODES 4096

void inode_set_init (void);
void inode_set_add (journal_ino_t ino);
bool inode_set_contains (journal_ino_t ino);

#endif /*  LIBDISKFS_JOURNAL_INODE_SET_H */
