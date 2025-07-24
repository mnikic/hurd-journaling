#include <libdiskfs/journal_filter.h>

#include <stdatomic.h>

#define FILTER_TABLE_SIZE 2047	// Number of entries in the journal
#define ATIME_MIN_DELTA_SEC 1

typedef struct __attribute__((aligned (64)))
{
  atomic_uint_fast64_t ino;
  atomic_long last_logged;
} journal_filter_entry_t;


static journal_filter_entry_t filter_table[FILTER_TABLE_SIZE];

static inline size_t
hash_ino (journal_ino_t ino)
{
  // Truncate to 32 bits in case journal_ino_t is ever widened to 64 bits.
  // Knuth's multiplicative hash constant is designed for 32-bit hashing.
  uint32_t ino32 = (uint32_t) ino;

  // Hash and extract the top 11 bits for a 2048-entry table
  return (ino32 * 2654435761U) >> (32 - 11);
}

bool
journal_filter_should_log (journal_ino_t ino, time_t atime)
{
  size_t idx = hash_ino (ino);
  journal_filter_entry_t *entry = &filter_table[idx];

  journal_ino_t current_ino =
    atomic_load_explicit (&entry->ino, memory_order_relaxed);
  if (current_ino != ino)
    {
      atomic_store_explicit (&entry->ino, ino, memory_order_relaxed);
      atomic_store_explicit (&entry->last_logged, atime,
			     memory_order_relaxed);
      return true;
    }

  time_t last =
    atomic_load_explicit (&entry->last_logged, memory_order_relaxed);
  if ((atime - last) < ATIME_MIN_DELTA_SEC)
    return false;

  atomic_store_explicit (&entry->last_logged, atime, memory_order_relaxed);
  return true;
}
