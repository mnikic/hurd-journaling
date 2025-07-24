#include <libdiskfs/journal_filter.h>

#include <stdatomic.h>

#define FILTER_TABLE_SIZE 2047 // Number of entries in the journal
#define CTIME_MIN_DELTA_SEC 1

typedef struct {
  journal_ino_t ino;
  atomic_long last_logged;  // time_t is long
} journal_filter_entry_t;

static journal_filter_entry_t filter_table[FILTER_TABLE_SIZE];

bool
journal_filter_should_log(journal_ino_t ino, time_t ctime)
{
  size_t idx = ino % FILTER_TABLE_SIZE;
  journal_filter_entry_t *entry = &filter_table[idx];

  // Fast path: different inode, insert or evict
  if (entry->ino != ino)
    {
      entry->ino = ino;
      atomic_store_explicit(&entry->last_logged, ctime, memory_order_relaxed);
      return true;
    }

  time_t last = atomic_load_explicit(&entry->last_logged, memory_order_relaxed);

  if ((ctime - last) < CTIME_MIN_DELTA_SEC)
    return false;

  // Attempt to update only if our value is newer (benign race if we lose)
  atomic_store_explicit(&entry->last_logged, ctime, memory_order_relaxed);
  return true;
}

