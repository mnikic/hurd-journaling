/* journal_shadow_fs.c - Journal utility for better path names

   Copyright (C) 2025 Free Software Foundation, Inc.

   Written by Milos Nikic.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   The GNU Hurd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with the GNU Hurd; if not, see <https://www.gnu.org/licenses/>.  */
#include <libdiskfs/journal_shadow_fs.h>
#include <libdiskfs/journal_util.h>

#include <stdbool.h>
#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

/* --- Internal types ---------------------------------------------------- */

typedef struct shadow_inode
{
  ino_t ino;
  ino_t parent;
  char name[SHADOWFS_NAME_MAX];
  uint64_t last_tx_id;
  bool is_deleted;
  struct shadow_inode *next;	/* bucket chain */
} shadow_inode_t;

typedef struct bucket
{
  shadow_inode_t *head;
} bucket_t;

/* --- State ------------------------------------------------------------- */

static bucket_t g_buckets[SHADOWFS_BUCKETS];
static pthread_mutex_t g_bucket_locks[SHADOWFS_BUCKET_LOCKS];
static struct journal_arena *g_arena = NULL;	/* owned by caller */
static bool g_degraded = false;
static shadowfs_stats_t g_stats;

/* --- Hash / locking ---------------------------------------------------- */

static inline uint32_t
hash_ino (uint64_t ino)
{
  uint64_t x = ino;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return (uint32_t) x & (SHADOWFS_BUCKETS - 1);
}

static inline uint32_t
lock_index_for_bucket (uint32_t bidx)
{
  return bidx & (SHADOWFS_BUCKET_LOCKS - 1);
}

static inline void
bucket_lock (uint32_t bidx)
{
  pthread_mutex_lock (&g_bucket_locks[lock_index_for_bucket (bidx)]);
}

static inline void
bucket_unlock (uint32_t bidx)
{
  pthread_mutex_unlock (&g_bucket_locks[lock_index_for_bucket (bidx)]);
}

/* --- Arena allocation -------------------------------------------------- */

static shadow_inode_t *
arena_alloc (void)
{
  if (!g_arena)
    {
      g_degraded = true;
      g_stats.degraded = 1;
      JOURNAL_LOG_ERROR ("Arena is NULL. No bueno! Continuing degraded");
      return NULL;
    }
  void *p = journal_arena_alloc (g_arena, sizeof (shadow_inode_t));
  if (!p)
    {
      JOURNAL_LOG_ERROR ("Shadow fs OOM, continuing degraded");
      g_degraded = true;
      g_stats.degraded = 1;
      return NULL;
    }
  memset (p, 0, sizeof (shadow_inode_t));
  g_stats.arena_used++;
  return (shadow_inode_t *) p;
}

/* --- Map ops ----------------------------------------------------------- */

static inline void
copy_name (char dst[SHADOWFS_NAME_MAX], const char *src)
{
  if (!src)
    {
      dst[0] = '\0';
      return;
    }
  strncpy (dst, src, SHADOWFS_NAME_MAX - 1);
  dst[SHADOWFS_NAME_MAX - 1] = '\0';
}

static shadow_inode_t *
bucket_find (uint32_t bidx, uint64_t ino)
{
  for (shadow_inode_t * p = g_buckets[bidx].head; p; p = p->next)
    if (p->ino == ino)
      return p;
  return NULL;
}

/* Returns node with its bucket locked; caller must unlock via map_release(). */
static shadow_inode_t *
map_get_or_create (uint64_t ino, uint32_t *out_bidx)
{
  uint32_t b = hash_ino (ino);
  if (out_bidx)
    *out_bidx = b;
  bucket_lock (b);
  shadow_inode_t *n = bucket_find (b, ino);
  if (n)
    return n;

  n = arena_alloc ();
  if (!n)
    {
      bucket_unlock (b);
      return NULL;
    }
  n->ino = ino;
  n->next = g_buckets[b].head;
  g_buckets[b].head = n;
  g_stats.entries++;
  return n;
}

static inline void
map_release (uint32_t bidx)
{
  bucket_unlock (bidx);
}

/* --- Update helpers (bucket locked) ----------------------------------- */

static inline void
record_path_locked (shadow_inode_t *n, uint64_t parent, const char *name,
		    uint64_t tx)
{
  if (tx >= n->last_tx_id)
    {
      n->parent = parent;
      copy_name (n->name, name);
      n->last_tx_id = tx;
      n->is_deleted = false;
    }
}

static inline void
mark_deleted_locked (shadow_inode_t *n, uint64_t tx)
{
  if (tx >= n->last_tx_id)
    {
      n->is_deleted = true;
      n->last_tx_id = tx;
    }
}

/* --- Public API -------------------------------------------------------- */


void
journal_sfs_init (struct journal_arena *arena)
{
  g_arena = arena;
  memset (g_buckets, 0, sizeof (g_buckets));
  for (size_t i = 0; i < SHADOWFS_BUCKET_LOCKS; ++i)
    pthread_mutex_init (&g_bucket_locks[i], NULL);
  g_degraded = false;
  memset (&g_stats, 0, sizeof (g_stats));
}

void
journal_sfs_shutdown (void)
{
  for (size_t i = 0; i < SHADOWFS_BUCKET_LOCKS; ++i)
    pthread_mutex_destroy (&g_bucket_locks[i]);
  /* arena lifetime is owned by caller; do not destroy here */
}

void
journal_sfs_capture (const shadowfs_capture_t *c)
{
  if (!c)
    return;

  switch (c->action)
    {
      /* create-ish */
    case JOURNAL_ACTION_CREATE:
    case JOURNAL_ACTION_MKDIR:
    case JOURNAL_ACTION_MKFILE:
    case JOURNAL_ACTION_SYMLINK:
    case JOURNAL_ACTION_LINK:
      {
	uint32_t bidx = 0;
	shadow_inode_t *n = map_get_or_create (c->ino, &bidx);
	if (n)
	  {
	    const char *nm = (c->name && c->name[0]) ? c->name
	      : (c->new_name && c->new_name[0]) ? c->new_name
	      : (c->old_name && c->old_name[0]) ? c->old_name : "";
	    uint64_t parent =
	      c->parent_ino ? c->parent_ino : c->src_parent_ino;
	    record_path_locked (n, parent, nm, c->tx_id);
	    g_stats.updates++;
	    map_release (bidx);
	  }
	break;
      }

      /* rename */
    case JOURNAL_ACTION_RENAME:
      {
	uint32_t b1 = 0;
	shadow_inode_t *src = map_get_or_create (c->ino, &b1);
	if (src)
	  {
	    const char *nm = (c->new_name && c->new_name[0]) ? c->new_name
	      : (c->name && c->name[0]) ? c->name
	      : (c->old_name && c->old_name[0]) ? c->old_name : "";
	    uint64_t parent = c->dst_parent_ino ? c->dst_parent_ino
	      : (c->parent_ino ? c->parent_ino : c->src_parent_ino);
	    record_path_locked (src, parent, nm, c->tx_id);
	    g_stats.renames++;
	    map_release (b1);
	  }

	/* Optional: clobber victim if you ever capture it */
	if (c->victim_ino)
	  {
	    uint32_t b2 = 0;
	    shadow_inode_t *vic = map_get_or_create (c->victim_ino, &b2);
	    if (vic)
	      {
		mark_deleted_locked (vic, c->tx_id);
		g_stats.victims++;
		map_release (b2);
	      }
	  }
	break;
      }

      /* delete-ish */
    case JOURNAL_ACTION_UNLINK:
    case JOURNAL_ACTION_RMDIR:
    case JOURNAL_ACTION_TOMBSTONE:
      {
	uint32_t bidx = 0;
	shadow_inode_t *n = map_get_or_create (c->ino, &bidx);
	if (n)
	  {
	    mark_deleted_locked (n, c->tx_id);
	    g_stats.deletes++;
	    map_release (bidx);
	  }
	break;
      }

      /* metadata-only: ignored for path shadowing */
    case JOURNAL_ACTION_CHMOD:
    case JOURNAL_ACTION_CHOWN:
    case JOURNAL_ACTION_UTIME:
    case JOURNAL_ACTION_ATIME:
    case JOURNAL_ACTION_TRUNCATE:
    case JOURNAL_ACTION_GROW:
    case JOURNAL_ACTION_CHAUTHOR:
    case JOURNAL_ACTION_CHFLAGS:
    case JOURNAL_ACTION_WRITE:
    case JOURNAL_ACTION_UNKNOWN:
    default:
      break;
    }
}

error_t
journal_sfs_resolve_path (ino_t leaf_ino, char *out, size_t out_sz)
{
  if (!out || !out_sz)
    return EINVAL;
  out[0] = '\0';

  const int MAX_DEPTH = 1024;
  const char *segs[MAX_DEPTH];
  int seg_lens[MAX_DEPTH];
  int depth = 0;

  uint64_t cur = leaf_ino;

  while (cur && depth < MAX_DEPTH)
    {
      if (cur == SHADOWFS_ROOT_INO)
	break;
      uint32_t b = hash_ino (cur);
      bucket_lock (b);
      shadow_inode_t *n = bucket_find (b, cur);
      if (!n || n->is_deleted || n->name[0] == '\0')
	{
	  JOURNAL_LOG_DEBUG ("Nothing found");
	  bucket_unlock (b);
	  g_stats.resolve_fail++;
	  return ENOENT;
	}
      segs[depth] = n->name;
      seg_lens[depth] = (int) strnlen (n->name, SHADOWFS_NAME_MAX);
      uint64_t parent = n->parent;
      bucket_unlock (b);

      depth++;
      if (cur == SHADOWFS_ROOT_INO)
	break;
      if (parent == 0 || parent == cur)
	break;
      cur = parent;
    }

  if (depth == 0)
    {
      g_stats.resolve_fail++;
      return ENOENT;
    }

  size_t pos = 0;
  if (pos < out_sz)
    out[pos++] = '/';
  for (int i = depth - 1; i >= 0; --i)
    {
      int len = seg_lens[i];
      if (len == 0)
	continue;
      if ((pos + (size_t) len + 1) >= out_sz)
	{
	  g_stats.resolve_fail++;
	  return ENAMETOOLONG;
	}
      memcpy (out + pos, segs[i], (size_t) len);
      pos += (size_t) len;
      if (i != 0)
	out[pos++] = '/';
    }
  if (pos >= out_sz)
    {
      g_stats.resolve_fail++;
      return ENAMETOOLONG;
    }
  out[pos] = '\0';
  g_stats.resolve_ok++;
  return 0;
}

void
journal_sfs_get_stats (shadowfs_stats_t *out)
{
  if (out)
    *out = g_stats;
}

void
journal_sfs_dump (FILE *fp)
{
  if (!fp)
    fp = stderr;
  for (uint32_t b = 0; b < SHADOWFS_BUCKETS; ++b)
    {
      bucket_lock (b);
      for (shadow_inode_t * n = g_buckets[b].head; n; n = n->next)
	{
	  fprintf (fp,
		   "ino=%llu parent=%llu deleted=%d tx=%llu name=\"%s\"\n",
		   (unsigned long long) n->ino,
		   (unsigned long long) n->parent, n->is_deleted ? 1 : 0,
		   (unsigned long long) n->last_tx_id, n->name);
	}
      bucket_unlock (b);
    }
}
