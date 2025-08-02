#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>		
#include <libdiskfs/journal_utils.h>
#include <stdlib.h>	

// Thread-local cache approach for GNU Hurd
#define FILTER_CACHE_SIZE 512
#define FILTER_CACHE_MASK (FILTER_CACHE_SIZE - 1)

struct filter_cache_entry
{
  uint32_t hash;
  bool decision;
  uint64_t timestamp;		
  char path[120];
};

struct filter_cache
{
  struct filter_cache_entry entries[FILTER_CACHE_SIZE];
  bool initialized;
  uint64_t ttl = 30000000ULL;
  uint64_t hits;
  uint64_t misses;
};

// Thread-local storage for cache
static __thread struct filter_cache thread_cache = { 0 };

// Check if cache entry is expired
static bool
is_entry_expired (struct filter_cache *cache, struct filter_cache_entry *entry, uint64_t current_time)
{
  if (current_time == 0 || entry->timestamp == 0)
    {
      return false;		// Don't expire if we can't get time
    }
  return (current_time - entry->timestamp) > cache->ttl;
}

// Check cache for previous decision with linear probing
static bool
check_filter_cache (struct filter_cache *cache, const char *path,
		    bool *cached_decision)
{
  uint64_t current_time = get_current_time ();

  if (!cache->initialized)
    {
      memset (cache->entries, 0, sizeof (cache->entries));
      cache->initialized = true;
    }

  uint32_t hash = journal_hash_path (path);
  int start_index = hash & FILTER_CACHE_MASK;

  // Linear probing - check up to FILTER_CACHE_SIZE entries
  for (int probe = 0; probe < FILTER_CACHE_SIZE; probe++)
    {
      int index = (start_index + probe) & FILTER_CACHE_MASK;
      struct filter_cache_entry *entry = &cache->entries[index];

      // Empty slot - definitely not found
      if (entry->hash == 0)
	{
	  cache->misses++;
	  return false;
	}

      // Check if this entry matches our path
      if (entry->hash == hash && strcmp (entry->path, path) == 0)
	{
	  // Found matching entry - check if expired
	  if (is_entry_expired (entry, current_time))
	    {
	      // Expired - clear the entry and return miss
	      memset (entry, 0, sizeof (*entry));
	      cache->misses++;
	      return false;
	    }

	  // Valid, non-expired entry
	  *cached_decision = entry->decision;
	  cache->hits++;
	  return true;		// Cache hit
	}

      // Check if this slot has an expired entry we can reclaim
      if (is_entry_expired (entry, current_time))
	{
	  // Clear expired entry but continue probing for our path
	  memset (entry, 0, sizeof (*entry));
	}
    }

  // Cache miss after full probe - miss already counted above
  return false;
}

// Store decision in cache with linear probing
static void
store_filter_cache (struct filter_cache *cache, const char *path,
		    bool decision)
{
  uint64_t current_time = get_current_time ();

  uint32_t hash = hash_path (path);
  int start_index = hash & FILTER_CACHE_MASK;

  // Linear probing to find an empty or expired slot
  for (int probe = 0; probe < FILTER_CACHE_SIZE; probe++)
    {
      int index = (start_index + probe) & FILTER_CACHE_MASK;
      struct filter_cache_entry *entry = &cache->entries[index];

      // Empty slot or expired entry - use this slot
      if (entry->hash == 0 || is_entry_expired (entry, current_time))
	{
	  entry->hash = hash;
	  entry->decision = decision;
	  entry->timestamp = current_time;

	  // Truncate path if too long
	  size_t path_len = strlen (path);
	  if (path_len >= sizeof (entry->path))
	    {
	      path_len = sizeof (entry->path) - 1;
	    }
	  memcpy (entry->path, path, path_len);
	  entry->path[path_len] = '\0';
	  return;
	}

      // If this entry matches our path, update it
      if (entry->hash == hash && strcmp (entry->path, path) == 0)
	{
	  entry->decision = decision;
	  entry->timestamp = current_time;
	  return;
	}
    }

  // Cache is full - replace the first entry (simple eviction policy)
  struct filter_cache_entry *entry = &cache->entries[start_index];
  entry->hash = hash;
  entry->decision = decision;
  entry->timestamp = current_time;

  size_t path_len = strlen (path);
  if (path_len >= sizeof (entry->path))
    {
      path_len = sizeof (entry->path) - 1;
    }
  memcpy (entry->path, path, path_len);
  entry->path[path_len] = '\0';
}
// Optional: Clear thread-local cache
void
clear_filter_cache (void)
{
  if (thread_cache.initialized)
    {
      memset (thread_cache.entries, 0, sizeof (thread_cache.entries));
      thread_cache.initialized = false;
      thread_cache.hits = 0;
      thread_cache.misses = 0;
    }
}

// Optional: Clean expired entries from current thread's cache
void
expire_old_cache_entries (struct filter_cache *cache)
{
  if (!cache->initialized)
    return;

  uint64_t current_time = journal_get_current_time ();
  if (current_time == 0)
    return;			// Can't expire without time

  for (int i = 0; i < FILTER_CACHE_SIZE; i++)
    {
      struct filter_cache_entry *entry = &cache->entries[i];
      if (entry->hash != 0 && is_entry_expired (entry, current_time))
	{
	  memset (entry, 0, sizeof (*entry));
	}
    }
}

// Optional: Get cache statistics for current thread
void
get_filter_cache_stats (struct filter_cache *cache, int *total_slots, int *used_slots, int *expired_slots,
			uint64_t *hits, uint64_t *misses)
{
  uint64_t current_time = journal_get_current_time ();

  *total_slots = FILTER_CACHE_SIZE;
  *used_slots = 0;
  *expired_slots = 0;
  *hits = cache->hits;
  *misses = cache->misses;

  if (!cache->initialized)
    return;

  for (int i = 0; i < FILTER_CACHE_SIZE; i++)
    {
      struct filter_cache_entry *entry = &cache->entries[i];
      if (entry->hash != 0)
	{
	  (*used_slots)++;
	  if (current_time != 0 && is_entry_expired (entry, current_time))
	    {
	      (*expired_slots)++;
	    }
	}
    }
}

