/* JBD2 binary compliant journal driver.

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

#include "ext2fs.h"

#ifndef JOURNAL_DEBUG
#define JOURNAL_DEBUG 1		/* Set to enable (very chatty) debug messages. */
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
typedef struct journal_transaction journal_transaction_t;

/* Initialize the journal subsystem using the inode provided (usually Inode 8). */
journal_t *journal_create (struct node *journal_inode);

/* Clean up and free the journal resources. */
void journal_destroy (journal_t * journal);


/**
 * Start tx: Ensure a valid running transaction exists.
 * Must be called before modifying any metadata.
 * Increments the transaction update count.
 */
error_t
journal_start_transaction (journal_t *journal, journal_transaction_t **out_txn);


/**
 * Mark dirty: Add a modified filesystem block to the current transaction.
 * Performs a shadow copy of 'data' into the journal memory.
 */
error_t
journal_dirty_block (journal_t * journal, block_t fs_blocknr,
		     const void *data);

/**
 * Stop tx: Decrement the transaction update count.
 * When the count reaches zero, the transaction is eligible for commit.
 */
void
journal_stop_transaction (journal_t *journal, journal_transaction_t *txn);

/**
 * Commit: Force the current running transaction to the log.
 * This contains the write barriers (flush_to_disk) that guarantee durability.
 */
error_t journal_commit_transaction (journal_t * journal);

void journal_notify_block_written (journal_t * journal, block_t blocknr);

/* Check if a block is currently pinned in a running transaction. */
int journal_block_is_active (journal_t * journal, block_t blocknr);

#endif //_JOURNAL_H
