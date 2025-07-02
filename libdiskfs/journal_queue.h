/* journal_queue.h - Asynchronous journal queue API

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

#ifndef JOURNAL_QUEUE_H
#define JOURNAL_QUEUE_H

#include <stddef.h>
#include <stdbool.h>

void journal_queue_init (void);
void journal_queue_shutdown (void);
bool journal_enqueue (const char *data, size_t len);
void journal_flush_now (void);
void *journal_flusher_thread (void *arg);

#endif /* JOURNAL_QUEUE_H */

