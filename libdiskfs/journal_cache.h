/* journal_cache.h - Journal thread-safe metadata decision cache.
   
   Provides a simple fixed-size hash-based cache for storing recent
   journaling decisions (e.g., whether a path should be logged).
   Includes TTL-based expiration and hit/miss statistics.

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

/* Single cache entry storing hash of the path, decision result,
   timestamp of when it was cached, and a copy of the path. */
typedef struct journal_cache_entry
{
  uint32_t hash;		/* Hash of the canonicalized path */
  bool decision;		/* Decision (true = allow, false = deny) */
  uint64_t timestamp;		/* Millisecond timestamp of entry creation */
  char path[255];		/* Canonicalized path string */
} journal_cache_entry_t;

/* Summary of current cache state, useful for diagnostics. */
typedef struct journal_cache_stats
{
  int total_slots;		/* Total capacity */
  int used_slots;		/* Slots currently filled with valid data */
  int expired_slots;		/* Expired entries found during last sweep */
  uint64_t hits;		/* Cache hits */
  uint64_t misses;		/* Cache misses */
} journal_cache_stats_t;

/* Cache instance. Backed by a caller-provided buffer of entries. */
typedef struct journal_cache
{
  journal_cache_entry_t *entries;	/* Pointer to caller-provided array */
  size_t size;			/* Number of slots (must be power of 2) */
  uint32_t mask;		/* Mask for fast modulo indexing */
  bool initialized;		/* Whether the cache is ready for use */
  uint64_t ttl_ms;		/* Time-to-live for each entry */
  uint64_t hits;		/* Running total of cache hits */
  uint64_t misses;		/* Running total of cache misses */
} journal_cache_t;

/* Initialize a journal cache with a given buffer and TTL.
   The buffer must have size a power of 2. TTL is in milliseconds. */
journal_cache_t
journal_cache_init (journal_cache_entry_t * buffer, size_t size,
		    uint64_t ttl_ms);

/* Check whether a path exists in the cache and is still valid.
   Returns true on cache hit, false on miss. */
bool
journal_cache_check (journal_cache_t * cache, const char *path,
		     bool *cached_decision);

/* Store a journaling decision for a given path. May evict old entry. */
void
journal_cache_store (journal_cache_t * cache, const char *path,
		     bool decision);

/* Clear all entries from the cache. */
void journal_cache_clear (journal_cache_t * cache);

/* Return current statistics for the cache. */
journal_cache_stats_t journal_cache_stats (journal_cache_t * cache);

/* Remove expired entries from the cache. Typically called periodically. */
void journal_cache_expire_old (journal_cache_t * cache);

#endif /* LIBDISKFS_JOURNAL_CACHE_H */
