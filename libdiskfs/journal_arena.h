/* journal_arena.h - Arena for the journaling system.

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
#ifndef JOURNAL_ARENA_H
#define JOURNAL_ARENA_H
#include <stddef.h>

struct journal_arena {
	char *base;
	size_t offset;
	size_t size;
};

struct journal_arena *
journal_arena_create(size_t size); 

void *
journal_arena_alloc(struct journal_arena *a, size_t sz);

void
journal_arena_destroy(struct journal_arena *a); 

#endif // JOURNAL_ARENA_H
