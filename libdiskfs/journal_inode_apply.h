#ifndef JOURNAL_INODE_APPLY_H
#define JOURNAL_INODE_APPLY_H

#include <error.h>
#include <libdiskfs/journal_graph.h>

error_t apply_inode_state_hurd(const inode_state_t *inode);

#endif
