/* journal_shadow_fs.h - Journal utility for better path names

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
#ifndef LIBDISKFS_JOURNAL_SHADOW_FS_H
#define LIBDISKFS_JOURNAL_SHADOW_FS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>

#include <libdiskfs/journal_arena.h>	// your arena (journal_arena_{create,alloc,destroy})
#include <libdiskfs/journal.h>

#ifndef SHADOWFS_BUCKETS
#define SHADOWFS_BUCKETS      4096	/* power of two */
#endif
#ifndef SHADOWFS_BUCKET_LOCKS
#define SHADOWFS_BUCKET_LOCKS 256	/* power of two, <= BUCKETS */
#endif
#ifndef SHADOWFS_ROOT_INO
#define SHADOWFS_ROOT_INO     2	/* ext2 root */
#endif
#ifndef SHADOWFS_NAME_MAX
#define SHADOWFS_NAME_MAX     256	/* tune to taste */
#endif

/* --------------------------------------------------------------------- */
/* Capture-facing struct: provide minimal facts straight from capture site
 * (node + journal_info), BEFORE policy filtering and BEFORE payload_bin
 * assembly. All pointers (name fields) are expected to be stable for the
 * duration of the call; they are copied internally.                     */
typedef struct shadowfs_capture
{
  journal_action_t action;	/* op kind */
  uint64_t tx_id;		/* monotonic capture id */

  /* primary inode affected by the op */
  ino_t ino;

  /* create/mkdir/mkfile/symlink/link */
  ino_t parent_ino;		/* directory in which NAME lives */
  const char *name;		/* final path component */

  /* rename */
  ino_t dst_parent_ino;		/* destination directory */
  const char *new_name;		/* new leaf name */
  ino_t src_parent_ino;	/* (optional) source dir */
  const char *old_name;		/* (optional) old leaf */

  /* optional: if you capture clobber target for rename */
  ino_t victim_ino;		/* 0 if none/unknown */
} shadowfs_capture_t;

/* Lifetime */
void journal_sfs_init (struct journal_arena *arena);
void journal_sfs_shutdown (void);	/* does NOT destroy arena */

/* Hot-path update: call BEFORE policy filtering and writer I/O. */
void journal_sfs_capture (const shadowfs_capture_t * c);

/* Resolve a best-effort full path for INO into OUT[OUT_SZ].
 * Returns 0 on success, ENOENT/ENAMETOOLONG otherwise.       */
error_t journal_sfs_resolve_path (ino_t ino, char *out, size_t out_sz);

/* Stats / debug */
typedef struct shadowfs_stats
{
  uint64_t entries;		/* current tracked nodes (created in table) */
  uint64_t arena_used;		/* number of nodes carved from arena */
  uint64_t updates;		/* create/link/etc path updates */
  uint64_t deletes;		/* unlink/rmdir/tombstone */
  uint64_t renames;		/* rename source updates */
  uint64_t victims;		/* rename clobber victims tombstoned */
  uint64_t resolve_ok;		/* successful resolves */
  uint64_t resolve_fail;	/* failed resolves */
  uint64_t degraded;		/* arena exhaustion observed */
} shadowfs_stats_t;

void journal_sfs_get_stats (shadowfs_stats_t * out);
void journal_sfs_dump (FILE * fp);	/* one-line per entry */

#endif /*   LIBDISKFS_JOURNAL_SHADOW_FS_H */
