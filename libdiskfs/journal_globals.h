/* journal_globals.h - Global journaling flags and shared synchronization state.

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

#ifndef LIBDISKFS_JOURNAL_GLOBALS_H
#define LIBDISKFS_JOURNAL_GLOBALS_H

#include <libdiskfs/journal_inode_denylist.h>

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

extern volatile size_t journal_dropped_events;
extern volatile bool journal_device_ready;
extern volatile bool journal_enabled;

extern const journal_inode_denylist_t *journal_denylist;

extern pthread_mutex_t queue_lock;
extern pthread_cond_t queue_cond;

#endif /* LIBDISKFS_JOURNAL_GLOBALS_H */

