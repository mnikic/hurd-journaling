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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ext2fs.h"
#include "jbd2_format.h"
#include "journal.h"

static pthread_t kjournald_tid;

extern void journal_sync_everything (void);

/**
 * Represents one modified block (4KB) that needs to be written to the journal.
 */
typedef struct journal_buffer
{
  block_t jb_blocknr;			/* The physical block number on the filesystem */
  void *jb_shadow_data;			/* 4KB Copy of the data to be logged */
  struct journal_buffer *jb_next;	/* Linked list next pointer */
  uint32_t jb_log_spot;
} journal_buffer_t;

/* The state of a transaction in memory */
typedef enum
{
  T_RUNNING,				/* Accepting new handles/buffers */
  T_LOCKED,				/* Locked, no new handles, waiting for updates to finish */
  T_FLUSHING,				/* Writing to the journal ring buffer */
  T_COMMIT,				/* Writing the commit block */
  T_FINISHED				/* Done, waiting to be checkpointed */
} transaction_state_t;

/* The Transaction Object */
struct journal_transaction
{
  uint32_t t_tid;			/* Transaction ID (Sequence Number) */
  transaction_state_t t_state;

  /* The Log Position */
  uint32_t t_log_start;			/* Where this transaction starts in the ring */
  uint32_t t_nr_blocks;			/* How many blocks it consumes */

  uint32_t t_updates;			/* Refcount: How many threads are in this transaction? */
  
  /* The Payload (The Shadow Buffers) */
  journal_buffer_t *t_buffers;		/* Linked List of dirty blocks */
  int t_buffer_count;

  /* Timing/Debug */
  long t_start_time;
};

/* The Simple Mapper (Virtual -> Physical) */
typedef struct journal_map
{
  block_t *phys_blocks;			/* The 64KB array we malloc'd */
  uint32_t total_blocks;		/* 16384 */
  struct node *inode;			/* Inode 8 (for keeping ref) */
} journal_map_t;

/* The Grand Abstraction */
typedef struct journal
{
  /* The Physics of it (The Map) */
  journal_map_t map;

  /* The Ring Buffer State (The Logic) */
  uint32_t j_head;			/* Where we are writing next */
  uint32_t j_tail;			/* The oldest live transaction (checkpoint) */
  uint32_t j_first;			/* First block of data (usually 1, after SB) */
  uint32_t j_last;			/* Last block of data */
  uint32_t j_free;			/* How many blocks left? */

  /* The Sequence Counter */
  uint32_t j_transaction_sequence;	/* Monotonic ID (e.g. 500, 501...) */

  pthread_mutex_t j_state_lock;	/* Protects the pointers below */
  /* The Transactions */
  struct journal_transaction *j_running_transaction;	/* Currently filling */
  struct journal_transaction *j_committing_transaction;	/* Flushing to journal */

} journal_t;

static void
init_map (journal_t *journal, struct node *jnode)
{
  journal->map.total_blocks = jnode->allocsize / block_size;
  journal->map.phys_blocks =
    malloc (journal->map.total_blocks * sizeof (block_t));
  if (!journal->map.phys_blocks)
    ext2_panic ("No RAM for journal map");

  for (uint32_t i = 0; i < journal->map.total_blocks; i++)
    {
      block_t phys = 0;

      /* ext2_getblk handles the indirect blocks/fragmentation. */
      error_t err = ext2_getblk (jnode, i, 0, &phys);

      if (err || phys == 0)
	{
	  ext2_panic ("[JOURNAL] Gap in journal file at logical %u!", i);
	}

      journal->map.phys_blocks[i] = phys;
    }

  journal->map.inode = jnode;
}

static void
destroy_map (journal_t *journal)
{
  free (journal->map.phys_blocks);
  journal->map.total_blocks = 0;
  if (journal->map.inode)
    diskfs_nput (journal->map.inode);
}

static void *
kjournald_thread (void *arg)
{
  journal_t *journal = (journal_t *) arg;
  while (1)
    {
      sleep (5);

      if (journal->j_running_transaction)
	{
	  JRNL_LOG_DEBUG ("Woke the journal up:\n"
			  " - Sequence: %u\n"
			  " - Start (Head): %u\n"
			  " - First Data Block: %u\n"
			  " - Total Blocks: %u",
			  journal->j_transaction_sequence, journal->j_head,
			  journal->j_first, journal->j_last);

	  // "Lightweight" commit - only writes the log
	  journal_commit_transaction (journal);
	}
    }
  return NULL;
}

static block_t
get_journal_phys_block (journal_t *journal, uint32_t idx)
{
  assert_backtrace (idx < journal->map.total_blocks);
  return journal->map.phys_blocks[idx];
}

/* Centralized logic to map FS Block -> Store Offset */
static store_offset_t
journal_map_offset (journal_t *journal, uint32_t logical_idx)
{
  block_t phys_block = get_journal_phys_block (journal, logical_idx);
  return phys_block << (log2_block_size - store->log2_block_size);
}

/**
 * Reads the JBD2 superblock (Block 0 of the journal file)
 * and initializes the journal_t state.
 */
error_t
journal_load_superblock (journal_t *journal)
{
  error_t err;
  journal_superblock_t *jsb;
  void *buf;

  buf = malloc (block_size);
  if (!buf)
    return ENOMEM;

  /* journal_read_block handles all the store_read/vm_deallocate logic internally */
  err = journal_read_block (journal, 0, buf);

  if (err)
    {
      JRNL_LOG_DEBUG ("[JOURNAL] Failed to read SB. Err: %s", strerror (err));
      free (buf);
      return err;
    }

  /* Interpret as JBD2 Superblock and verify */
  jsb = (journal_superblock_t *) buf;
  uint32_t magic = be32toh (jsb->s_header[0]);
  uint32_t type = be32toh (jsb->s_header[1]);
  if (magic != JBD2_MAGIC_NUMBER)
    {
      ext2_warning ("[JOURNAL] Invalid Magic: %x (Expected %x)", magic,
		    JBD2_MAGIC_NUMBER);
      free (buf);
      return EINVAL;
    }
  if (type != JBD2_SUPERBLOCK_V2 && type != JBD2_SUPERBLOCK_V1)
    {
      ext2_warning ("[JOURNAL] Invalid SB Type: %d", type);
      free (buf);
      return EINVAL;
    }

  /* Populate Journal Struct */
  journal->j_first = be32toh (jsb->s_first);
  journal->j_last = be32toh (jsb->s_maxlen);
  journal->j_head = be32toh (jsb->s_start);
  journal->j_tail = journal->j_head;
  journal->j_transaction_sequence = be32toh (jsb->s_sequence);

  /* Validate blocksize */
  uint32_t j_bsize = be32toh (jsb->s_blocksize);
  if (j_bsize != block_size)
    {
      ext2_warning ("[JOURNAL] Blocksize mismatch! Journal: %u, FS: %u",
		    j_bsize, block_size);
      free (buf);
      return EINVAL;
    }

  JRNL_LOG_DEBUG ("Loaded JBD2 Superblock:\n"
		  " - Sequence: %u\n"
		  " - Start (Head): %u\n"
		  " - First Data Block: %u\n"
		  " - Total Blocks: %u",
		  journal->j_transaction_sequence, journal->j_head,
		  journal->j_first, journal->j_last);

  free (buf);
  return 0;
}

error_t
journal_update_superblock (journal_t *journal, uint32_t sequence,
			   uint32_t start)
{
  void *buf;
  journal_superblock_t *jsb;
  error_t err;

  buf = malloc (block_size);
  if (!buf)
    return ENOMEM;

  /* Read existing SB to preserve UUID/Features */
  err = journal_read_block (journal, 0, buf);
  if (err)
    {
      JRNL_LOG_DEBUG ("[SB] Critical: Failed to read SB. Aborting update.");
      free (buf);
      return err;
    }

  jsb = (journal_superblock_t *) buf;

  /* Sanity Check Magic (Don't overwrite garbage with garbage) */
  if (jsb->s_header[0] != htobe32 (JBD2_MAGIC_NUMBER))
    {
      JRNL_LOG_DEBUG ("[SB] Critical: On-disk magic invalid. Aborting.");
      free (buf);
      return EIO;
    }

  /* Update Dynamic Fields */
  jsb->s_sequence = htobe32 (sequence);
  jsb->s_start = htobe32 (start);

  /* Ensure maxlen matches the map (self-correction) */
  jsb->s_maxlen = htobe32 (journal->map.total_blocks);

  JRNL_LOG_DEBUG ("[SB] Updating: Seq %u, Head %u", sequence, start);

  /* Write Back */
  err = journal_write_block (journal, 0, buf);

  free (buf);
  return err;
}

journal_t *
journal_create (struct node *journal_inode)
{
  journal_t *j = calloc (1, sizeof (struct journal));
  if (!j)
    ext2_panic ("[JOURNAL] Cannot create journal struct.");

  init_map (j, journal_inode);

  /* Take ownership of the inode ref */
  diskfs_nref (journal_inode);

  /* Set generic defaults (Will be overwritten by Superblock read later) */
  j->j_first = 1;		/* Skip SB block by default */
  j->j_last = j->map.total_blocks - 1;
  j->j_free = j->j_last - j->j_first;

  if (journal_load_superblock (j) != 0)
    {
      ext2_panic ("[JOURNAL] Failed to load superblock!");
    }
  pthread_mutex_init (&j->j_state_lock, NULL);

  if (pthread_create (&kjournald_tid, NULL, kjournald_thread, j) != 0)
    {
      JRNL_LOG_DEBUG ("Failed to create a flusher thread.");
    }
  else
    {
      JRNL_LOG_DEBUG ("Created flusher thread.");
    }
  return j;
}

/**
 * Writes a full filesystem block (4096 bytes) to the journal.
 * Handles the Logical -> Physical -> Store Offset conversion.
 */
error_t
journal_write_block (journal_t *journal, uint32_t logical_idx, void *data)
{
  store_offset_t offset;
  size_t written_amount = 0;
  error_t err;

  /* Safety Check */
  if (logical_idx >= journal->map.total_blocks)
    {
      ext2_warning ("[JOURNAL] Write out of bounds! Index: %u, Max: %u",
		    logical_idx, journal->map.total_blocks);
      return EINVAL;
    }

  offset = journal_map_offset (journal, logical_idx);
  err = store_write (store, offset, data, block_size, &written_amount);

  if (err)
    {
      JRNL_LOG_DEBUG
	("[JOURNAL] Write failed at logical %u. Err: %s",
	 logical_idx, strerror (err));
      return err;
    }

  if (written_amount != block_size)
    {
      JRNL_LOG_DEBUG ("[JOURNAL] Short write! Wanted %u, wrote %lu",
		      block_size, written_amount);
      return EIO;
    }

  return 0;
}

/**
 * Reads a full filesystem block (4096 bytes) from the journal into 'out_buf'.
 * out_buf must be at least block_size bytes.
 */
error_t
journal_read_block (journal_t *journal, uint32_t logical_idx, void *out_buf)
{
  store_offset_t offset;
  size_t read_amount = 0;
  void *temp_buf = NULL;
  error_t err;

  if (!out_buf)
    return EINVAL;

  if (logical_idx >= journal->map.total_blocks)
    {
      ext2_warning ("[JOURNAL] Read out of bounds! Index: %u, Max: %u",
		    logical_idx, journal->map.total_blocks);
      return EINVAL;
    }

  offset = journal_map_offset (journal, logical_idx);

  err = store_read (store, offset, block_size, &temp_buf, &read_amount);

  if (err)
    {
      if (temp_buf)
	vm_deallocate (mach_task_self (), (vm_address_t) temp_buf,
		       read_amount);
      return err;
    }

  if (read_amount != block_size)
    {
      JRNL_LOG_DEBUG ("[JOURNAL] Short read! Wanted %u, got %lu", block_size,
		      read_amount);
      if (temp_buf)
	vm_deallocate (mach_task_self (), (vm_address_t) temp_buf,
		       read_amount);
      return EIO;
    }

  memcpy (out_buf, temp_buf, block_size);
  vm_deallocate (mach_task_self (), (vm_address_t) temp_buf, read_amount);
  return 0;
}

void
journal_destroy (journal_t *journal)
{
  destroy_map (journal);
  pthread_mutex_destroy (&journal->j_state_lock);

  free (journal);
}

/**
 * Called when we are running out of space.
 * Since we do a version of sync() on every commit, we can safely declare all 
 * previous transactions "checkpointed" and reset the log.
 */
static void
journal_force_checkpoint (journal_t *journal, uint32_t current_tid)
{
  JRNL_LOG_DEBUG
    ("[CHECKPOINT] Journal Full! Forcing Global Sync & Reset...");

  journal_sync_everything ();

  journal->j_tail = journal->j_head;
  journal->j_free = journal->j_last - journal->j_first;

  /* Update Superblock */
  journal_update_superblock (journal, current_tid,
			     journal->j_head);
  JRNL_LOG_DEBUG ("[CHECKPOINT] after superblock...");

  JRNL_LOG_DEBUG
    ("[CHECKPOINT] Reset complete. Tail moved to %u. Free space restored.",
     journal->j_tail);
}

static uint32_t
journal_next_log_block (journal_t *journal)
{
  journal->j_head++;
  if (journal->j_head > journal->j_last)
    {
      journal->j_head = journal->j_first;
    }
  journal->j_free--;
  return journal->j_head;
}

/* Helper to calculate where the next block is, handling the ring buffer wrap.
   Must match journal_next_log_block logic exactly! */
static uint32_t
journal_next_after (journal_t *journal, uint32_t current_block)
{
  uint32_t next = current_block + 1;
  /* Wrap around to the first usable block */
  if (next > journal->j_last)
    next = journal->j_first;
  return next;
}

error_t
journal_commit_transaction (journal_t *journal)
{
  struct journal_transaction *txn;
  void *descriptor_buf = NULL, *commit_buf = NULL;
  journal_header_t *hdr;
  error_t err;
  uint32_t descriptor_loc, commit_loc;
  uint32_t tag_offset;
  journal_buffer_t *jb;

  pthread_mutex_lock (&journal->j_state_lock);
  txn = journal->j_running_transaction;

  if (!txn || txn->t_state != T_RUNNING)
    {
      pthread_mutex_unlock (&journal->j_state_lock);
      return EINVAL;
    }

  /* Detach from global state (Stop NEW writers) */
  journal->j_running_transaction = NULL;
  txn->t_state = T_LOCKED;

  journal->j_transaction_sequence = txn->t_tid + 1;

  /* DRAIN WRITERS */
  while (txn->t_updates > 0)
    {
      JRNL_LOG_DEBUG ("[COMMIT] Waiting for %u active handles...",
		      txn->t_updates);
      pthread_mutex_unlock (&journal->j_state_lock);
      sched_yield ();
      pthread_mutex_lock (&journal->j_state_lock);
    }

  txn->t_state = T_FLUSHING;

  /* RESERVATION */
  if (journal->j_free < txn->t_nr_blocks + 50)
    {
      if (txn->t_nr_blocks > (journal->j_last - journal->j_first))
	{
	  ext2_panic ("[COMMIT] Transaction too huge (%u) for journal!",
		      txn->t_nr_blocks);
	}

      journal_force_checkpoint (journal, txn->t_tid);
    }
  /* Reserve Descriptor */
  descriptor_loc = journal_next_log_block (journal);

  /* Reserve Data */
  jb = txn->t_buffers;
  uint32_t expected = journal_next_after (journal, descriptor_loc);

  while (jb)
    {
      jb->jb_log_spot = journal_next_log_block (journal);
      if (jb->jb_log_spot != expected)
	{
	  ext2_panic ("[COMMIT] Layout Logic Error! Expected %u got %u",
		      expected, jb->jb_log_spot);
	}
      expected = journal_next_after (journal, expected);

      jb = jb->jb_next;
    }

  /* Reserve Commit */
  commit_loc = journal_next_log_block (journal);
  if (commit_loc != expected)
    {
      ext2_panic ("[COMMIT] Layout Logic Error! Commit expected %u got %u",
		  expected, commit_loc);
    }

  pthread_mutex_unlock (&journal->j_state_lock);


  /* WRITE DESCRIPTOR FIRST */
  descriptor_buf = calloc (1, block_size);
  if (!descriptor_buf)
    {
      err = ENOMEM;
      goto out;
    }

  hdr = (journal_header_t *) descriptor_buf;
  hdr->h_magic = htobe32 (JBD2_MAGIC_NUMBER);
  hdr->h_blocktype = htobe32 (JBD2_DESCRIPTOR_BLOCK);
  hdr->h_sequence = htobe32 (txn->t_tid);

  tag_offset = sizeof (journal_header_t);
  jb = txn->t_buffers;

  while (jb)
    {
      if (tag_offset + sizeof (journal_block_tag_t) > block_size)
	{
	  err = E2BIG;
	  goto out;
	}

      journal_block_tag_t *tag =
	(journal_block_tag_t *) ((char *) descriptor_buf + tag_offset);
      tag->t_blocknr = htobe32 (jb->jb_blocknr);

      uint32_t flags = JBD2_FLAG_SAME_UUID;
      if (jb->jb_next == NULL)
	flags |= JBD2_FLAG_LAST_TAG;
      tag->t_flags = htobe32 (flags);

      jb = jb->jb_next;
      tag_offset += sizeof (journal_block_tag_t);
    }

  JRNL_LOG_DEBUG ("[COMMIT] Writing Descriptor to %u", descriptor_loc);
  err = journal_write_block (journal, descriptor_loc, descriptor_buf);
  free (descriptor_buf);
  descriptor_buf = NULL;
  if (err)
    goto out;


  /* WRITE DATA BLOCKS NEXT */
  jb = txn->t_buffers;
  while (jb)
    {
      err =
	journal_write_block (journal, jb->jb_log_spot, jb->jb_shadow_data);
      if (err)
	goto out;
      jb = jb->jb_next;
    }


  /* ONLY THEN WRITE A COMMIT BLOCK */
  commit_buf = calloc (1, block_size);
  if (!commit_buf)
    {
      err = ENOMEM;
      goto out;
    }

  hdr = (journal_header_t *) commit_buf;
  hdr->h_magic = htobe32 (JBD2_MAGIC_NUMBER);
  hdr->h_blocktype = htobe32 (JBD2_COMMIT_BLOCK);
  hdr->h_sequence = htobe32 (txn->t_tid);

  JRNL_LOG_DEBUG ("[COMMIT] Writing Commit Block at %u", commit_loc);
  err = journal_write_block (journal, commit_loc, commit_buf);
  JRNL_LOG_DEBUG ("[COMMIT] Done with Commit Block at %u", commit_loc);
  free (commit_buf);
  commit_buf = NULL;

  /* BARRIER & UPDATE at the end */
  if (!err)
    {
      if (journal->j_tail == 0)
	{
	  JRNL_LOG_DEBUG ("[COMMIT] First Time: Anchoring Tail at Block %u",
			  journal->j_first);

	  journal_update_superblock (journal, txn->t_tid, journal->j_first);
	  journal->j_tail = journal->j_first;
	}
    }

out:
  if (descriptor_buf)
    free (descriptor_buf);
  if (commit_buf)
    free (commit_buf);

  journal_buffer_t *curr = txn->t_buffers;
  while (curr)
    {
      journal_buffer_t *next = curr->jb_next;
      free (curr->jb_shadow_data);
      free (curr);
      curr = next;
    }
  free (txn);

  return err;
}

/**
 * Ensures there is a VALID running transaction to attach to.
 * Returns 0 on success, or error code.
 */
error_t
journal_start_transaction (journal_t *journal)
{
  struct journal_transaction *txn;

  if (!journal)
    return EINVAL;
  pthread_mutex_lock (&journal->j_state_lock);
  txn = journal->j_running_transaction;
  if (txn && txn->t_state == T_RUNNING)
    {
      txn->t_updates++;
    }
  else
    {
      /* TODO: Is the *previous* transaction still committing?
         If so, we might need to wait if we run out of space.
         For now, assume we have infinite space.) */
      txn = calloc (1, sizeof (struct journal_transaction));
      if (!txn)
	{
	  pthread_mutex_unlock (&journal->j_state_lock);
	  return ENOMEM;
	}

      txn->t_tid = journal->j_transaction_sequence;
      txn->t_state = T_RUNNING;
      txn->t_updates = 1;

      journal->j_running_transaction = txn;
      JRNL_LOG_DEBUG ("[TRX] Created NEW TID %u", txn->t_tid);
    }

  pthread_mutex_unlock (&journal->j_state_lock);
  return 0;
}

void
journal_stop_transaction (journal_t *journal)
{
  struct journal_transaction *txn;

  if (!journal)
    return;

  pthread_mutex_lock (&journal->j_state_lock);

  txn = journal->j_running_transaction;
  if (!txn)
    {
      ext2_warning
	("[TRX] stop_transaction called but no transaction running!");
      pthread_mutex_unlock (&journal->j_state_lock);
      return;
    }

  txn->t_updates--;
  pthread_mutex_unlock (&journal->j_state_lock);
}

/**
 * Adds a modified filesystem block to the current running transaction.
 * Performs a "Shadow Copy" of the data immediately.
 */
error_t
journal_dirty_block (journal_t *journal, block_t fs_blocknr, const void *data)
{
  struct journal_transaction *txn;
  journal_buffer_t *jb;
  journal_buffer_t *new_jb;

  if (!journal || !data)
    return EINVAL;

  pthread_mutex_lock (&journal->j_state_lock);

  txn = journal->j_running_transaction;

  if (!txn || txn->t_state != T_RUNNING)
    {
      JRNL_LOG_DEBUG
	("[ERROR] journal_dirty_block called outside of transaction!");
      pthread_mutex_unlock (&journal->j_state_lock);
      return EPERM;
    }

  /* TODO: Optimization: linear scan is fine for now. In prod use hash tables. */
  jb = txn->t_buffers;
  while (jb)
    {
      if (jb->jb_blocknr == fs_blocknr)
	{
	  memcpy (jb->jb_shadow_data, data, block_size);
	  pthread_mutex_unlock (&journal->j_state_lock);
	  return 0;
	}
      jb = jb->jb_next;
    }

  new_jb = malloc (sizeof (journal_buffer_t));
  if (!new_jb)
    {
      pthread_mutex_unlock (&journal->j_state_lock);
      return ENOMEM;
    }

  new_jb->jb_shadow_data = malloc (block_size);
  if (!new_jb->jb_shadow_data)
    {
      free (new_jb);
      pthread_mutex_unlock (&journal->j_state_lock);
      return ENOMEM;
    }

  new_jb->jb_blocknr = fs_blocknr;
  memcpy (new_jb->jb_shadow_data, data, block_size);

  new_jb->jb_next = txn->t_buffers;
  txn->t_buffers = new_jb;
  txn->t_buffer_count++;
  txn->t_nr_blocks++;

  pthread_mutex_unlock (&journal->j_state_lock);
  return 0;
}

/** 
 * Check if a specific filesystem block is currently part of the
 * Running Transaction.
 * * Returns: 1 if the block is "pinned" (must not be written to disk yet),
 * 0 if it is safe to write.
 */
int
journal_block_is_active (journal_t *journal, block_t blocknr)
{
  struct journal_transaction *txn;
  journal_buffer_t *jb;
  int is_active = 0;

  if (!journal)
    return 0;

  pthread_mutex_lock (&journal->j_state_lock);
  txn = journal->j_running_transaction;

  if (txn && txn->t_state == T_RUNNING)
    {
      /** TODO: hash map please! */
      jb = txn->t_buffers;
      while (jb)
	{
	  if (jb->jb_blocknr == blocknr)
	    {
	      is_active = 1;
	      break;
	    }
	  jb = jb->jb_next;
	}
    }

  pthread_mutex_unlock (&journal->j_state_lock);
  return is_active;
}
