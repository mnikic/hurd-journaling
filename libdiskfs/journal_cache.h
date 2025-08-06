/* journal_cache.h - Journal thread safe cache. 

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

#ifndef LIBDISKFS_JOURNAL_CACHE_H
#define LIBDISKFS_JOURNAL_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct journal_cache_entry
{
  uint32_t hash;
  bool decision;
  uint64_t timestamp;
  char path[255];
} journal_cache_entry_t;

typedef struct journal_cache_stats
{
  int total_slots;
  int used_slots;
  int expired_slots;
  uint64_t hits;
  uint64_t misses;
} journal_cache_stats_t;

typedef struct journal_cache
{
  journal_cache_entry_t *entries;
  size_t size;
  uint32_t mask;
  bool initialized;
  uint64_t ttl_ms;
  uint64_t hits;
  uint64_t misses;
} journal_cache_t;

journal_cache_t
journal_cache_init (journal_cache_entry_t * buffer, size_t size,
		    uint64_t ttl_ms);

bool
journal_cache_check (journal_cache_t * cache, const char *path,
		     bool *cached_decision);
void
journal_cache_store (journal_cache_t * cache, const char *path,
		     bool decision);
void journal_cache_clear (journal_cache_t * cache);

journal_cache_stats_t journal_cache_stats (journal_cache_t * cache);

void journal_cache_expire_old (journal_cache_t * cache);

#endif
