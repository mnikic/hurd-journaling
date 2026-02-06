/* JBD2 binary compliant journal driver for ext2

   Implements "Writeback" journaling mode:
     - Metadata (Inodes, Bitmaps, Superblock) is journaled and crash-consistent.
     - File Data is written directly to disk (not journaled) and lacks
       explicit ordering guarantees relative to the metadata commit.
   This provides the best performance but allows for "stale data" in
   recently allocated blocks after a crash.

   Copyright (C) 2026 Free Software Foundation, Inc.
   Written by Milos Nikic.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#ifndef _JOURNAL_H
#define _JOURNAL_H

#include <stdint.h>
#include <stdio.h>
#include <libdiskfs/diskfs.h>

#include "ext2fs.h"

#ifndef JOURNAL_DEBUG
#define JOURNAL_DEBUG 0		/* Set to enable (very chatty) debug messages. */
#endif

#if JOURNAL_DEBUG
#define JRNL_LOG_DEBUG(fmt, ...)                                             \
    do {                                                                     \
        fprintf(stderr, "[JRNL][DEBUG] " fmt "\n", ##__VA_ARGS__);           \
        fflush(stderr);                                                      \
    } while (0)
#else
#define JRNL_LOG_DEBUG(fmt, ...) do { } while (0)
#endif

/* Opaque handle for the journal object */
typedef struct journal journal_t;

/* Initialize the journal subsystem using the inode provided (usually Inode 8). */
journal_t *journal_create (struct node *journal_inode);

/**
 * Safely marks the journal as clean on disk.
 * MUST only be called after sync_global(1) ensures no pager I/O is in flight,
 * otherwise asynchronous pager notifications will cause a Use-After-Free!
 */
void
journal_quiesce_checkpoints (void);

/**
 * Mark dirty: Add a modified filesystem block to the given transaction.
 * Performs a shadow copy of 'data' into the journal memory.
 */
error_t
journal_dirty_block (diskfs_transaction_t * txn,
		     block_t fs_blocknr, const void *data);

/**
 * Records a range of deleted blocks so they can be unpinned from older
 * checkpoint lists AFTER this transaction safely commits.
 */
void
journal_record_freed_blocks (block_t start, unsigned long count);

/**
 * Called by the pager BEFORE writing blocks to their permanent home.
 * Enforces WAL ordering for a range of blocks. If the Mach is under
 * extreme memory pressure and the journal is locked, this acts as a
 * pressure-relief valve and safely bypasses WAL to prevent OS deadlocks.
 */
void journal_ensure_blocks_journaled (block_t start_block, size_t n_blocks);

/**
 * Force the current running transaction to the log if journaling
 * is enabled. This contains the write barriers (flush_to_disk) that
 * guarantee durability. This function will behave almost identical
 * to the diskfs_journal_commit_transaction except that doesn't take
 * a transaction argument so that it always works on the currently
 * running transaction, if there is one.
 */
error_t journal_commit_running_transaction (void);

/**
 * Called by the Pager (store_write hook) after writing blocks to the main disk.
 * This notifies the journal that these blocks are now safely written so that
 * the journal can properly unpin them and advance the tail.
 * Bulk version to handle clustered pageouts efficiently.
 * This checks the checkpoint lists, but also checks the RUNNING and COMMITTING
 * transactions to catch blocks that were asynchronously dirtied by the VFS
 * while the Pager was busy performing the physical disk write.
 */
void journal_notify_blocks_written (block_t start_block, size_t n_blocks);

#endif //_JOURNAL_H
