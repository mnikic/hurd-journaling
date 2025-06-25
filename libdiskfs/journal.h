#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdbool.h>
#include <sys/types.h>

struct journal_entry_info {
    const char *action;      // "sync", "create", "unlink", etc.
    const char *name;        // filename if available
    ino_t parent_ino;        // parent inode if known
    // Future: uid, gid, device, flags, etc.
};

void journal_init(void);
void journal_shutdown(void);
bool flush_journal_to_file(void);
void journal_log_metadata(void *node_ptr, const struct journal_entry_info *info);

#endif /* JOURNAL_H */
