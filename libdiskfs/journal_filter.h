#ifndef JOURNAL_FILTER_H
#define JOURNAL_FILTER_H

#include <libdiskfs/journal_format.h>

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

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
bool journal_filter_should_log (journal_ino_t ino, time_t ctime);

#endif // JOURNAL_FILTER_H
