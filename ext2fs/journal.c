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
#include <time.h>

#include <libdiskfs/diskfs.h>
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

/**
 * Slab Allocator Pool Size.
 * Pre-allocates a contiguous chunk of memory for journal buffers
 * (512 * 4KB = 2MB).
 * This allows journal_dirty_block to be a zero-allocation operation for the
 * vast majority of workloads, falling back to dynamic allocation only under
 * extreme metadata pressure.
 */
#define JRNL_MAX_FREE_BUFFERS 512

/**
 * Initial capacity for the per-transaction Robin Hood hash map.
 * Must be a power of 2. We start small (32) so that tiny metadata updates
 * don't waste memory, but the map will dynamically double in size
 * as the transaction grows.
 */
#define JRNL_MAP_INIT_CAPACITY 32

#define JOURNAL_LOCK(j)  \
  do { \
    assert_backtrace ((j) != NULL); \
    pthread_mutex_lock(&(j)->j_state_lock); \
  } while (0)

#define JOURNAL_UNLOCK(j) \
  do { \
    assert_backtrace ((j) != NULL); \
    pthread_mutex_unlock(&(j)->j_state_lock); \
  } while (0)

#define JOURNAL_WAIT(cond, j) \
    pthread_cond_wait((cond), &(j)->j_state_lock)

static pthread_t kjournald_tid;

/**
 * Represents one modified block (4KB) that needs to be written to the journal.
 */
typedef struct journal_buffer
{
  block_t jb_blocknr;		/* The physical block number on the filesystem */
  char jb_shadow_data[4096];	/* 4KB Copy of the data to be logged */
  struct journal_buffer *jb_next;	/* Linked list next pointer */
  uint8_t jb_is_written;
} journal_buffer_t;

/**
 * A single bucket for custom hash table.
 */
typedef struct
{
  block_t blocknr;		/* The key */
  uint32_t probe_dist;		/* Robin Hood distance from ideal slot */
  journal_buffer_t *jb;		/* The value. NULL means this bucket is empty. */
} journal_bucket_t;

typedef struct
{
  journal_bucket_t *buckets;
  size_t capacity;		/* Must be a power of 2 */
  size_t mask;			/* capacity - 1 */
  size_t size;			/* Number of active items */
} journal_block_map_t;

/* The state of a transaction in memory */
typedef enum
{
  T_RUNNING,			/* Accepting new handles/buffers */
  T_LOCKED,			/* Locked, no new handles,waiting for updates to finish */
  T_FLUSHING,			/* Writing to the journal ring buffer */
  T_FINISHED			/* Done, waiting to be checkpointed */
} transaction_state_t;

typedef struct journal_freed_extent
{
  block_t fe_start;
  unsigned long fe_count;
  struct journal_freed_extent *fe_next;
} journal_freed_extent_t;

/* The Transaction Object */
struct diskfs_transaction
{
  uint32_t t_tid;		/* Transaction ID (Sequence Number) */
  transaction_state_t t_state;

  /* The Log Position */
  uint32_t t_log_start;		/* Where this transaction starts in the ring */

  uint32_t t_updates;		/* Refcount: How many threads are in this transaction? */

  /* The Map: block_t -> journal_buffer_t (for O(1) lookups).
   * We use a per-transaction map rather than a global, journal-wide map.
   * While a global map would make asynchronous block notifications slightly
   * faster, it makes transaction cleanup significantly slower and more fragile.
   * Because the strict capacity geometry of our ring buffer and proactive
   * checkpointing strictly limit the number of concurrent transactions,
   * the O(N) iteration over active transactions during a block notification
   * is strictly bounded to a tiny number. Therefore, per-transaction maps
   * provide the best balance of fast lookups and instantaneous,
   * memory-safe cleanup. */
  journal_block_map_t t_buffer_map;

  struct diskfs_transaction *t_checkpoint_next;	/* Next in global checkpoint list */
  int t_outstanding_io;

  journal_freed_extent_t *t_freed_blocks;	/* Blocks deleted in this txn */
  uint8_t sync_needed;		/* IOU flag for synchronous commits */
};

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
  diskfs_transaction_t *j_running_transaction;	/* Currently filling */
  diskfs_transaction_t *j_committing_transaction;	/* Transaction that is beeing committed. */
  diskfs_transaction_t *j_checkpoint_list;	/* Head (Oldest, defines j_tail) */
  diskfs_transaction_t *j_checkpoint_last;

  uint32_t j_max_transaction_buffers;	/* Max size of a single transaction */
  uint32_t j_min_free;

  uint32_t j_last_committed_tid;	/* Transaction ID of the last committed txn. */
  pthread_cond_t j_commit_done;	/* Cond. var. while waiting for the tx to be committed. */

  int j_must_exit;		/* variable that tells journal thread when to stop. */
  pthread_cond_t j_flusher_wakeup;	/* Cond. var for the kjournald sleep cycle */

  /* Pre-allocated buffers for zero-allocation commits */
  void *j_descriptor_buf;
  void *j_commit_buf;
  /* Pre-allocated buffers for (near) zero-allocation journal_dirty_block */
  journal_buffer_t *j_pool_memory;	/* The raw contiguous block */
  journal_buffer_t *j_free_buffers;	/* The linked list head */
} journal_t;

/* Specialized integer hash */
static inline uint32_t
journal_hash_block (block_t blocknr)
{
  uint64_t h = (uint64_t) blocknr;
  h ^= h >> 23;
  h *= 0x2127599bf4325c37ULL;
  h ^= h >> 47;
  return (uint32_t) h;
}

static int
journal_map_init (journal_block_map_t *map, size_t initial_cap)
{
  size_t cap =
    initial_cap <
    JRNL_MAP_INIT_CAPACITY ? JRNL_MAP_INIT_CAPACITY : initial_cap;

  size_t pow2 = 1;
  while (pow2 < cap)
    pow2 *= 2;

  map->buckets = calloc (pow2, sizeof (journal_bucket_t));
  if (!map->buckets)
    return ENOMEM;

  map->capacity = pow2;
  map->mask = pow2 - 1;
  map->size = 0;
  return 0;
}

static void
journal_map_destroy (journal_block_map_t *map)
{
  free (map->buckets);
  map->buckets = NULL;
  map->capacity = 0;
  map->size = 0;
}

/**
 * Doubles the capacity of the block map and rehashes all existing elements.
 * Returns 0 on success, or ENOMEM if allocation fails.
 */
static int
journal_map_resize (journal_block_map_t *map)
{
  size_t old_cap = map->capacity;
  journal_bucket_t *old_buckets = map->buckets;
  size_t new_cap = old_cap * 2;
  journal_bucket_t *new_buckets = calloc (new_cap, sizeof (journal_bucket_t));
  if (!new_buckets)
    return ENOMEM;

  size_t new_mask = new_cap - 1;

  for (size_t i = 0; i < old_cap; i++)
    {
      journal_bucket_t *old_b = &old_buckets[i];
      if (old_b->jb != NULL)
	{
	  block_t blocknr = old_b->blocknr;
	  journal_buffer_t *jb = old_b->jb;
	  uint32_t pos = journal_hash_block (blocknr) & new_mask;
	  uint32_t dist = 0;
	  for (;;)
	    {
	      journal_bucket_t *new_b = &new_buckets[pos];
	      if (new_b->jb == NULL)
		{
		  new_b->blocknr = blocknr;
		  new_b->probe_dist = dist;
		  new_b->jb = jb;
		  break;
		}
	      /* Robin Hood Swap: Displace items that are closer to their
	       * ideal hash position than our current item. */
	      if (new_b->probe_dist < dist)
		{
		  block_t tmp_block = new_b->blocknr;
		  uint32_t tmp_dist = new_b->probe_dist;
		  journal_buffer_t *tmp_jb = new_b->jb;

		  new_b->blocknr = blocknr;
		  new_b->probe_dist = dist;
		  new_b->jb = jb;

		  blocknr = tmp_block;
		  dist = tmp_dist;
		  jb = tmp_jb;
		}

	      pos = (pos + 1) & new_mask;
	      dist++;
	    }
	}
    }

  free (old_buckets);
  map->buckets = new_buckets;
  map->capacity = new_cap;
  map->mask = new_mask;
  /* map->size remains exactly the same! */

  return 0;
}

static int
journal_map_insert (journal_block_map_t *map, block_t blocknr, journal_buffer_t *jb)
{
  if (map->size * 100 >= map->capacity * 85)
    {
      int err = journal_map_resize (map);
      if (err)
	return err;
    }

  uint32_t pos = journal_hash_block (blocknr) & map->mask;
  uint32_t dist = 0;

  for (;;)
    {
      journal_bucket_t *bucket = &map->buckets[pos];
      if (bucket->jb == NULL)
	{
	  bucket->blocknr = blocknr;
	  bucket->probe_dist = dist;
	  bucket->jb = jb;
	  map->size++;
	  return 0;
	}

      if (bucket->blocknr == blocknr)
	{
	  bucket->jb = jb;
	  return 0;
	}

      /* Robin Hood Swap: If the current bucket is closer to its ideal
       * position than our new item is, we steal this slot and continue
       * finding a home for the displaced item. */
      if (bucket->probe_dist < dist)
	{
	  block_t tmp_block = bucket->blocknr;
	  uint32_t tmp_dist = bucket->probe_dist;
	  journal_buffer_t *tmp_jb = bucket->jb;

	  bucket->blocknr = blocknr;
	  bucket->probe_dist = dist;
	  bucket->jb = jb;

	  blocknr = tmp_block;
	  dist = tmp_dist;
	  jb = tmp_jb;
	}

      pos = (pos + 1) & map->mask;
      dist++;
    }
}

static journal_buffer_t *
journal_map_lookup (journal_block_map_t *map, block_t blocknr)
{
  if (!map->buckets)
    return NULL;

  uint32_t pos = journal_hash_block (blocknr) & map->mask;
  uint32_t dist = 0;

  for (;;)
    {
      journal_bucket_t *bucket = &map->buckets[pos];

      if (bucket->jb == NULL)
	return NULL;
      if (bucket->blocknr == blocknr)
	return bucket->jb;

      /* If we've probed further than the item sitting here, our target
       * cannot possibly be in this table. Stop searching. */
      if (dist > bucket->probe_dist)
	return NULL;

      pos = (pos + 1) & map->mask;
      dist++;
    }
}

/**
 * Iterates over the block map.
 * Must be initialized to 0 before the first call.
 */
journal_buffer_t *
journal_map_iterate (const journal_block_map_t *map, size_t *iter)
{
  if (!map->buckets)
    return NULL;

  while (*iter < map->capacity)
    {
      journal_bucket_t *bucket = &map->buckets[*iter];
      (*iter)++;

      if (bucket->jb != NULL)
	return bucket->jb;
    }

  return NULL;
}

/**
 * O(1) Slab Allocation.
 * MUST be called with JOURNAL_LOCK held.
 */
static inline journal_buffer_t *
journal_alloc_buffer (journal_t *journal)
{
  if (journal->j_free_buffers)
    {
      journal_buffer_t *jb = journal->j_free_buffers;
      journal->j_free_buffers = jb->jb_next;

      /* Make sure we don't leak stale data from a previous transaction */
      jb->jb_next = NULL;
      jb->jb_is_written = 0;
      return jb;
    }
  journal_buffer_t *jb = calloc (1, sizeof (journal_buffer_t));
  return jb;
}

/**
 * O(1) Slab Deallocation.
 * MUST be called with JOURNAL_LOCK held.
 */
static inline void
journal_free_buffer (journal_t *journal, journal_buffer_t *jb)
{
  /* Check if this pointer falls inside our contiguous pool block */
  if (jb >= journal->j_pool_memory &&
      jb < (journal->j_pool_memory + JRNL_MAX_FREE_BUFFERS))
    {
      /* It belongs to the permanent pool. Link it back up! */
      jb->jb_next = journal->j_free_buffers;
      journal->j_free_buffers = jb;
    }
  else
    {
      /* It falls outside the pool boundary. It was dynamically allocated. */
      free (jb);
    }
}

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
	ext2_panic ("[JOURNAL] Gap in journal file at logical %u!", i);

      journal->map.phys_blocks[i] = phys;
    }

  journal->map.inode = jnode;
}

#if JOURNAL_DEBUG
static void
journal_debug_dump_head (journal_t *journal)
{
  diskfs_transaction_t *txn = journal->j_checkpoint_list;
  if (!txn)
    {
      JRNL_LOG_DEBUG ("=== CHECKPOINT LIST EMPTY ===");
      return;
    }

  JRNL_LOG_DEBUG ("=== DEBUG DUMP HEAD TID %u ===", txn->t_tid);
  JRNL_LOG_DEBUG (" State: %d", txn->t_state);
  JRNL_LOG_DEBUG (" Outstanding IO Counter: %d", txn->t_outstanding_io);
  JRNL_LOG_DEBUG (" Map Entries (Iterating...):");

  /* Iterate the custom hash map to see what is ACTUALLY inside */
  size_t iter = 0;
  journal_buffer_t *jb;
  while ((jb = journal_map_iterate (&txn->t_buffer_map, &iter)) != NULL)
    {
      JRNL_LOG_DEBUG ("   [STUCK] Waiting for Block %u", jb->jb_blocknr);
    }

  JRNL_LOG_DEBUG ("===============================");
}
#else
static void
journal_debug_dump_head (journal_t *journal)
{
}
#endif

/**
 * The background journal thread (kjournald).
 * * Wakes up every 5 seconds to commit the currently running transaction
 * to the journal ring buffer. Crucially, this ONLY writes to the log,
 * not the main filesystem.
 *
 * Purpose:
 * - Data Loss Bound: Caps the maximum window of lost work to 5 seconds
 * (matching standard Linux ext3/ext4 behavior), rather than relying on
 * the 30-second diskfs_sync_everything() interval.
 * - Transaction Sizing: Prevents the in-memory shadow buffer map from
 * growing infinitely large during heavy metadata storms (e.g., compiling).
 * - Pager Optimization: By eagerly committing transactions in the background,
 * we proactively satisfy the Write-Ahead Log (WAL) barrier. This ensures
 * that when the Mach VM Pager eventually needs to flush dirty pages to the
 * main disk, it doesn't stall the system waiting for synchronous journal I/O.
 *
 * Future work: The interval could be made dynamic based on VFS load, but
 * a static 5-second interval provides a solid baseline.
 */
static void *
kjournald_thread (void *arg)
{
  journal_t *journal = (journal_t *) arg;
  struct timespec ts;

  JOURNAL_LOCK (journal);
  while (!journal->j_must_exit)
    {
      clock_gettime (CLOCK_REALTIME, &ts);
      ts.tv_sec += 5;

      pthread_cond_timedwait (&journal->j_flusher_wakeup,
			      &journal->j_state_lock, &ts);

      if (journal->j_must_exit)
	break;
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

	  JOURNAL_UNLOCK (journal);
	  journal_commit_running_transaction ();
	  JOURNAL_LOCK (journal);
	}
    }
  JOURNAL_UNLOCK (journal);
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
      ext2_warning
	("[JOURNAL] Write failed at logical %u. Err: %s",
	 logical_idx, strerror (err));
      return err;
    }

  if (written_amount != block_size)
    {
      ext2_warning ("[JOURNAL] Short write! Wanted %u, wrote %lu",
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
    return err;

  if (read_amount != block_size)
    {
      ext2_warning ("[JOURNAL] Short read! Wanted %u, got %lu", block_size,
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
      ext2_warning ("[JOURNAL] Failed to read SB. Err: %s", strerror (err));
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

  jsb->s_sequence = htobe32 (sequence);
  jsb->s_start = htobe32 (start);

  JRNL_LOG_DEBUG ("[SB] Updating: Seq %u, Head %u", sequence, start);
  err = journal_write_block (journal, 0, jsb);
  if (err)
    return err;
  return 0;
}

static diskfs_transaction_t *
journal_get_oldest_transaction_locked (journal_t *journal)
{
  if (journal->j_checkpoint_list)
    return journal->j_checkpoint_list;

  if (journal->j_committing_transaction)
    return journal->j_committing_transaction;

  if (journal->j_running_transaction)
    return journal->j_running_transaction;

  return NULL;
}

/**
 * Records a range of deleted blocks so they can be unpinned from older
 * checkpoint lists AFTER this transaction safely commits.
 */
void
journal_record_freed_blocks (block_t start, unsigned long count)
{
  if (!ext2_journal)
    return;

  journal_freed_extent_t *ext = malloc (sizeof (journal_freed_extent_t));
  if (!ext)
    {
      ext2_warning
	("[JOURNAL] ENOMEM tracking freed blocks. Harmless I/O overhead may occur.");
      return;
    }

  ext->fe_start = start;
  ext->fe_count = count;

  JOURNAL_LOCK (ext2_journal);
  diskfs_transaction_t *txn = ext2_journal->j_running_transaction;
  if (!txn || txn->t_state != T_RUNNING)
    {
      /* The transaction was committed by another thread before we locked!
         We just drop the recording, since the block is already forgotten. */
      JOURNAL_UNLOCK (ext2_journal);
      free (ext);
      return;
    }
  ext->fe_next = txn->t_freed_blocks;
  txn->t_freed_blocks = ext;
  JOURNAL_UNLOCK (ext2_journal);
}

/**
 * Consumes the freed blocks list and deallocates them.
 */
static void
journal_forget_freed_blocks (journal_freed_extent_t *ext)
{
  while (ext)
    {
      journal_freed_extent_t *next = ext->fe_next;
      journal_notify_blocks_written (ext->fe_start, ext->fe_count);
      free (ext);
      ext = next;
    }
}

/**
 * Fully destroys the transaction structure.
 * Called only when checkpointing is 100% complete.
 */
static void
journal_free_transaction (diskfs_transaction_t *txn)
{
  if (!txn)
    return;
  journal_freed_extent_t *ext = txn->t_freed_blocks;
  while (ext)
    {
      journal_freed_extent_t *next = ext->fe_next;
      free (ext);
      ext = next;
    }
  txn->t_freed_blocks = NULL;

  /* Free all journal buffers using the O(N) map iterator */
  size_t iter = 0;
  journal_buffer_t *jb;
  while ((jb = journal_map_iterate (&txn->t_buffer_map, &iter)) != NULL)
    {
      /* The slab allocator handles linking it back into the free pool */
      journal_free_buffer (ext2_journal, jb);
    }

  journal_map_destroy (&txn->t_buffer_map);
  free (txn);
}

/**
 * Checks if the oldest transaction(s) are fully written.
 * If so, frees them and advances the journal tail.
 */
static int
journal_try_advance_tail_locked (journal_t *journal)
{
  int advanced = 0;
  diskfs_transaction_t *txn = journal->j_checkpoint_list;

  JRNL_LOG_DEBUG ("Attempting to advance tail...");

  /* Loop until we hit a busy transaction or empty list */
  while (txn)
    {
      if (txn->t_outstanding_io > 0)
	break;

      JRNL_LOG_DEBUG ("[CHECKPOINT] Cascade Reclaim TID %u (0 pending).",
		      txn->t_tid);

      /* Unlink from list */
      journal->j_checkpoint_list = txn->t_checkpoint_next;
      if (journal->j_checkpoint_list == NULL)
	journal->j_checkpoint_last = NULL;

      diskfs_transaction_t *oldest =
	journal_get_oldest_transaction_locked (journal);
      if (oldest)
	journal->j_tail = oldest->t_log_start;
      else
	journal->j_tail = 0;

      /* Destroy the object */
      journal_free_transaction (txn);

      advanced = 1;

      /* Relod the new head to check in the next iteration */
      txn = journal->j_checkpoint_list;
    }

  if (advanced)
    {
      uint32_t capacity = journal->j_last - journal->j_first + 1;
      uint32_t used_len = 0;
      if (journal->j_tail > 0)
	{
	  if (journal->j_head >= journal->j_tail)
	    used_len = journal->j_head - journal->j_tail;
	  else
	    used_len = (journal->j_last - journal->j_tail + 1) +
	      (journal->j_head - journal->j_first);
	}

      journal->j_free = capacity - used_len;
      JRNL_LOG_DEBUG ("[CHECKPOINT] Tail advanced. New Free: %u",
		      journal->j_free);
    }

  return advanced;
}

journal_t *
journal_create (struct node *journal_inode)
{
  journal_t *j = calloc (1, sizeof (journal_t));
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
  j->j_descriptor_buf = malloc (block_size);
  j->j_commit_buf = malloc (block_size);
  if (!j->j_descriptor_buf || !j->j_commit_buf)
    ext2_panic ("[JOURNAL] No RAM for commit buffers!");

  if (journal_load_superblock (j) != 0)
    ext2_panic ("[JOURNAL] Failed to load superblock!");
  j->j_last_committed_tid = j->j_transaction_sequence - 1;
  pthread_cond_init (&j->j_commit_done, NULL);
  pthread_mutex_init (&j->j_state_lock, NULL);
  pthread_cond_init (&j->j_commit_wait, NULL);
  pthread_cond_init (&j->j_flusher_wakeup, NULL);
  j->j_must_exit = 0;
  if (pthread_create (&kjournald_tid, NULL, kjournald_thread, j) != 0)
    ext2_warning ("Failed to create a flusher thread.");
  else
    JRNL_LOG_DEBUG ("Created flusher thread.");

  j->j_pool_memory =
    calloc (JRNL_MAX_FREE_BUFFERS, sizeof (journal_buffer_t));
  if (!j->j_pool_memory)
    ext2_panic ("[JOURNAL] No RAM for buffer pool!");

  /* Chain the contiguous blocks together into a free list */
  for (int i = 0; i < JRNL_MAX_FREE_BUFFERS - 1; i++)
    j->j_pool_memory[i].jb_next = &j->j_pool_memory[i + 1];

  /* The last block points to NULL, and the head points to block 0 */
  j->j_pool_memory[JRNL_MAX_FREE_BUFFERS - 1].jb_next = NULL;
  j->j_free_buffers = &j->j_pool_memory[0];

  return j;
}

/**
 * Forcefully clears the checkpoint list.
 * SAFE ONLY after a full filesystem sync.
 */
static void
journal_clear_checkpoint_list_locked (journal_t *journal)
{
  diskfs_transaction_t *txn = journal->j_checkpoint_list;
  /* Destroy all checkpoint transactions */
  while (txn)
    {
      diskfs_transaction_t *next = txn->t_checkpoint_next;
      journal_free_transaction (txn);
      txn = next;
    }
  journal->j_checkpoint_list = NULL;
  journal->j_checkpoint_last = NULL;

  diskfs_transaction_t *oldest =
    journal_get_oldest_transaction_locked (journal);
  if (oldest)
    journal->j_tail = oldest->t_log_start;
  else
    journal->j_tail = 0;

  uint32_t capacity = journal->j_last - journal->j_first + 1;
  uint32_t used_len = 0;

  if (journal->j_tail > 0)
    {
      if (journal->j_head >= journal->j_tail)
	used_len = journal->j_head - journal->j_tail;
      else
	used_len = (journal->j_last - journal->j_tail + 1) +
	  (journal->j_head - journal->j_first);
    }

  journal->j_free = capacity - used_len;

  journal_update_superblock (journal, journal->j_transaction_sequence,
			     journal->j_tail);
}

/**
 * Safely marks the journal as clean on disk.
 * MUST only be called after sync_global(1) ensures no pager I/O is in flight,
 * otherwise asynchronous pager notifications will cause a Use-After-Free!
 */
void
journal_quiesce_checkpoints (void)
{
  if (!ext2_journal)
    return;

  JOURNAL_LOCK (ext2_journal);
  /* Wait for any active commit to finish writing to the log */
  while (ext2_journal->j_committing_transaction != NULL)
    JOURNAL_WAIT (&ext2_journal->j_commit_done, ext2_journal);

  /* Clear the list and write s_start = 0 to the JBD2 superblock */
  journal_clear_checkpoint_list_locked (ext2_journal);
  JOURNAL_UNLOCK (ext2_journal);
}

/**
 * Called when we are running out of space.
 * Since we do a version of sync() on every commit, we can safely declare all
 * previous transactions "checkpointed" and reset the log.
 */
static void
journal_force_checkpoint_locked (journal_t *journal)
{
  JRNL_LOG_DEBUG ("[CHECKPOINT] Journal Full (Free: %u). Squeezing disk...",
		  journal->j_free);
  JOURNAL_UNLOCK (journal);
  journal_sync_everything ();
  JOURNAL_LOCK (journal);

  journal_clear_checkpoint_list_locked (journal);
  JOURNAL_UNLOCK (journal);
  flush_to_disk ();
  JOURNAL_LOCK (journal);

  JRNL_LOG_DEBUG ("[CHECKPOINT] Space reclaimed. Free: %u. Tail: %u",
		  journal->j_free, journal->j_tail);
}

static inline uint32_t
journal_next_log_block_safe (journal_t *journal)
{
  uint32_t block;
  JOURNAL_LOCK (journal);
  journal->j_head++;
  if (journal->j_head > journal->j_last)
    journal->j_head = journal->j_first;
  journal->j_free--;
  block = journal->j_head;
  JOURNAL_UNLOCK (journal);
  return block;
}

/* Helper: Returns 1 if t1 > t2 (handling wrapping), 0 otherwise */
static inline int
tid_gt (uint32_t t1, uint32_t t2)
{
  return (int32_t) (t1 - t2) > 0;
}

static void
journal_wait_on_tid_locked (journal_t *journal, uint32_t target_tid)
{
  while (tid_gt (target_tid, journal->j_last_committed_tid))
    /* Sleep until a commit finishes */
    JOURNAL_WAIT (&journal->j_commit_done, journal);
}

diskfs_transaction_t *
diskfs_journal_start_transaction (void)
{
  diskfs_transaction_t *txn;
  if (!ext2_journal)
    return NULL;

  JOURNAL_LOCK (ext2_journal);

  if (ext2_journal->j_free < ext2_journal->j_min_free)
    {
      JRNL_LOG_DEBUG
	("[TRX] Journal full (Free: %u). Forcing checkpoint.",
	 ext2_journal->j_free);

      journal_force_checkpoint_locked (ext2_journal);
    }

  txn = ext2_journal->j_running_transaction;

  if (txn)
    {
      assert_backtrace (txn->t_state == T_RUNNING);
      txn->t_updates++;
    }
  else
    {
      txn = calloc (1, sizeof (diskfs_transaction_t));
      if (!txn)
	{
	  JOURNAL_UNLOCK (ext2_journal);
	  return NULL;
	}

      journal_map_init (&txn->t_buffer_map, 0);
      txn->t_tid = ext2_journal->j_transaction_sequence++;
      txn->t_state = T_RUNNING;
      txn->t_updates = 1;

      ext2_journal->j_running_transaction = txn;
      JRNL_LOG_DEBUG ("[TRX] Created NEW TID %u", txn->t_tid);
    }

  JOURNAL_UNLOCK (ext2_journal);
  return txn;
}

/**
 * Adds a modified filesystem block to the SPECIFIC transaction handle.
 * Performs a "Shadow Copy" of the data immediately.
 */
error_t
journal_dirty_block (diskfs_transaction_t *txn, block_t fs_blocknr,
		     const void *data)
{
  journal_buffer_t *jb;
  journal_buffer_t *new_jb;
  error_t err;

  if (!ext2_journal || !txn || !data)
    return EINVAL;

  JOURNAL_LOCK (ext2_journal);

  if (txn->t_state != T_RUNNING && txn->t_state != T_LOCKED)
    {
      /* The transaction was stolen underneath us! */
      ext2_warning
	("[ERROR] journal_dirty_block: Txn %u is %d (Not RUNNING)!",
	 txn->t_tid, txn->t_state);
      JOURNAL_UNLOCK (ext2_journal);
      return EROFS;
    }
  jb = journal_map_lookup (&txn->t_buffer_map, fs_blocknr);

  if (jb)
    {
      if (jb->jb_is_written)
	{
	  jb->jb_is_written = 0;
	  txn->t_outstanding_io++;
	}
      memcpy (jb->jb_shadow_data, data, block_size);
      JOURNAL_UNLOCK (ext2_journal);
      return 0;
    }
  new_jb = journal_alloc_buffer (ext2_journal);
  if (!new_jb)
    {
      JOURNAL_UNLOCK (ext2_journal);
      return ENOMEM;
    }

  new_jb->jb_blocknr = fs_blocknr;
  memcpy (new_jb->jb_shadow_data, data, block_size);
  err = journal_map_insert (&txn->t_buffer_map, fs_blocknr, new_jb);

  if (err)
    {
      journal_free_buffer (ext2_journal, new_jb);
      JOURNAL_UNLOCK (ext2_journal);
      return err;
    }

  txn->t_outstanding_io++;
  JOURNAL_UNLOCK (ext2_journal);
  return 0;
}

void
diskfs_journal_set_sync (diskfs_transaction_t *txn)
{
  if (txn)
    txn->sync_needed = 1;
}

int
diskfs_journal_needs_sync (diskfs_transaction_t *txn)
{
  return txn ? txn->sync_needed : 0;
}

static void
journal_stop_transaction_locked (journal_t *journal,
				 diskfs_transaction_t *txn)
{
  if (txn->t_updates == 0)
    {
      /* This implies a double-stop or corruption */
      ext2_warning ("[TRX] Logic Error: Transaction stopped too many times!");
      return;
    }
  txn->t_updates--;
  if (txn->t_updates == 0)
    /* If anyone is sleeping in the commit loop waiting for this, wake them */
    pthread_cond_broadcast (&journal->j_commit_wait);
}

void
diskfs_journal_stop_transaction (diskfs_transaction_t *txn)
{
  if (!ext2_journal || !txn)
    return;

  JOURNAL_LOCK (ext2_journal);
  journal_stop_transaction_locked (ext2_journal, txn);
  JOURNAL_UNLOCK (ext2_journal);
}

/* Helper to reset the header for a new block */
static void
setup_header (void *buf, const diskfs_transaction_t *txn, uint32_t block_type)
{
  journal_header_t *h = (journal_header_t *) buf;
  h->h_magic = htobe32 (JBD2_MAGIC_NUMBER);
  h->h_blocktype = htobe32 (block_type);
  h->h_sequence = htobe32 (txn->t_tid);
}

/* Writes the Descriptor Block + All Data Blocks (Escaped) */
static error_t
journal_write_payload (journal_t *journal, const diskfs_transaction_t *txn)
{
  if (txn->t_buffer_map.size == 0)
    return 0;

  void *descriptor_buf = journal->j_descriptor_buf;
  memset (descriptor_buf, 0, block_size);

  setup_header (descriptor_buf, txn, JBD2_DESCRIPTOR_BLOCK);

  uint32_t tag_offset = sizeof (journal_header_t);
  error_t err = 0;

  uint32_t descriptor_loc = journal_next_log_block_safe (journal);

  size_t iter = 0;
  size_t batch_start_iter = 0;
  uint32_t batch_count = 0;
  size_t items_written = 0;

  journal_buffer_t *jb;

  while ((jb = journal_map_iterate (&txn->t_buffer_map, &iter)) != NULL)
    {
      /* If the descriptor block is full, flush it and its data blocks first */
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
	  if ((err =
	       journal_write_block (journal, descriptor_loc, descriptor_buf)))
	    goto err_out;

	  /* Write the Data Blocks for this batch using a fresh iterator. */
	  size_t data_iter = batch_start_iter;
	  for (uint32_t i = 0; i < batch_count; i++)
	    {
	      journal_buffer_t *p =
		journal_map_iterate (&txn->t_buffer_map, &data_iter);
	      uint32_t data_loc = journal_next_log_block_safe (journal);
	      if ((err =
		   journal_write_block (journal, data_loc,
					p->jb_shadow_data)))
		goto err_out;
	    }

	  /* Prepare for the next batch */
	  descriptor_loc = journal_next_log_block_safe (journal);
	  memset (descriptor_buf, 0, block_size);
	  setup_header (descriptor_buf, txn, JBD2_DESCRIPTOR_BLOCK);
	  tag_offset = sizeof (journal_header_t);

	  /* The next batch starts at the exact index of the current 'jb'.
	     Since journal_map_iterate advanced 'iter', the current item is at iter - 1. */
	  batch_start_iter = iter - 1;
	  batch_count = 0;
	}

      /* Add the current block to the Descriptor Block */
      journal_block_tag_t *tag =
	(journal_block_tag_t *) ((char *) descriptor_buf + tag_offset);
      tag->t_blocknr = htobe32 (jb->jb_blocknr);

      items_written++;
      uint32_t flags = JBD2_FLAG_SAME_UUID;

      /* If this is the absolute last item in the transaction, set the LAST_TAG flag */
      if (items_written == txn->t_buffer_map.size)
	flags |= JBD2_FLAG_LAST_TAG;

      /* Escaping Logic: If data looks like a header, mask it. */
      uint32_t *data_head = (uint32_t *) jb->jb_shadow_data;
      if (*data_head == htobe32 (JBD2_MAGIC_NUMBER))
	{
	  flags |= JBD2_FLAG_ESCAPE;
	  *data_head = 0;
	}
      tag->t_flags = htobe32 (flags);

      tag_offset += sizeof (journal_block_tag_t);
      batch_count++;
    }

  /* Write the Final Descriptor Block */
  if (batch_count > 0)
    {
      JRNL_LOG_DEBUG ("[COMMIT] Writing final Descriptor for tx %u to %u",
		      txn->t_tid, descriptor_loc);
      if ((err =
	   journal_write_block (journal, descriptor_loc, descriptor_buf)))
	goto err_out;

      /* Write the Final Data Blocks */
      size_t data_iter = batch_start_iter;
      for (uint32_t i = 0; i < batch_count; i++)
	{
	  journal_buffer_t *p =
	    journal_map_iterate (&txn->t_buffer_map, &data_iter);
	  uint32_t data_loc = journal_next_log_block_safe (journal);
	  if ((err =
	       journal_write_block (journal, data_loc, p->jb_shadow_data)))
	    goto err_out;
	}
    }

err_out:
  return err;
}

/* Writes the Commit Block */
static error_t
journal_write_commit_record (journal_t *journal,
			     diskfs_transaction_t *txn, uint32_t commit_loc)
{
  void *commit_buf = journal->j_commit_buf;
  memset (commit_buf, 0, block_size);
  setup_header (commit_buf, txn, JBD2_COMMIT_BLOCK);
  return journal_write_block (journal, commit_loc, commit_buf);
}

/**
 * Commits the transaction. This function expects
 * journal lock to be held, and returns journal lock unlocked. */
static error_t
journal_commit_running_transaction_locked (journal_t *journal)
{
  error_t err = 0;
  uint32_t commit_loc;
  diskfs_transaction_t *txn;

  while (journal->j_committing_transaction != NULL)
    JOURNAL_WAIT (&journal->j_commit_done, journal);

  txn = journal->j_running_transaction;
  if (!txn)
    {
      /* Nothing to do, unlock and go back. We won't
       * even broadcast commit_done, because we haven't
       * done anything really. */
      JOURNAL_UNLOCK (journal);
      return 0;
    }

  journal->j_committing_transaction = txn;
  journal->j_running_transaction = NULL;
  txn->t_state = T_LOCKED;

  while (txn->t_updates > 0)
    JOURNAL_WAIT (&journal->j_commit_wait, journal);

  txn->t_state = T_FLUSHING;

  /* Since we have active checkpointing in place it should be
   * rare that this is needed. Yet it is here as an escape hatch
   * in those rare cases. */
  uint32_t needed =
    txn->t_buffer_map.size +
    (txn->t_buffer_map.size / JRNL_DESCRIPTOR_RATIO) + JRNL_COMMIT_MARGIN;
  uint32_t low_water =
    (journal->j_last - journal->j_first) / JRNL_LOW_WATER_RATIO;
  if (journal->j_free < needed || journal->j_free < low_water)
    journal_force_checkpoint_locked (journal);

  txn->t_log_start = journal->j_head + 1;
  if (txn->t_log_start > journal->j_last)
    txn->t_log_start = journal->j_first;
  /* We unlock for IO! */
  JOURNAL_UNLOCK (journal);

  /* Write Data (I/O) */
  err = journal_write_payload (journal, txn);
  if (err)
    goto abort_commit;

  /* Ensure Data is on disk */
  flush_to_disk ();

  commit_loc = journal_next_log_block_safe (journal);
  err = journal_write_commit_record (journal, txn, commit_loc);
  if (err)
    goto abort_commit;

  /* Ensure Commit is persistent */
  flush_to_disk ();

  int need_sb_flush = 0;
  /* IO done, lock again and finalize Metadata */
  JOURNAL_LOCK (journal);

  if (journal->j_tail == 0)
    {
      journal->j_tail = txn->t_log_start;
      journal_update_superblock (journal, txn->t_tid, journal->j_tail);
      need_sb_flush = 1;
    }
  journal->j_last_committed_tid = txn->t_tid;

  txn->t_state = T_FINISHED;
  txn->t_checkpoint_next = NULL;
  journal->j_committing_transaction = NULL;
  journal_freed_extent_t *freed_extents = txn->t_freed_blocks;
  txn->t_freed_blocks = NULL;

  if (journal->j_checkpoint_last)
    journal->j_checkpoint_last->t_checkpoint_next = txn;
  else
    journal->j_checkpoint_list = txn;
  journal->j_checkpoint_last = txn;

  /* Wake up everyone waiting in journal_wait_on_tid */
  pthread_cond_broadcast (&journal->j_commit_done);
  JOURNAL_UNLOCK (journal);
  if (need_sb_flush)
    flush_to_disk ();

  journal_forget_freed_blocks (freed_extents);
  return 0;
abort_commit:
  /* We hit a physical I/O error. We must clear the pipeline slot and wake
     up any sleeping threads so they don't deadlock, before we free the txn. */
  JOURNAL_LOCK (journal);
  journal->j_committing_transaction = NULL;
  pthread_cond_broadcast (&journal->j_commit_done);
  JOURNAL_UNLOCK (journal);
  journal_free_transaction (txn);
  return err;
}

error_t
journal_commit_running_transaction (void)
{
  if (!ext2_journal)
    return 0;
  diskfs_transaction_t *txn;

  JOURNAL_LOCK (ext2_journal);
  txn = ext2_journal->j_running_transaction;

  if (!txn || txn->t_state != T_RUNNING)
    {
      JOURNAL_UNLOCK (ext2_journal);
      return EINVAL;
    }
  if (txn->t_buffer_map.size == 0)
    {
      JRNL_LOG_DEBUG ("Txn %u is empty. Keeping it open.", txn->t_tid);
      JOURNAL_UNLOCK (ext2_journal);
      return 0;
    }
  journal_debug_dump_head (ext2_journal);
  return journal_commit_running_transaction_locked (ext2_journal);
}

/**
 * Consumes the transaction handle (caller must not call stop after this).
 * Ensures the transaction is on disk before returning.
 */
void
diskfs_journal_commit_transaction (diskfs_transaction_t *opaque_txn)
{
  if (!ext2_journal || !opaque_txn)
    return;

  diskfs_transaction_t *txn = (diskfs_transaction_t *) opaque_txn;
  uint32_t tid = txn->t_tid;

  JRNL_LOG_DEBUG ("Commiting tx id: %u.", txn->t_tid);

  JOURNAL_LOCK (ext2_journal);

  /* Decrement the refcount while holding the lock. */
  journal_stop_transaction_locked (ext2_journal, txn);

  if (txn->t_buffer_map.size == 0)
    JRNL_LOG_DEBUG ("In diskfs_commit, something with no buffers. txn Id: %u",
		    tid);
  /* Check if the transaction is currently RUNNING.
     If it is, We "steal" it and become the committer. */
  if (ext2_journal->j_running_transaction == txn)
    {
      journal_commit_running_transaction_locked (ext2_journal);
      return;
    }
  /* We missed it. Someone else (kjournald) stole it.
     We will wait for them to finish here. */
  journal_wait_on_tid_locked (ext2_journal, tid);
  JOURNAL_UNLOCK (ext2_journal);
}

/**
 * Called by the pager BEFORE writing blocks to their permanent home.
 * Tries its best to ensure WAL ordering for a range of blocks.
 * There are cases in which that is not feasible though! (see below)
 */
void
journal_ensure_blocks_journaled (block_t start_block, size_t n_blocks)
{
  if (!ext2_journal || n_blocks == 0)
    return;

  JOURNAL_LOCK (ext2_journal);

  diskfs_transaction_t *commit = ext2_journal->j_committing_transaction;
  diskfs_transaction_t *run = ext2_journal->j_running_transaction;
  uint32_t wait_tid = 0;
  int force_commit = 0;
  int in_committing = 0;

  for (size_t i = 0; i < n_blocks; i++)
    {
      block_t b = start_block + i;
      journal_buffer_t *jb;

      /* If ANY block is in the running transaction, we must force a commit. */
      if (run && (jb = journal_map_lookup (&run->t_buffer_map, b)))
	{
	  force_commit = 1;
	  wait_tid = run->t_tid;
	  break;
	}
      /* Keep checking in case a later block is in the COMMITTING state. */
      else if (commit && (jb = journal_map_lookup (&commit->t_buffer_map, b)))
	{
	  if (wait_tid == 0)
	    wait_tid = commit->t_tid;
	  in_committing = 1;
	}
    }

  /* The libpager MUST NOT sleep waiting for a VFS thread or a journal thread.
     If we make it sleep, we risk a system-wide deadlock.
     On the other hand, if we return EAGAIN or similar, the pager
     will just drop the request and the block writes will be lost.
     Therefore, if the journal is not instantly ready to commit, we MUST bypass
     the WAL barrier and write directly to the main disk. */
  if (force_commit)
    {
      /* If VFS is mutating the transaction, or the disk pipeline is already full,
         we cannot commit right now. Bail out and bypass! */
      if (run->t_updates > 0 || commit != NULL)
	{
	  ext2_warning
	    ("[WARN] VM Deadlock Hazard! Bypassing WAL for RUNNING TID %u.",
	     wait_tid);
	  JOURNAL_UNLOCK (ext2_journal);
	  return;
	}

      /* It is perfectly safe. The Pager will drive the commit synchronously right now. */
      JRNL_LOG_DEBUG ("Pager forcing synchronous commit for TID %u",
		      wait_tid);
      journal_commit_running_transaction_locked (ext2_journal);
      return;
    }
  else if (in_committing)
    {
      /* The block is actively being written to the log by another thread.
         We cannot wait for it. Bail out and bypass! */
      ext2_warning
	("[WARN] VM Deadlock Hazard! Bypassing WAL for COMMITTING TID %u",
	 wait_tid);
    }

  JOURNAL_UNLOCK (ext2_journal);
}

/**
 * Called by the Pager (store_write hook) after writing blocks to the main disk.
 * Bulk version to handle clustered pageouts efficiently.
 */
void
journal_notify_blocks_written (block_t start_block, size_t n_blocks)
{
  int sb_changed = 0;
  if (!ext2_journal || n_blocks == 0)
    return;

  JRNL_LOG_DEBUG ("Got notification for %zu blocks starting at %u",
		  n_blocks, start_block);
  JOURNAL_LOCK (ext2_journal);

  /* Check Running Transaction (To catch blocks dirtied during physical I/O) */
  diskfs_transaction_t *run = ext2_journal->j_running_transaction;
  if (run)
    {
      for (size_t i = 0; i < n_blocks; i++)
	{
	  block_t b = start_block + i;
	  journal_buffer_t *jb = journal_map_lookup (&run->t_buffer_map, b);
	  if (jb && !jb->jb_is_written)
	    {
	      jb->jb_is_written = 1;
	      if (run->t_outstanding_io > 0)
		run->t_outstanding_io--;
	      JRNL_LOG_DEBUG
		("[NOTIFY] Early flush caught block %u in RUNNING Txn %u!", b,
		 run->t_tid);
	    }
	}
    }

  /* Check Committing Transaction */
  diskfs_transaction_t *commit = ext2_journal->j_committing_transaction;
  if (commit)
    {
      for (size_t i = 0; i < n_blocks; i++)
	{
	  block_t b = start_block + i;
	  journal_buffer_t *jb = journal_map_lookup (&commit->t_buffer_map, b);
	  if (jb && !jb->jb_is_written)
	    {
	      jb->jb_is_written = 1;
	      if (commit->t_outstanding_io > 0)
		commit->t_outstanding_io--;
	      JRNL_LOG_DEBUG
		("[NOTIFY] Caught block %u in COMMITTING Txn %u!", b,
		 commit->t_tid);
	    }
	}
    }

  /* Iterate over checkpoint list to find who owns these blocks */
  diskfs_transaction_t *txn = ext2_journal->j_checkpoint_list;

  while (txn)
    {
      /* Fast-path cleanup for empty transactions lingering at the head */
      if (txn->t_outstanding_io == 0
	  && txn == ext2_journal->j_checkpoint_list)
	{
	  if (journal_try_advance_tail_locked (ext2_journal))
	    sb_changed = 1;
	  txn = ext2_journal->j_checkpoint_list;
	  continue;
	}

      /* Check all blocks in this notification against this transaction */
      for (size_t i = 0; i < n_blocks && txn->t_outstanding_io > 0; i++)
	{
	  block_t b = start_block + i;
	  journal_buffer_t *jb = journal_map_lookup (&txn->t_buffer_map, b);
	  if (jb && !jb->jb_is_written)
	    {
	      jb->jb_is_written = 1;
	      txn->t_outstanding_io--;
	      JRNL_LOG_DEBUG
		("For tx %u block %u the outstanding count is %u", txn->t_tid,
		 b, txn->t_outstanding_io);
	    }
	}

      /* Did this cluster of writes finish off the transaction? */
      if (txn->t_outstanding_io == 0
	  && txn == ext2_journal->j_checkpoint_list)
	{
	  if (journal_try_advance_tail_locked (ext2_journal))
	    sb_changed = 1;
	  /* The head shifted, so reload it for the next iteration */
	  txn = ext2_journal->j_checkpoint_list;
	  continue;
	}

      txn = txn->t_checkpoint_next;
    }

  if (sb_changed)
    {
      uint32_t tail_seq;
      diskfs_transaction_t *oldest =
	journal_get_oldest_transaction_locked (ext2_journal);

      if (oldest)
	tail_seq = oldest->t_tid;
      else
	tail_seq = ext2_journal->j_transaction_sequence;

      /* Update Superblock Persistently */
      journal_update_superblock (ext2_journal, tail_seq,
				 ext2_journal->j_tail);
      JOURNAL_UNLOCK (ext2_journal);
      flush_to_disk ();
      return;
    }
  JOURNAL_UNLOCK (ext2_journal);
}
