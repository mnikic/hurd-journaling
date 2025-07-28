#ifndef JOURNAL_FILTER_H
#define JOURNAL_FILTER_H

#include <libdiskfs/journal_format.h>

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stdatomic.h>

typedef struct __attribute__((aligned (64)))
{
  atomic_uint_fast64_t ino;
  atomic_long last_logged;
} journal_filter_entry_t;

typedef struct journal_filter_instance
{
  size_t size;
  time_t min_delta_sec;
  journal_filter_entry_t *table;
} journal_filter_instance_t;

/**
 * journal_filter_should_log - Decide whether to log a metadata change
 *
 * This function tracks the last time an inode was journaled and enforces
 * a minimum delta (in seconds) between accepted updates for the same inode.
 *
 * @ino: inode number to check
 * @ctime: proposed ctime update
 *
 * Returns: true if update should be logged, false if filtered out
 */
bool journal_filter_should_log (journal_filter_instance_t * filter,
				journal_ino_t ino, time_t time);

#endif // JOURNAL_FILTER_H
