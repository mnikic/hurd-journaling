/* journal_replayer.h - Journal replayer for GNU Hurd journaling
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

#ifndef LIBDISKFS_JOURNAL_REPLAYER_H
#define LIBDISKFS_JOURNAL_REPLAYER_H

/* Replay a binary journal file from disk (used during early boot).
   Note: 'path' is currently ignored; replay uses a fixed device inode. */
void journal_replay_from_file (const char *path);

#endif /* LIBDISKFS_JOURNAL_REPLAYER_H */

