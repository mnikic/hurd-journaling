#include <libdiskfs/journal_filter.h>

#include <stdatomic.h>

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
journal_filter_should_log (journal_filter_instance_t * instance,
			   journal_ino_t ino, time_t now)
{
  if (!instance || !instance->table)
    return true;

  size_t idx = hash_ino (ino) % instance->size;
  journal_filter_entry_t *entry = &instance->table[idx];

  journal_ino_t current_ino =
    atomic_load_explicit (&entry->ino, memory_order_relaxed);

  if (current_ino != ino)
    {
      atomic_store_explicit (&entry->ino, ino, memory_order_relaxed);
      atomic_store_explicit (&entry->last_logged, now, memory_order_relaxed);
      return true;
    }

  time_t last =
    atomic_load_explicit (&entry->last_logged, memory_order_relaxed);

  if ((now - last) < instance->min_delta_sec)
    return false;

  atomic_store_explicit (&entry->last_logged, now, memory_order_relaxed);
  return true;
}
