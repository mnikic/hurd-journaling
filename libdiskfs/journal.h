#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdbool.h>
#include <sys/types.h>

struct journal_entry_info {
    const char *action;            // "create", "unlink", "rename", etc.
    const char *name;              // Affected file name
    ino_t parent_ino;              // For actions involving directories
    const char *old_name;          // For rename
    const char *new_name;          // For rename
    ino_t src_parent_ino;          // For rename
    ino_t dst_parent_ino;          // For rename
    const char *extra;             // Optional free-form field (e.g. "chmod mode=0755")
    // Future: maybe uid/gid/mode/time deltas for richer logs
};

void journal_init(void);
void journal_shutdown(void);
bool flush_journal_to_file(void);
void journal_log_metadata(void *node_ptr, const struct journal_entry_info *info);

#endif /* JOURNAL_H */
