/* journal_cache.c - Journal thread-safe metadata decision cache.
   
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
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>

#include <libdiskfs/journal_cache.h>
#include <libdiskfs/journal_util.h>
#include <libdiskfs/journal_path_util.h>

static bool
is_entry_expired (journal_cache_t *cache, journal_cache_entry_t *entry,
		  uint64_t current_time)
{
  if (current_time == 0 || entry->timestamp == 0)
    return false;
  return (current_time - entry->timestamp) > cache->ttl_ms;
}

journal_cache_t
journal_cache_init (journal_cache_entry_t *buffer, size_t size,
		    uint64_t ttl_ms)
{
  journal_cache_t cache = { 0 };
  if (!buffer)
    return cache;
  if ((size & (size - 1)) != 0)
    {
      JOURNAL_LOG_ERROR ("Cache size must be power of two");
      return cache;
    }

  memset (buffer, 0, sizeof (journal_cache_entry_t) * size);
  cache.entries = buffer;
  cache.size = size;
  cache.mask = size - 1;
  cache.ttl_ms = ttl_ms;
  cache.initialized = true;
  cache.hits = 0;
  cache.misses = 0;
  return cache;
}

bool
journal_cache_check (journal_cache_t *cache, const char *path,
		     bool *cached_decision)
{
  if (!cache || !cache->initialized || !cache->entries)
    return false;

  uint64_t current_time = journal_current_time_ms ();
  uint32_t hash = journal_hash_path (path);
  int start_index = hash & cache->mask;

  for (int probe = 0; probe < cache->size; probe++)
    {
      int index = (start_index + probe) & cache->mask;
      journal_cache_entry_t *entry = &cache->entries[index];

      if (entry->hash == 0)
	{
	  cache->misses++;
	  return false;
	}
      if (entry->hash == hash && strcmp (entry->path, path) == 0)
	{
	  if (is_entry_expired (cache, entry, current_time))
	    {
	      memset (entry, 0, sizeof (*entry));
	      cache->misses++;
	      return false;
	    }
	  *cached_decision = entry->decision;
	  cache->hits++;
	  return true;
	}

      if (is_entry_expired (cache, entry, current_time))
	{
	  memset (entry, 0, sizeof (*entry));
	}
    }

  cache->misses++;
  return false;
}

void
journal_cache_store (journal_cache_t *cache, const char *path, bool decision)
{
  if (!cache || !cache->initialized || !cache->entries)
    return;

  uint64_t current_time = journal_current_time_ms ();
  uint32_t hash = journal_hash_path (path);
  int start_index = hash & cache->mask;

  for (int probe = 0; probe < cache->size; probe++)
    {
      int index = (start_index + probe) & cache->mask;
      journal_cache_entry_t *entry = &cache->entries[index];

      if (entry->hash == 0 || is_entry_expired (cache, entry, current_time))
	{
	  entry->hash = hash;
	  entry->decision = decision;
	  entry->timestamp = current_time;

	  size_t path_len = strlen (path);
	  if (path_len >= sizeof (entry->path))
	    path_len = sizeof (entry->path) - 1;
	  memcpy (entry->path, path, path_len);
	  entry->path[path_len] = '\0';
	  return;
	}

      if (entry->hash == hash && strcmp (entry->path, path) == 0)
	{
	  entry->decision = decision;
	  entry->timestamp = current_time;
	  return;
	}
    }

  // fallback eviction
  int index = start_index;
  journal_cache_entry_t *entry = &cache->entries[index];
  entry->hash = hash;
  entry->decision = decision;
  entry->timestamp = current_time;
  size_t path_len = strlen (path);
  if (path_len >= sizeof (entry->path))
    path_len = sizeof (entry->path) - 1;
  memcpy (entry->path, path, path_len);
  entry->path[path_len] = '\0';
}

void
journal_cache_clear (journal_cache_t *cache)
{
  if (cache && cache->initialized && cache->entries)
    {
      memset (cache->entries, 0,
	      sizeof (journal_cache_entry_t) * cache->size);
      cache->hits = 0;
      cache->misses = 0;
    }
}

void
journal_cache_expire_old (journal_cache_t *cache)
{
  if (!cache || !cache->initialized || !cache->entries)
    return;

  uint64_t current_time = journal_current_time_ms ();
  if (current_time == 0)
    return;

  for (int i = 0; i < cache->size; i++)
    {
      journal_cache_entry_t *entry = &cache->entries[i];
      if (entry->hash != 0 && is_entry_expired (cache, entry, current_time))
	{
	  memset (entry, 0, sizeof (*entry));
	}
    }
}

journal_cache_stats_t
journal_cache_stats (journal_cache_t *cache)
{
  journal_cache_stats_t stats = { 0 };
  if (!cache || !cache->entries || !cache->initialized)
    return stats;

  uint64_t current_time = journal_current_time_ms ();
  stats.total_slots = cache->size;
  stats.hits = cache->hits;
  stats.misses = cache->misses;

  for (int i = 0; i < cache->size; i++)
    {
      journal_cache_entry_t *entry = &cache->entries[i];
      if (entry->hash != 0)
	{
	  stats.used_slots++;
	  if (current_time != 0
	      && is_entry_expired (cache, entry, current_time))
	    {
	      stats.expired_slots++;
	    }
	}
    }
  return stats;
}
