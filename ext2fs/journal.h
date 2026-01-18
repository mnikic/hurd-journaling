/* JBD2 binary compliant journal driver. 

   Copyright (C) 2026 Free Software Foundation, Inc.
   Written by Milos Nikic.

   Converted for ext2fs by Miles Bader <miles@gnu.org>

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

#ifndef  _JOURNAL_H
#define _JOURNAL_H

#include <stdio.h>

#include "ext2fs.h"

#ifndef JOURNAL_DEBUG
#define JOURNAL_DEBUG 0		/* Set to enable (very chatty) debug messages. */
#endif

#if JOURNAL_DEBUG
#define JRNL_LOG_DEBUG(fmt, ...)                                          \
	do {                                                                       \
		fprintf(stderr, "[JRNL][DEBUG] " fmt "\n", ##__VA_ARGS__);           \
		fflush(stderr);                                                         \
	} while (0)
#else
#define JRNL_LOG_DEBUG(fmt, ...) do { } while (0)
#endif

/* Forward declaration only. The struct contents are hidden. */
typedef struct journal journal_t;

journal_t *journal_create (struct node *journal_inode);
error_t
journal_write_block (journal_t * journal, uint32_t logical_idx, void *data);
error_t
journal_read_block (journal_t * journal, uint32_t logical_idx, void *out_buf);
void journal_destroy (journal_t * journal);

error_t journal_start_transaction (journal_t * journal);
error_t journal_commit_transaction (journal_t * journal);
void journal_stop_transaction (journal_t * journal);

error_t
journal_dirty_block (journal_t * journal, block_t fs_blocknr,
		     const void *data);
int journal_block_is_active (journal_t * journal, block_t blocknr);

#endif //_JOURNAL_H
