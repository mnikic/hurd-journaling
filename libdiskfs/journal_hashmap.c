#ifndef JOURNAL_REPLAY_HASHMAP_H
#define JOURNAL_REPLAY_HASHMAP_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <hurd/types.h> // for ino_t
#include <libdiskfs/journal_format.h> // for struct journal_payload_bin

#define MAX_BUCKETS 1024

struct inode_entry_list {
  struct journal_payload_bin **entries;
  size_t count;
  size_t capacity;
};

struct inode_event_map {
  ino_t inode;
  struct inode_entry_list list;
  struct inode_event_map *next;
};

static struct inode_event_map *buckets[MAX_BUCKETS];

static unsigned int
hash_inode(ino_t inode)
{
  return (unsigned int)(inode % MAX_BUCKETS);
}

static void
append_inode_event(struct inode_entry_list *list, struct journal_payload_bin *entry)
{
  if (list->count == list->capacity)
    {
      size_t new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;
      list->entries = realloc(list->entries, new_capacity * sizeof(*list->entries));
      list->capacity = new_capacity;
    }
  list->entries[list->count++] = entry;
}

static void
add_event_to_map(struct journal_payload_bin *entry)
{
  unsigned int idx = hash_inode(entry->ino);
  struct inode_event_map *bucket = buckets[idx];
  while (bucket)
    {
      if (bucket->inode == entry->ino)
	{
	  append_inode_event(&bucket->list, entry);
	  return;
	}
      bucket = bucket->next;
    }
  // Not found, create new bucket entry
  struct inode_event_map *new_bucket = malloc(sizeof(*new_bucket));
  new_bucket->inode = entry->ino;
  new_bucket->list.entries = NULL;
  new_bucket->list.count = 0;
  new_bucket->list.capacity = 0;
  new_bucket->next = buckets[idx];
  buckets[idx] = new_bucket;
  append_inode_event(&new_bucket->list, entry);
}

static void
sort_all_inode_entries(void)
{
  for (size_t i = 0; i < MAX_BUCKETS; ++i)
    {
      struct inode_event_map *bucket = buckets[i];
      while (bucket)
	{
	  qsort(bucket->list.entries, bucket->list.count,
		 sizeof(struct journal_payload_bin *), compare_entries_by_time_then_txid);
	  bucket = bucket->next;
	}
    }
}

static int
compare_entries_by_time_then_txid(const void *a, const void *b)
{
  const struct journal_payload_bin *entry_a = *(const struct journal_payload_bin **)a;
  const struct journal_payload_bin *entry_b = *(const struct journal_payload_bin **)b;

  if (entry_a->timestamp_ms < entry_b->timestamp_ms)
    return -1;
  if (entry_a->timestamp_ms > entry_b->timestamp_ms)
    return 1;

  // Tie-breaker: lower tx_id wins
  if (entry_a->tx_id < entry_b->tx_id)
    return -1;
  if (entry_a->tx_id > entry_b->tx_id)
    return 1;

  return 0;
}

#endif // JOURNAL_REPLAY_HASHMAP_H
