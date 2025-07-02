/* journal_config.h - Journal configuration: tunable paths and sizes.

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

#ifndef LIBDISKFS_JOURNAL_CONFIG_H
#define LIBDISKFS_JOURNAL_CONFIG_H


#define RAW_DEVICE_PATH "/root/journal.img"
#define JOURNAL_RAW_INO 162637
#define RAW_DEVICE_SIZE (8 * 1024 * 1024)  /* 8MB */
/* Set to 1 to simulate journal replay without mutating the file system. */
#define JOURNAL_REPLAY_DRY_RUN 0 /* Set to 1 to simulate journal replay without making changes */
#define JOURNAL_ENTRY_SIZE 4096ULL
#define JOURNAL_RESERVED_SPACE 4096ULL  /* Leave room for future header growth */
#define JOURNAL_DATA_CAPACITY (RAW_DEVICE_SIZE - JOURNAL_RESERVED_SPACE)
#define JOURNAL_NUM_ENTRIES (JOURNAL_DATA_CAPACITY / JOURNAL_ENTRY_SIZE)
#define JOURNAL_REPLAY_MIN_INO 256

#endif /* LIBDISKFS_JOURNAL_CONFIG_H */

