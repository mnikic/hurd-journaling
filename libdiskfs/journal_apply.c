/* journal_apply.c - Replay journaled inode metadata updates

   Copyright (C) 2025 Free Software Foundation, Inc.

   Written by Milos Nikic.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with the GNU Hurd.  If not, see <https://www.gnu.org/licenses/>.  */

#include "journal_diskfs_helper.h"
#include <libdiskfs/diskfs.h>
#include <libdiskfs/journal_apply.h>
#include <libdiskfs/journal_config.h>
#include <libdiskfs/journal_diskfs_helper.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_path_util.h>
#include <libdiskfs/journal_policy.h>
#include <libdiskfs/journal_util.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define APPEND_CHANGE(fmt, ...)                                                \
  do {                                                                         \
    int n = snprintf(change_desc + desc_len, sizeof(change_desc) - desc_len,   \
                     "%s" fmt, first ? "" : ", ", ##__VA_ARGS__);              \
    if (n > 0 && (desc_len + (size_t)n) < sizeof(change_desc))                 \
      desc_len += (size_t)n;                                                   \
    first = false;                                                             \
  } while (0)

static inline bool
node_matches_fingerprint (const struct node *np,
			  const inode_replay_state_t *state)
{
  const struct stat *st = &np->dn_stat;
  if (st->st_size != state->st_size || st->st_blocks != state->st_blocks ||
      st->st_nlink != state->st_nlink || st->st_gen != state->st_gen)
    {
      JOURNAL_LOG_DEBUG
	("Inode %u blocked: fingerprint mismatch. Actual size %" PRIu64
	 " vs %" PRIu64 ". Actual blocks %" PRIu64 " vs %" PRIu64
	 ". Actual nlink %" PRIu64 " vs %" PRIu64 ". Actual gen %u vs %u",
	 (unsigned int) state->ino, (unsigned long long) st->st_size,
	 (unsigned long long) state->st_size,
	 (unsigned long long) st->st_blocks,
	 (unsigned long long) state->st_blocks,
	 (unsigned long long) st->st_nlink,
	 (unsigned long long) state->st_nlink, st->st_gen, state->st_gen);

      return false;
    }
  return true;
}

/* Results of trying to resolve an existing node */
typedef enum
{
  RES_FOUND = 0,		/* *out set (locked) */
  RES_NOT_FOUND,		/* neither inode nor path found */
  RES_SKIP,			/* exists, but policy says skip */
  RES_PATH_INVALID,		/* didn't find it by ino and path cannot be used */
  RES_MISMATCH,			/* found, but fingerprint mismatch */
  RES_ERROR			/* hard error (errno in *perr) */
} resolve_result_t;

/* Policy gate: same checks you had inline */
static inline bool
eligible_for_replay (const struct node *np, const inode_replay_state_t *st)
{
  if (!journal_is_safe_stat (np->dn_stat.st_mode))
    return false;

  if ((int64_t) np->dn_stat.st_mtime < 0
      || (int64_t) np->dn_stat.st_ctime < 0)
    return false;

  if ((uint64_t) np->dn_stat.st_ctime >= st->last_seen)
    return false;

  return true;
}

/* Lookup by path + fingerprint */
static inline resolve_result_t
lookup_by_path_locked (const char *path, const inode_replay_state_t *st,
		       struct node *fs_root, struct protid *cred,
		       struct node **out, error_t *perr)
{
  if (!path)
    return RES_PATH_INVALID;
  *out = NULL;
  struct node *np = NULL;
  error_t err = diskfs_lookup_path (fs_root, path, cred, &np);
  if (err == ENOENT)
    return RES_NOT_FOUND;
  if (err)
    {
      *perr = err;
      return RES_ERROR;
    }
  if (!node_matches_fingerprint (np, st))
    {
      diskfs_nput (np);
      return RES_MISMATCH;
    }

  if (!eligible_for_replay (np, st))
    {
      diskfs_nput (np);
      return RES_SKIP;
    }
  JOURNAL_LOG_DEBUG ("Done with lookup");
  *out = np;
  return RES_FOUND;
}

/* First try inode cache + fingerprint; if that fails, try path + fingerprint.
   Never creates anything. Returns locked *out on RES_FOUND. */
static inline resolve_result_t
resolve_existing_node_locked (const char *opt_full_path,
			      const inode_replay_state_t *st,
			      struct node *fs_root, struct protid *cred,
			      struct node **out, error_t *perr)
{
  *out = NULL;
  if (perr)
    *perr = 0;

  struct node *np = NULL;
  error_t err = diskfs_cached_lookup ((ino_t) st->ino, &np);
  bool alive = diskfs_cached_node_alive (np);
  if (!err && alive && node_matches_fingerprint (np, st))
    {
      JOURNAL_LOG_DEBUG ("inode %" PRIu32
			 " found! mode %o links %u matches fingerprint",
			 st->ino, np->dn_stat.st_mode, np->dn_stat.st_nlink);
      if (!eligible_for_replay (np, st))
	{
	  diskfs_nput (np);
	  return RES_SKIP;
	}

      *out = np;
      return RES_FOUND;
    }
  if (np)
    {
      if (alive)
	  diskfs_nput (np);
      else
	  diskfs_cached_node_nput_dead (np);
      np = NULL;
    }

  return lookup_by_path_locked (opt_full_path, st, fs_root, cred, out, perr);
}

/* Ensure directory exists under restore_root:
 * - Calls diskfs_mkdir_p(restore_root, dir_path, cred)
 * - Re-looks up the directory and returns a LOCKED node in *out
 *
 * On success: returns 0 and *out is a locked node (caller must diskfs_nput()).
 * On failure: returns error and sets *out = NULL.
 *
 * restore_root must be locked by the caller.
 */
static inline error_t
mkdirp_lookup_dir_locked (struct node *restore_root, const char *dir_path,
			  struct protid *cred,
			  const inode_replay_state_t *state,
			  struct node **out)
{
  if (!restore_root || !dir_path || !cred || !out)
    return EINVAL;

  *out = NULL;

  error_t err = diskfs_mkdir_p (restore_root, dir_path, cred);
  if (err)
    {
      JOURNAL_LOG_ERROR ("inode %u: mkdir_p('%s') failed: %s",
			 state ? state->ino : 0, dir_path, strerror (err));
      return err;
    }

  struct node *dir = NULL;
  err = diskfs_lookup_path (restore_root, dir_path, cred, &dir);
  if (err || !dir)
    {
      JOURNAL_LOG_ERROR ("inode %u: re-lookup of dir '%s' failed: %s",
			 state ? state->ino : 0, dir_path, strerror (err));
      return err ? err : EIO;
    }

  JOURNAL_LOG_DEBUG ("inode %u: Directory ensured. Path: %s. Ino: %" PRIu64,
		     state ? state->ino : 0, dir_path, dir->dn_stat.st_ino);

  *out = dir;			/* locked */
  return 0;
}

/* Ensure directory exists under restore_root and return it locked. */
static inline error_t
create_directory_under_restore (struct node *restore_root,
				const char *dir_path,
				const inode_replay_state_t *state,
				struct protid *cred, struct node **out)
{
  if (!journal_good_dir_path (dir_path))
    {
      JOURNAL_LOG_DEBUG
	("Rejected: ino %u path='%s' is not a supported directory name.",
	 state->ino, dir_path);
      *out = NULL;
      return EINVAL;
    }

  return mkdirp_lookup_dir_locked (restore_root, dir_path, cred, state, out);
}

static inline error_t
create_file_under_restore (struct node *restore_root, const char *full_path,	/* relative */
			   const inode_replay_state_t *state,
			   struct protid *cred, struct node **out)
{
  char dir_path[JOURNAL_PATH_MAX];
  char file_name[JOURNAL_FILENAME_MAX + 1];

  *out = NULL;

  if (!journal_split_path (full_path, dir_path, sizeof (dir_path), file_name,
			   sizeof (file_name)))
    return EINVAL;

  if (!journal_good_dir_path (dir_path) || !journal_good_filename (file_name))
    return EINVAL;

  /* Ensure parent exists (returns LOCKED dir). */
  struct node *dir = NULL;
  error_t err =
    mkdirp_lookup_dir_locked (restore_root, dir_path, cred, state, &dir);
  if (err || !dir)
    return err ? err : EIO;

  /* Create file inside locked parent. make_file handles sync if needed. */
  struct node *file = NULL;
  err = diskfs_make_file (dir, file_name, cred, &file);
  diskfs_nput (dir);

  if (err)
    {
      JOURNAL_LOG_ERROR ("inode %u: make_file('%s' in '%s') failed: %s",
			 state->ino, file_name, dir_path, strerror (err));
      return err;
    }

  JOURNAL_LOG_DEBUG ("inode %u: File created. Path: %s. Ino: %" PRIu64,
		     state->ino, full_path, file->dn_stat.st_ino);
  *out = file;
  return 0;
}

static error_t
create_node (const char *opt_full_path,
	     inode_replay_state_t *state,
	     struct node *restore_root, struct protid *cred,
	     struct node **out)
{
  if (!opt_full_path)
    return EINVAL;

  JOURNAL_LOG_DEBUG
    ("It doesn't seem file ino: %u is there. Lets see if we can create it.",
     state->ino);
  uint32_t mode = state->st_mode;
  if (S_ISDIR (mode))
    return create_directory_under_restore (restore_root, opt_full_path, state,
					   cred, out);
  else
    return create_file_under_restore (restore_root, opt_full_path, state,
				      cred, out);
}

static inline int
apply_metadata_changes_locked (struct node *np,
			       const inode_replay_state_t *state)
{
  int changes = 0;
  char change_desc[128] = { 0 };
  size_t desc_len = 0;
  bool first = true;

  /* (optionally gate uid/gid/flags on has_* if you use those flags) */
  if (np->dn_stat.st_uid != state->uid)
    {
      APPEND_CHANGE ("uid");
      np->dn_stat.st_uid = state->uid;
      changes++;
    }
  if (np->dn_stat.st_gid != state->gid)
    {
      APPEND_CHANGE ("gid");
      np->dn_stat.st_gid = state->gid;
      changes++;
    }
  if (np->dn_stat.st_author != state->author)
    {
      APPEND_CHANGE ("author");
      np->dn_stat.st_author = state->author;
      changes++;
    }

  if ((np->dn_stat.st_mode & 07777) != (state->st_mode & 07777))
    {
      APPEND_CHANGE ("mode new 0%o, old 0%o", state->st_mode & 07777,
		     np->dn_stat.st_mode & 07777);
      np->dn_stat.st_mode =
	(np->dn_stat.st_mode & S_IFMT) | (state->st_mode & 07777);
      changes++;
    }

  if (state->has_mtime && np->dn_stat.st_mtime < state->mtime)
    {
      APPEND_CHANGE ("mtime");
      np->dn_stat.st_mtime = state->mtime;
      changes++;
    }

  if (state->has_ctime && np->dn_stat.st_ctime < state->ctime)
    {
      APPEND_CHANGE ("ctime");	/* np->dn_stat.st_ctime ignored; dn_set_ctime below */
      changes++;
    }

  if (state->has_atime && np->dn_stat.st_atime < state->atime)
    {
      APPEND_CHANGE ("atime");
      np->dn_stat.st_atime = state->atime;
      changes++;
    }

  if (np->dn_stat.st_flags != state->flags)
    {
      APPEND_CHANGE ("flags 0x%x", state->flags);
      np->dn_stat.st_flags = state->flags;
      changes++;
    }

  if (changes > 0)
    {
#if JOURNAL_REPLAY_DRY_RUN
      JOURNAL_LOG_DEBUG ("[DRY_RUN] inode %" PRIu32 ": %d changes: [%s]",
			 +state->ino, changes, change_desc);
#else
      np->dn_stat_dirty = 1;
      np->dn_set_ctime = 1;
      diskfs_node_update (np, 0);
      JOURNAL_LOG_DEBUG ("inode %" PRIu32 ": %d changes applied: [%s]",
			 state->ino, changes, change_desc);
#endif
    }
  else
    {
      JOURNAL_LOG_DEBUG ("inode %" PRIu32 ": no changes needed.", state->ino);
    }
  return changes;
}

error_t
apply_node_replay (inode_replay_state_t *state, struct node *fs_root,
		   struct node *restore_root, struct protid *cred)
{
  if (state->ino < JOURNAL_REPLAY_MIN_INO)
    {
      JOURNAL_LOG_DEBUG ("inode %" PRIu32
			 " below REPLAY_MIN_INO (%d) skipping as potentially system-critical",
			 state->ino, JOURNAL_REPLAY_MIN_INO);
      return 0;
    }
  struct node *np = NULL;
  char full_path[JOURNAL_NORMALIZED_PATH_MAX];
  error_t err = journal_resolve_full_path (state->resolved_path, state->name,
					   full_path, sizeof (full_path));
  const char *opt_path = err ? NULL : full_path;
  resolve_result_t rr =
    resolve_existing_node_locked (opt_path, state, fs_root, cred, &np, &err);

  if (err)
    return err;
  if (rr == RES_ERROR)
    return err;
  if (rr == RES_SKIP || rr == RES_MISMATCH)
    return 0;			/* found but we intentionally skip */
  if (rr == RES_PATH_INVALID)
    {
      JOURNAL_LOG_DEBUG
	("inode %u: Ino not found and no valid path for creation; skipping.",
	 state->ino);
      return 0;
    }
  if (rr == RES_NOT_FOUND)
    err = create_node (opt_path, state, restore_root, cred, &np);

  if (err || !np)
    return err ? err : EIO;

  (void) apply_metadata_changes_locked (np, state);
  diskfs_nput (np);
  return 0;
}

