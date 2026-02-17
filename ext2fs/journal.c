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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <hurd/ihash.h>

#include "ext2fs.h"
#include "jbd2_format.h"
#include "journal.h"

/* Journal Tuning Params */

/**
 * We limit a single transaction to 1/4 of the total journal size.
 * This ensures we can pipeline:
 * [ Committing Txn ] + [ Running Txn ] + [ Buffer/Wrap Space ]
 */
#define JRNL_MAX_TRANS_RATIO   4

/**
 * The minimum size (in blocks) of a transaction.
 * Below this, the overhead of commit records outweighs the data throughput.
 * 256 blocks = 1MB (assuming 4k blocks).
 */
#define JRNL_MIN_BATCH_BLOCKS  256

/**
 * Reserve space for metadata overhead (Descriptor blocks + Commit block).
 * 32 blocks allows for ~8000 data blocks to be described (approx),
 * which is plenty of safety margin for the size limits above.
 */
#define JRNL_METADATA_OVERHEAD 32

/**
 * Ratio for estimating descriptor blocks.
 * We estimate 1 descriptor block for every 32 data blocks.
 * (Real capacity is ~250 tags/block, so 32 is a very conservative/safe estimate).
 */
#define JRNL_DESCRIPTOR_RATIO  32

/**
 * Safety margin during commit.
 * Reserves space for: 1 Commit Block + 1 Descriptor Block + 3 blocks slop
 * to handle alignment/wrapping edge cases without hitting the tail.
 */
#define JRNL_COMMIT_MARGIN     5

/**
 * Low Water Mark Ratio.
 * If free space drops below 1/8th of the total journal, we force a checkpoint.
 * This ensures the NEXT transaction has breathing room to start.
 */
#define JRNL_LOW_WATER_RATIO   8

static pthread_t kjournald_tid;

/**
 * This function exists to sync all AND avoid a deadlock with commit.
 * It doesn't call journal_commit back yet it syncs everything.
 **/
extern void journal_sync_everything (void);

/**
 * Represents one modified block (4KB) that needs to be written to the journal.
 */
typedef struct journal_buffer
{
  block_t jb_blocknr;		/* The physical block number on the filesystem */
  void *jb_shadow_data;		/* 4KB Copy of the data to be logged */
  struct journal_buffer *jb_next;	/* Linked list next pointer */
  uint32_t jb_log_spot;
} journal_buffer_t;

/* The state of a transaction in memory */
typedef enum
{
  T_RUNNING,			/* Accepting new handles/buffers */
  T_LOCKED,			/* Locked, no new handles,waiting for updates to finish */
  T_FLUSHING,			/* Writing to the journal ring buffer */
  T_COMMIT,			/* Writing the commit block */
  T_FINISHED			/* Done, waiting to be checkpointed */
} transaction_state_t;

/* The Transaction Object */
typedef struct journal_transaction
{
  uint32_t t_tid;		/* Transaction ID (Sequence Number) */
  transaction_state_t t_state;

  /* The Log Position */
  uint32_t t_log_start;		/* Where this transaction starts in the ring */
  uint32_t t_nr_blocks;		/* How many blocks it consumes */

  uint32_t t_updates;		/* Refcount: How many threads are in this transaction? */

  /* The Payload (The Shadow Buffers) */
  journal_buffer_t *t_buffers;	/* Linked List of dirty blocks */
  int t_buffer_count;
  struct hurd_ihash t_buffer_map;	/* The Map (for O(1) lookups) */

  struct journal_transaction *t_checkpoint_next;	/* Next in global checkpoint list */
  int t_outstanding_io;

  /* Timing/Debug */
  long t_start_time;
} journal_transaction_t;

/* The Simple Mapper (Virtual -> Physical) */
typedef struct journal_map
{
  block_t *phys_blocks;		/* The 64KB array we malloc'd */
  uint32_t total_blocks;	/* 16384 */
  struct node *inode;		/* Inode 8 (for keeping ref) */
} journal_map_t;

/* The Grand Abstraction */
typedef struct journal
{
  /* The Physics of it (The Map) */
  journal_map_t map;

  /* The Ring Buffer State (The Logic) */
  uint32_t j_head;		/* Where we are writing next */
  uint32_t j_tail;		/* The oldest live transaction (checkpoint) */
  uint32_t j_first;		/* First block of data (usually 1, after SB) */
  uint32_t j_last;		/* Last block of data */
  uint32_t j_free;		/* How many blocks left? */

  /* The Sequence Counter */
  uint32_t j_transaction_sequence;	/* Monotonic ID (e.g. 500, 501...) */
  void *j_sb_buffer;		/* Buffer holding the journal superblock */

  pthread_mutex_t j_state_lock;	/* Protects the pointers below */
  pthread_cond_t j_commit_wait;	/* Cond. var. while waiting for the tx to be ready. */
  /* The Transactions */
  journal_transaction_t *j_running_transaction;	/* Currently filling */

  journal_transaction_t *j_checkpoint_list;	/* Head (Oldest, defines j_tail) */
  journal_transaction_t *j_checkpoint_last;

  uint32_t j_max_transaction_buffers;	/* Max size of a single transaction */
  uint32_t j_min_free;

  uint32_t j_last_committed_tid;	/* Transaction ID of the last committed txn. */
  pthread_cond_t j_commit_done;	/* Cond. var. while waiting for the tx to be committed. */
  int j_must_exit;		/* variable that tells journal thread when to stop. */
} journal_t;

static void
flush_to_disk (void)
{
  error_t err = store_sync (store);
  /* Ignore EOPNOTSUPP (drivers), but warn on real I/O errors */
  if (err && err != EOPNOTSUPP)
    ext2_warning ("device flush failed: %s", strerror (err));
}

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

      if (diskfs_readonly)
	continue;

      if (journal->j_running_transaction)
	{
	  JRNL_LOG_DEBUG ("Woke the journal up:\n"
			  " - Sequence: %u\n"
			  " - Start (Head): %u\n"
			  " - First Data Block: %u\n"
			  " - Total Blocks: %u",
			  journal->j_transaction_sequence, journal->j_head,
			  journal->j_first, journal->j_last);

	  journal_commit_transaction ();
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
 * Writes a full filesystem block (4096 bytes) to the journal.
 * Handles the Logical -> Physical -> Store Offset conversion.
 */
static error_t
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
static error_t
journal_read_block (journal_t *journal, uint32_t logical_idx, void *out_buf)
{
  store_offset_t offset;
  size_t read_amount = 0;
  error_t err;
  void *read_buf = out_buf;

  if (!out_buf)
    return EINVAL;

  if (logical_idx >= journal->map.total_blocks)
    {
      ext2_warning ("[JOURNAL] Read out of bounds! Index: %u, Max: %u",
		    logical_idx, journal->map.total_blocks);
      return EINVAL;
    }

  offset = journal_map_offset (journal, logical_idx);
  err = store_read (store, offset, block_size, &read_buf, &read_amount);

  if (err)
    {
      return err;
    }

  if (read_amount != block_size)
    {
      JRNL_LOG_DEBUG ("[JOURNAL] Short read! Wanted %u, got %lu", block_size,
		      read_amount);
      if (read_buf != out_buf)
	vm_deallocate (mach_task_self (), (vm_address_t) read_buf,
		       read_amount);
      return EIO;
    }

  if (read_buf != out_buf)
    {
      memcpy (out_buf, read_buf, block_size);
      vm_deallocate (mach_task_self (), (vm_address_t) read_buf, read_amount);
    }
  return 0;
}

/**
 * Reads the JBD2 superblock (Block 0 of the journal file)
 * and initializes the journal_t state.
 */
static error_t
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

  /* Check versions */
  if (type == JBD2_SUPERBLOCK_V1)
    {
      ext2_warning
	("[JOURNAL] Mounting V1 journal. 64-bit features disabled.");
      /* V1 ends at s_errno. Zero out all V2-specific fields. */
      jsb->s_feature_compat = 0;
      jsb->s_feature_incompat = 0;
      jsb->s_feature_ro_compat = 0;
      memset (jsb->s_uuid, 0, 16);
      jsb->s_nr_users = 0;
      jsb->s_dynsuper = 0;
      jsb->s_max_transaction = 0;
      jsb->s_max_trans_data = 0;
      jsb->s_checksum_type = 0;
      memset (jsb->s_padding2, 0, sizeof (jsb->s_padding2));
      jsb->s_checksum = 0;
      memset (jsb->s_users, 0, sizeof (jsb->s_users));
    }
  else if (type != JBD2_SUPERBLOCK_V2)
    {
      ext2_warning ("[JOURNAL] Invalid SB Type: %d", type);
      free (buf);
      return EINVAL;
    }

  /* Populate Journal Struct */
  journal->j_first = be32toh (jsb->s_first);
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
  jsb->s_maxlen = htobe32 (journal->map.total_blocks);

  journal->j_sb_buffer = buf;
  journal->j_last = journal->map.total_blocks - 1;
  journal->j_free = journal->j_last - journal->j_first;

  JRNL_LOG_DEBUG ("Loaded JBD2 Superblock:\n"
		  " - Sequence: %u\n"
		  " - Start (Head): %u\n"
		  " - First Data Block: %u\n"
		  " - Total Blocks: %u",
		  journal->j_transaction_sequence, journal->j_head,
		  journal->j_first, journal->j_last);
  return 0;
}

/* Updates superblock and flushes it to disk */
static error_t
journal_update_superblock (journal_t *journal, uint32_t sequence,
			   uint32_t start)
{
  error_t err;
  journal_superblock_t *jsb = (journal_superblock_t *) journal->j_sb_buffer;

  /* Update Dynamic Fields */
  jsb->s_sequence = htobe32 (sequence);
  jsb->s_start = htobe32 (start);

  JRNL_LOG_DEBUG ("[SB] Updating: Seq %u, Head %u", sequence, start);
  err = journal_write_block (journal, 0, jsb);
  if (err)
    return err;
  return 0;
}

/**
 * Frees the SHADOW copies of data to save RAM, but keeps the
 * transaction struct and metadata alive for tracking.
 */
static void
journal_strip_transaction (journal_transaction_t *txn)
{
  journal_buffer_t *jb = txn->t_buffers;
  while (jb)
    {
      /* We don't need the data copy anymore, it's in the log */
      if (jb->jb_shadow_data)
	{
	  free (jb->jb_shadow_data);
	  jb->jb_shadow_data = NULL;
	}
      jb = jb->jb_next;
    }
  /* NOTE: We do NOT destroy t_buffer_map yet! */
}

/* * Fully destroys the transaction structure.
 * Called only when checkpointing is 100% complete.
 */
static void
journal_free_transaction (journal_transaction_t *txn)
{
  journal_buffer_t *jb = txn->t_buffers;
  while (jb)
    {
      journal_buffer_t *next = jb->jb_next;
      /* shadow_data should be gone, but check to be safe */
      if (jb->jb_shadow_data)
	free (jb->jb_shadow_data);
      free (jb);
      jb = next;
    }
  hurd_ihash_destroy (&txn->t_buffer_map);
  free (txn);
}

/**
 * Checks if the oldest transaction(s) are fully written.
 * If so, frees them and advances the journal tail.
 */
static int
journal_try_advance_tail_locked (journal_t *journal)
{
  journal_transaction_t *txn = journal->j_checkpoint_list;
  int changed = 0;

  while (txn)
    {
      if (txn->t_outstanding_io > 0)
	{
	  /* Oldest txn is still pending. We can't advance past it. */
	  break;
	}

      /* This transaction is done! */
      JRNL_LOG_DEBUG ("[CHECKPOINT] TID %u fully written. Reclaiming space.",
		      txn->t_tid);

      /* Unlink from list */
      journal->j_checkpoint_list = txn->t_checkpoint_next;
      if (journal->j_checkpoint_list == NULL)
	journal->j_checkpoint_last = NULL;

      /* Calculate used space carefully handling wrap */
      if (txn->t_nr_blocks > 0)
	{
	  /* Simple approximation: Just rely on j_free math or re-calculate */
	  /* Better: The journal tail effectively becomes the log_start of the NEXT txn */
	  if (journal->j_checkpoint_list)
	    journal->j_tail = journal->j_checkpoint_list->t_log_start;
	  else
	    journal->j_tail = journal->j_head;	/* Empty journal */
	}

      /* Recompute free space (Global Truth) */
      uint32_t capacity = journal->j_last - journal->j_first + 1;
      uint32_t used_len = 0;
      if (journal->j_head >= journal->j_tail)
	used_len = journal->j_head - journal->j_tail;
      else
	used_len =
	  (journal->j_last - journal->j_tail + 1) + (journal->j_head -
						     journal->j_first);

      journal->j_free = capacity - used_len;

      /* Destroy the object */
      journal_free_transaction (txn);

      txn = journal->j_checkpoint_list;
      changed = 1;
    }
  return changed;
}

/*
 * Called by the Pager (store_write hook)
 */
void
journal_notify_block_written (block_t blocknr)
{
  int sb_changed = 0;
  if (!ext2_journal)
    return;

  pthread_mutex_lock (&ext2_journal->j_state_lock);

  /* Iterate over checkpoint list to find who owns this block */
  journal_transaction_t *txn = ext2_journal->j_checkpoint_list;

  while (txn)
    {
      /* Use the hash map for O(1) check */
      journal_buffer_t *jb =
	(journal_buffer_t *) hurd_ihash_find (&txn->t_buffer_map,
					      (hurd_ihash_key_t) blocknr);
      if (jb)
	{
	  /* Found it! This block is now safe. */

	  /* Remove from map so we don't count it twice */
	  hurd_ihash_remove (&txn->t_buffer_map, (hurd_ihash_key_t) blocknr);

	  if (txn->t_outstanding_io > 0)
	    txn->t_outstanding_io--;

	  /* Optimization: If this was the only block, try to advance tail immediately */
	  if (txn->t_outstanding_io == 0
	      && txn == ext2_journal->j_checkpoint_list)
	    {
	      journal_try_advance_tail_locked (ext2_journal);
	    }

	  /* A block usually belongs to only one checkpoint txn (the latest committed one).
	     We can stop searching. */
	  break;
	}
      txn = txn->t_checkpoint_next;
    }

  pthread_mutex_unlock (&ext2_journal->j_state_lock);
  if (sb_changed)
    {
      /* Update Superblock Persistently */
      /* Note: We are already holding the lock, and update_superblock might do I/O.
         But since we are passive, this is just a sector write, not a sync. */
      journal_update_superblock (ext2_journal,
				 ext2_journal->j_transaction_sequence,
				 ext2_journal->j_tail);
      flush_to_disk ();
      pthread_cond_broadcast (&ext2_journal->j_commit_done);
    }

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
  uint32_t total_len = j->j_last - j->j_first;
  j->j_free = total_len;

  j->j_max_transaction_buffers = total_len / JRNL_MAX_TRANS_RATIO;
  if (j->j_max_transaction_buffers < JRNL_MIN_BATCH_BLOCKS)
    j->j_max_transaction_buffers = JRNL_MIN_BATCH_BLOCKS;
  j->j_min_free = j->j_max_transaction_buffers + JRNL_METADATA_OVERHEAD;

  if (journal_load_superblock (j) != 0)
    {
      ext2_panic ("[JOURNAL] Failed to load superblock!");
    }
  j->j_last_committed_tid = j->j_transaction_sequence - 1;
  pthread_cond_init (&j->j_commit_done, NULL);
  pthread_mutex_init (&j->j_state_lock, NULL);
  pthread_cond_init (&j->j_commit_wait, NULL);
  j->j_must_exit = 0;
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

void
journal_destroy (journal_t *journal)
{
  pthread_mutex_lock (&journal->j_state_lock);
  journal->j_must_exit = 1;
  pthread_cond_broadcast (&journal->j_commit_wait);
  pthread_mutex_unlock (&journal->j_state_lock);

  pthread_join (kjournald_tid, NULL);

  destroy_map (journal);

  pthread_mutex_destroy (&journal->j_state_lock);
  pthread_cond_destroy (&journal->j_commit_wait);
  pthread_cond_destroy (&journal->j_commit_done);

  if (journal->j_sb_buffer)
    free (journal->j_sb_buffer);

  free (journal);
}

/**
 * Called when we are running out of space.
 * Since we do a version of sync() on every commit, we can safely declare all 
 * previous transactions "checkpointed" and reset the log.
 */
static void
journal_force_checkpoint_locked (journal_t *journal, uint32_t tid)
{
  JRNL_LOG_DEBUG ("[CHECKPOINT] Journal Full (Free: %u). Squeezing disk...",
		  journal->j_free);
  while (journal->j_free < journal->j_min_free)
    {
      pthread_mutex_unlock (&journal->j_state_lock);
      journal_sync_everything ();
      pthread_mutex_lock (&journal->j_state_lock);

      if (journal->j_free < journal->j_min_free)
	{
	  /* Wait for the specific signal from advance_tail */
	  pthread_cond_wait (&journal->j_commit_done, &journal->j_state_lock);
	}
    }

  JRNL_LOG_DEBUG ("[CHECKPOINT] Space reclaimed. Free: %u. Tail: %u",
		  journal->j_free, journal->j_tail);
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

static inline uint32_t
journal_next_log_block_safe (journal_t *journal)
{
  uint32_t block;
  pthread_mutex_lock (&journal->j_state_lock);
  block = journal_next_log_block (journal);
  pthread_mutex_unlock (&journal->j_state_lock);
  return block;
}

/* Helper to reset the header for a new block */
static void
setup_header (void *buf, const journal_transaction_t *txn,
	      uint32_t block_type)
{
  journal_header_t *h = (journal_header_t *) buf;
  h->h_magic = htobe32 (JBD2_MAGIC_NUMBER);
  h->h_blocktype = htobe32 (block_type);
  //h->h_blocktype = htobe32 (JBD2_DESCRIPTOR_BLOCK);
  h->h_sequence = htobe32 (txn->t_tid);
}

/* Writes the Descriptor Block + All Data Blocks (Escaped) */
static error_t
journal_write_payload (journal_t *journal, const journal_transaction_t *txn)
{
  if (txn->t_buffers == NULL)
    return 0;
  void *descriptor_buf = calloc (1, block_size);
  if (!descriptor_buf)
    return ENOMEM;

  setup_header (descriptor_buf, txn, JBD2_DESCRIPTOR_BLOCK);

  uint32_t tag_offset = sizeof (journal_header_t);
  journal_buffer_t *jb = txn->t_buffers;
  error_t err = 0;
  journal_buffer_t *batch_start = txn->t_buffers;
  uint32_t descriptor_loc = journal_next_log_block_safe (journal);
  while (jb)
    {
      if (tag_offset + sizeof (journal_block_tag_t) > block_size)
	{
	  journal_block_tag_t *prev_tag =
	    (journal_block_tag_t *) ((char *) descriptor_buf + tag_offset -
				     sizeof (journal_block_tag_t));
	  /* We need to OR the flag into the existing flags */
	  uint32_t prev_flags = be32toh (prev_tag->t_flags);
	  prev_tag->t_flags = htobe32 (prev_flags | JBD2_FLAG_LAST_TAG);

	  /* Write the current Descriptor */
	  JRNL_LOG_DEBUG ("[COMMIT] Writing Interleaved Descriptor to %u",
			  descriptor_loc);
	  if (journal_write_block (journal, descriptor_loc, descriptor_buf))
	    goto err_out;

	  /* Write the Data Blocks for this batch IMMEDIATELY */
	  journal_buffer_t *p = batch_start;
	  while (p != jb)
	    {
	      uint32_t data_loc = journal_next_log_block_safe (journal);
	      if (journal_write_block (journal, data_loc, p->jb_shadow_data))
		goto err_out;
	      p = p->jb_next;
	    }

	  /* Prepare for next batch */
	  descriptor_loc = journal_next_log_block_safe (journal);

	  memset (descriptor_buf, 0, block_size);
	  setup_header (descriptor_buf, txn, JBD2_DESCRIPTOR_BLOCK);
	  tag_offset = sizeof (journal_header_t);
	  batch_start = jb;
	}
      journal_block_tag_t *tag =
	(journal_block_tag_t *) ((char *) descriptor_buf + tag_offset);
      tag->t_blocknr = htobe32 (jb->jb_blocknr);

      uint32_t flags = JBD2_FLAG_SAME_UUID;
      if (jb->jb_next == NULL)
	flags |= JBD2_FLAG_LAST_TAG;

      /* Escaping Logic: If data looks like a header, mask it. */
      uint32_t *data_head = (uint32_t *) jb->jb_shadow_data;
      if (*data_head == htobe32 (JBD2_MAGIC_NUMBER))
	{
	  flags |= JBD2_FLAG_ESCAPE;
	  *data_head = 0;
	}
      tag->t_flags = htobe32 (flags);

      jb = jb->jb_next;
      tag_offset += sizeof (journal_block_tag_t);
    }

  /* Write Descriptor */
  JRNL_LOG_DEBUG ("[COMMIT] Writing final Descriptor to %u", descriptor_loc);
  err = journal_write_block (journal, descriptor_loc, descriptor_buf);
  if (err)
    goto err_out;

  /* Write Data Blocks */
  journal_buffer_t *p = batch_start;
  while (p)
    {
      uint32_t data_loc = journal_next_log_block_safe (journal);
      err = journal_write_block (journal, data_loc, p->jb_shadow_data);
      if (err)
	goto err_out;
      p = p->jb_next;
    }

err_out:
  free (descriptor_buf);
  return err;
}

/* Writes the Commit Block */
static error_t
journal_write_commit_record (journal_t *journal,
			     journal_transaction_t *txn, uint32_t commit_loc)
{
  void *commit_buf = calloc (1, block_size);
  if (!commit_buf)
    return ENOMEM;

  setup_header (commit_buf, txn, JBD2_COMMIT_BLOCK);
  error_t err = journal_write_block (journal, commit_loc, commit_buf);
  free (commit_buf);
  return err;
}

/* Cleans up the transaction. */
static error_t
journal_cleanup_transaction (journal_transaction_t *txn, error_t err)
{
  journal_buffer_t *jb = txn->t_buffers;
  while (jb)
    {
      journal_buffer_t *next = jb->jb_next;
      free (jb->jb_shadow_data);
      free (jb);
      jb = next;
    }
  hurd_ihash_destroy (&txn->t_buffer_map);
  free (txn);
  return err;
}

static error_t
journal_commit_transaction_locked (journal_t *journal,
				   journal_transaction_t *txn)
{
  error_t err = 0;
  uint32_t commit_loc;

  JRNL_LOG_DEBUG ("About to enter wait on cond for tx id: %u.", txn->t_tid);
  while (txn->t_updates > 0)
    pthread_cond_wait (&journal->j_commit_wait, &journal->j_state_lock);

  JRNL_LOG_DEBUG ("Entered after wait for tx id: %u.", txn->t_tid);
  txn->t_state = T_FLUSHING;

  uint32_t needed =
    txn->t_nr_blocks + (txn->t_nr_blocks / JRNL_DESCRIPTOR_RATIO) +
    JRNL_COMMIT_MARGIN;
  uint32_t low_water =
    (journal->j_last - journal->j_first) / JRNL_LOW_WATER_RATIO;

  if (journal->j_free < needed || journal->j_free < low_water)
    journal_force_checkpoint_locked (journal, txn->t_tid);

  pthread_mutex_unlock (&journal->j_state_lock);

  /* Write Data (I/O) */
  err = journal_write_payload (journal, txn);
  if (err)
    return journal_cleanup_transaction (txn, err);

  /* Ensure Data is on disk */
  flush_to_disk ();

  commit_loc = journal_next_log_block_safe (journal);
  /* Write Commit Record */
  err = journal_write_commit_record (journal, txn, commit_loc);
  if (err)
    return journal_cleanup_transaction (txn, err);

  /* Ensure Commit is persistent */
  flush_to_disk ();

  /* Finalize Metadata */
  pthread_mutex_lock (&journal->j_state_lock);

  if (journal->j_tail == 0)
    {
      journal->j_tail = journal->j_first;
      journal_update_superblock (journal, txn->t_tid, journal->j_first);
      flush_to_disk ();
    }
  journal->j_last_committed_tid = txn->t_tid;

  txn->t_state = T_FINISHED;

  /* The number of items we must wait for is the number of dirty buffers 
     we tracked in the hash map. */
  txn->t_outstanding_io = txn->t_buffer_count;
  txn->t_checkpoint_next = NULL;

  /* Drop the 4KB shadow buffers to save RAM. 
     We keep the journal_buffer_t headers and the t_buffer_map alive 
     so pending_blocks_write can find them later. */
  journal_strip_transaction (txn);

  /* Link into the Global Checkpoint Queue */
  if (journal->j_checkpoint_last)
    {
      journal->j_checkpoint_last->t_checkpoint_next = txn;
    }
  else
    {
      /* Queue was empty, we are the new Head */
      journal->j_checkpoint_list = txn;
    }
  journal->j_checkpoint_last = txn;

  /* Wake up everyone waiting in journal_wait_on_tid */
  pthread_cond_broadcast (&journal->j_commit_done);
  pthread_mutex_unlock (&journal->j_state_lock);

  JRNL_LOG_DEBUG ("Done done with tx id: %u", txn->t_tid);
  return 0;
}

error_t
journal_commit_transaction (void)
{
  journal_transaction_t *txn;

  pthread_mutex_lock (&ext2_journal->j_state_lock);
  txn = ext2_journal->j_running_transaction;

  if (!txn || txn->t_state != T_RUNNING)
    {
      pthread_mutex_unlock (&ext2_journal->j_state_lock);
      return EINVAL;
    }
  ext2_journal->j_running_transaction = NULL;
  txn->t_state = T_LOCKED;

  return journal_commit_transaction_locked (ext2_journal, txn);
}

/**
 * Ensures there is a VALID running transaction to attach to.
 * Returns 0 on success, or error code.
 */
error_t
journal_start_transaction (journal_transaction_t **out_txn)
{
  journal_transaction_t *txn;

  if (!ext2_journal)
    return EINVAL;

  pthread_mutex_lock (&ext2_journal->j_state_lock);

  /* If the journal is too full, we must wait for the pager 
     to flush older transactions and reclaim space. */
  while (ext2_journal->j_free < ext2_journal->j_min_free)
    {
      JRNL_LOG_DEBUG
	("[TRX] Journal full (Free: %u). Waiting for checkpoint...",
	 ext2_journal->j_free);

      /* We wait on j_commit_done. 
         MAKE SURE: journal_try_advance_tail calls broadcast on this! */
      pthread_cond_wait (&ext2_journal->j_commit_done,
			 &ext2_journal->j_state_lock);
    }

  txn = ext2_journal->j_running_transaction;

  if (txn)
    {
      if (txn->t_state != T_RUNNING)
	{
	  pthread_mutex_unlock (&ext2_journal->j_state_lock);	// Don't forget to unlock on panic path usually
	  ext2_panic
	    ("[TRX] Logic Error: Running transaction is not T_RUNNING!");
	}
      txn->t_updates++;
      JRNL_LOG_DEBUG ("Just joined tx id: %u.", txn->t_tid);
    }
  else
    {
      txn = calloc (1, sizeof (journal_transaction_t));
      if (!txn)
	{
	  pthread_mutex_unlock (&ext2_journal->j_state_lock);
	  return ENOMEM;
	}

      hurd_ihash_init (&txn->t_buffer_map, HURD_IHASH_NO_LOCP);
      txn->t_tid = ext2_journal->j_transaction_sequence++;
      txn->t_state = T_RUNNING;
      txn->t_updates = 1;

      ext2_journal->j_running_transaction = txn;
      JRNL_LOG_DEBUG ("[TRX] Created NEW TID %u", txn->t_tid);
    }

  pthread_mutex_unlock (&ext2_journal->j_state_lock);
  *out_txn = txn;
  return 0;
}

static void
journal_stop_transaction_locked (journal_t *journal,
				 journal_transaction_t *txn)
{
  if (txn->t_updates == 0)
    {
      /* This implies a double-stop or corruption */
      ext2_panic ("[TRX] Logic Error: Transaction stopped too many times!");
    }
  txn->t_updates--;
  if (txn->t_updates == 0)
    {
      /* If anyone is sleeping in the commit loop waiting for this, wake them */
      pthread_cond_broadcast (&journal->j_commit_wait);
    }
}

void
journal_stop_transaction (journal_transaction_t *txn)
{
  if (!ext2_journal || !txn)
    return;

  pthread_mutex_lock (&ext2_journal->j_state_lock);
  journal_stop_transaction_locked (ext2_journal, txn);
  pthread_mutex_unlock (&ext2_journal->j_state_lock);
}

/**
 * Adds a modified filesystem block to the SPECIFIC transaction handle.
 * Performs a "Shadow Copy" of the data immediately.
 */
error_t
journal_dirty_block (journal_transaction_t *txn, block_t fs_blocknr,
		     const void *data)
{
  journal_buffer_t *jb;
  journal_buffer_t *new_jb;
  error_t err;

  if (!ext2_journal || !txn || !data)
    return EINVAL;

  if (fs_blocknr == 0)
    {
      JRNL_LOG_DEBUG ("[CRITICAL] Attempting to log FS Block 0! Txn ID: %u",
		      txn->t_tid);
      pthread_mutex_unlock (&ext2_journal->j_state_lock);
      /* Return error to force the caller to fail/crash so we see who it is */
      return EINVAL;
    }
  pthread_mutex_lock (&ext2_journal->j_state_lock);

  if (txn->t_state != T_RUNNING)
    {
      /* The transaction was stolen underneath us! */
      JRNL_LOG_DEBUG
	("[ERROR] journal_dirty_block: Txn %u is %d (Not RUNNING)!",
	 txn->t_tid, txn->t_state);
      pthread_mutex_unlock (&ext2_journal->j_state_lock);
      return EROFS;		/* or EBUSY, or restart logic needed */
    }

  /* FAST PATH using Hurd's libihash */
  jb = (journal_buffer_t *) hurd_ihash_find (&txn->t_buffer_map,
					     (hurd_ihash_key_t) fs_blocknr);
  if (jb)
    {
      memcpy (jb->jb_shadow_data, data, block_size);
      pthread_mutex_unlock (&ext2_journal->j_state_lock);
      return 0;
    }

  new_jb = malloc (sizeof (journal_buffer_t));
  if (!new_jb)
    {
      pthread_mutex_unlock (&ext2_journal->j_state_lock);
      return ENOMEM;
    }

  new_jb->jb_shadow_data = malloc (block_size);
  if (!new_jb->jb_shadow_data)
    {
      free (new_jb);
      pthread_mutex_unlock (&ext2_journal->j_state_lock);
      return ENOMEM;
    }

  new_jb->jb_blocknr = fs_blocknr;
  memcpy (new_jb->jb_shadow_data, data, block_size);

  err = hurd_ihash_add (&txn->t_buffer_map, (hurd_ihash_key_t) fs_blocknr,
			(hurd_ihash_value_t) new_jb);
  if (err)
    {
      free (new_jb->jb_shadow_data);
      free (new_jb);
      pthread_mutex_unlock (&ext2_journal->j_state_lock);
      return err;
    }

  new_jb->jb_next = txn->t_buffers;
  txn->t_buffers = new_jb;

  txn->t_buffer_count++;
  txn->t_nr_blocks++;

  pthread_mutex_unlock (&ext2_journal->j_state_lock);
  return 0;
}

/**
 * Check if a specific filesystem block is currently part of the Running
 * Transaction.
 * Returns: 1 if the block is "pinned" (must not be written to disk yet),
 * 0 if it is safe to write.
 */
int
journal_block_is_active (block_t blocknr)
{
  journal_transaction_t *txn;
  int is_active = 0;

  if (!ext2_journal)
    return 0;

  pthread_mutex_lock (&ext2_journal->j_state_lock);
  txn = ext2_journal->j_running_transaction;

  if (txn && txn->t_state == T_RUNNING)
    {
      if (hurd_ihash_find (&txn->t_buffer_map, (hurd_ihash_key_t) blocknr))
	{
	  is_active = 1;
	}
    }

  pthread_mutex_unlock (&ext2_journal->j_state_lock);
  return is_active;
}

/* Helper: Returns 1 if t1 > t2 (handling wrapping), 0 otherwise */
static inline int
tid_gt (uint32_t t1, uint32_t t2)
{
  return (int32_t) (t1 - t2) > 0;
}

struct diskfs_transaction *
diskfs_journal_start_transaction (void)
{
  if (ext2_journal)
    {
      journal_transaction_t *real_txn;
      error_t err = journal_start_transaction (&real_txn);
      if (err)
	return NULL;
      return (struct diskfs_transaction *) real_txn;
    }
  return NULL;
}

void
diskfs_journal_stop_transaction (struct diskfs_transaction *txn)
{
  if (ext2_journal)
    {
      journal_transaction_t *real_txn = (journal_transaction_t *) txn;
      journal_stop_transaction (real_txn);
    }
}

static void
journal_wait_on_tid_locked (journal_t *journal, uint32_t target_tid)
{
  /* WAIT LOOP
     We sleep as long as the target TID is "greater than" the 
     last committed TID. */
  while (tid_gt (target_tid, journal->j_last_committed_tid))
    {
      /* Sleep until a commit finishes */
      pthread_cond_wait (&journal->j_commit_done, &journal->j_state_lock);
    }
}

/**
 * Consumes the transaction handle (caller must not call stop after this).
 * Ensures the transaction is on disk before returning.
 */
void
diskfs_journal_commit_transaction (struct diskfs_transaction *opaque_txn)
{
  if (!ext2_journal || !opaque_txn)
    return;

  journal_transaction_t *txn = (journal_transaction_t *) opaque_txn;
  uint32_t tid = txn->t_tid;

  JRNL_LOG_DEBUG ("Commiting tx id: %u.", txn->t_tid);

  pthread_mutex_lock (&ext2_journal->j_state_lock);

  /* Decrement the refcount while holding the lock. */
  journal_stop_transaction_locked (ext2_journal, txn);

  /* Check if the transaction is currently RUNNING. 
     If it is, WE steal it and become the committer. */
  if (ext2_journal->j_running_transaction == txn)
    {
      /* Steal it! */
      ext2_journal->j_running_transaction = NULL;
      txn->t_state = T_LOCKED;
      /* Pass ownership to the locked helper. */
      journal_commit_transaction_locked (ext2_journal, txn);
      return;
    }
  /* We missed it. Someone else (kjournald) stole it. 
     We will wait for them to finish here. */
  journal_wait_on_tid_locked (ext2_journal, tid);
  pthread_mutex_unlock (&ext2_journal->j_state_lock);
}

int
diskfs_journal_is_running (void)
{
  return ext2_journal != NULL;
}
