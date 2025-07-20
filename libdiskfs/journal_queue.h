/* journal_queue.h - Internal fallback journal queue (in-memory async buffer)

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

#ifndef LIBDISKFS_JOURNAL_QUEUE_H
#define LIBDISKFS_JOURNAL_QUEUE_H

#include <stddef.h>
#include <stdbool.h>

/* Initialize the in-memory journal queue.  */
void journal_queue_init (void);

/* Shutdown and release all queue resources.  */
void journal_queue_shutdown (void);

/* Enqueue a journal entry for later flush.
   Returns false if data is not of the correct length  */
bool journal_enqueue (const char *data, size_t len);

/* Background thread that flushes queued journal entries to disk.  */
void *journal_flusher_thread (void *arg);

#endif /* LIBDISKFS_JOURNAL_QUEUE_H */
