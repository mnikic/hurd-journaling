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

#include <libdiskfs/journal_apply.h>
#include <libdiskfs/journal_config.h>
#include <libdiskfs/journal_util.h>
#include <libdiskfs/journal_path_util.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_diskfs_helper.h>
#include <libdiskfs/journal_policy.h>
#include <libdiskfs/diskfs.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define APPEND_CHANGE(fmt, ...)                                     \
  do {                                                              \
    int n = snprintf(change_desc + desc_len,                        \
                     sizeof(change_desc) - desc_len,                \
                     "%s" fmt, first ? "" : ", ", ##__VA_ARGS__);   \
    if (n > 0 && (desc_len + (size_t)n) < sizeof(change_desc))      \
      desc_len += (size_t)n;                                        \
    first = false;                                                  \
  } while (0)


static inline bool
node_matches_fingerprint (const struct node *np,
			  const inode_replay_state_t * state)
{
  const struct stat *st = &np->dn_stat;
  if (st->st_size != state->st_size || st->st_blocks != state->st_blocks
      || st->st_nlink != state->st_nlink || st->st_gen != state->st_gen)
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

/* Lookup `path` under `fs_root` and verify it matches `state` fingerprint.
   On success: returns 0 and sets *out to a LOCKED node (caller must nput()).
   On ENOENT: returns ENOENT and sets *out = NULL (caller may create).
   On fingerprint mismatch: returns EINVAL and sets *out = NULL (caller should NOT create).
   On other errors: returns the error and sets *out = NULL. */
static inline error_t
find_matching_node_by_path (const char *path,
			    const inode_replay_state_t * state,
			    const struct node *fs_root,
			    struct protid *cred, struct node **out)
{
  if (!path || !*path || !state || !fs_root || !out)
    return EINVAL;

  *out = NULL;

  struct node *np = NULL;
  error_t err = diskfs_lookup_path (fs_root, path, cred, &np);
  if (err)
    {
      if (err != ENOENT)
	JOURNAL_LOG_DEBUG ("lookup_path('%s') failed: %s", path,
			   strerror (err));
      return err;		/* ENOENT means "not found", others bubble up */
    }

  if (!node_matches_fingerprint (np, state))
    {
      JOURNAL_LOG_DEBUG ("Node %" PRIu64
			 " at '%s' failed fingerprint; skipping.",
			 np->dn_stat.st_ino, path);
      diskfs_nput (np);
      return EINVAL;		/* mismatch: do not create */
    }

  *out = np;			/* locked */
  return 0;
}

/* Ensure directory exists under restore_root:
 * - Calls diskfs_mkdir_p(restore_root, dir_path, cred)
 * - Re-looks up the directory and returns a LOCKED node in *out
 * - If diskfs_synchronous, updates the dir
 *
 * On success: returns 0 and *out is a locked node (caller must diskfs_nput()).
 * On failure: returns error and sets *out = NULL.
 *
 * restore_root must be locked by the caller.
 */
static inline error_t
mkdirp_lookup_dir_locked (struct node *restore_root,
			  const char *dir_path,
			  struct protid *cred,
			  const inode_replay_state_t * state,
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

static error_t
find_or_create_directory (struct node *fs_root, struct node *restore_root,
			  const char *dir_path,
			  const inode_replay_state_t * state,
			  struct protid *cred, struct node **out)
{
  if (!journal_good_dir_path (dir_path))
    {
      JOURNAL_LOG_DEBUG
	("Rejected: ino %u path='%s' is not a supported directory name.",
	 state->ino, dir_path);
      return EINVAL;
    }

  struct node *np = NULL;
  error_t err =
    find_matching_node_by_path (dir_path, state, fs_root, cred, &np);
  if (err != ENOENT)
    return err;

  JOURNAL_LOG_DEBUG
    ("inode %u: Directory not found. Creating a new one. Path: %s",
     state->ino, dir_path);

  struct node *dir = NULL;
  err = mkdirp_lookup_dir_locked (restore_root, dir_path, cred, state, &dir);
  if (err)
    {
      *out = NULL;
      return err;
    }
  *out = dir;
  return 0;
}

static error_t
find_or_create_file (struct node *fs_root, struct node *restore_root,
		     const char *full_path,
		     const inode_replay_state_t * state, struct protid *cred,
		     struct node **out)
{
  char dir_path[JOURNAL_PATH_MAX];
  char file_name[JOURNAL_FILENAME_MAX + 1];

  if (!journal_split_path (full_path, dir_path, sizeof (dir_path),
			   file_name, sizeof (file_name)))
    {
      *out = NULL;
      return EINVAL;
    }

  if (!journal_good_dir_path (dir_path))
    {
      JOURNAL_LOG_DEBUG
	("Rejected: ino %u its directory path='%s' is not supported.",
	 state->ino, dir_path);
      *out = NULL;
      return EINVAL;
    }

  if (!journal_good_filename (file_name))
    {
      JOURNAL_LOG_DEBUG
	("Rejected: ino %u its file name '%s' is not supported.", state->ino,
	 file_name);
      *out = NULL;
      return EINVAL;
    }

  struct node *np = NULL;
  error_t err =
    find_matching_node_by_path (full_path, state, fs_root, cred, &np);
  if (err != ENOENT)
    return err;

  JOURNAL_LOG_DEBUG
    ("inode %u: File not found. Creating new one. Path: %s. Dir: '%s' File: '%s'",
     state->ino, full_path, dir_path, file_name);
  struct node *dir = NULL;
  err = mkdirp_lookup_dir_locked (restore_root, dir_path, cred, state, &dir);
  if (err)
    {
      *out = NULL;
      return err;
    }
  err = diskfs_make_file (dir, file_name, cred, out);
  diskfs_nput (dir);
  if (err)
    {
      JOURNAL_LOG_ERROR
	("inode %u: Failed to create a file. Skipping. Path: %s. Error: %s",
	 state->ino, dir_path, strerror (err));
      *out = NULL;
      return err;
    }

  JOURNAL_LOG_DEBUG
    ("inode %u: File created. Path: %s. New Ino: %" PRIu64 "",
     state->ino, full_path, (*out)->dn_stat.st_ino);
  return 0;
}

static error_t
find_by_path_or_create (inode_replay_state_t * state, struct node *fs_root,
			struct node *restore_root, struct protid *cred,
			struct node **out)
{
  char full_path[JOURNAL_NORMALIZED_PATH_MAX];
  error_t err = journal_resolve_full_path (state->resolved_path, state->name,
					   full_path, sizeof (full_path));
  if (err)
    {
      *out = NULL;
      return err;
    }
  uint32_t mode = state->st_mode;
  if (S_ISDIR (mode))
    return find_or_create_directory (fs_root, restore_root, full_path,
				     state, cred, out);
  else
    return find_or_create_file (fs_root, restore_root, full_path, state, cred,
				out);
}

error_t
apply_node_replay (inode_replay_state_t * state, struct node *fs_root,
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
  char *path = state->resolved_path;
  error_t err = diskfs_cached_lookup ((ino_t) state->ino, &np);
  if (!err && np->dn_stat.st_mode > 0 && np->dn_stat.st_nlink > 0
      && node_matches_fingerprint (np, state))
    {
      JOURNAL_LOG_DEBUG ("inode %" PRIu32
			 " found! mode is %o and links %u that matches the fingerprint.",
			 state->ino, np->dn_stat.st_mode,
			 np->dn_stat.st_nlink);
      if (!journal_is_safe_stat (np->dn_stat.st_mode))
	{
	  diskfs_nput (np);
	  return 0;
	}

      if ((int64_t) np->dn_stat.st_mtime < 0
	  || (int64_t) np->dn_stat.st_ctime < 0)
	{
	  diskfs_nput (np);
	  return 0;
	}

      if ((uint64_t) np->dn_stat.st_ctime >= state->last_seen)
	{
	  diskfs_nput (np);
	  return 0;
	}
    }
  else
    {
      // First cleanup!
      if (np)
	{
	  diskfs_nput (np);
	  np = NULL;
	}
      JOURNAL_LOG_DEBUG
	("It doesn't seem file ino: %u is there. Lets see if we can find it by name.",
	 state->ino);
      // Then action
      err = find_by_path_or_create (state, fs_root, restore_root, cred, &np);
      // All has failed
      if (!np)
	return err;
    }
  int changes = 0;
  char change_desc[128];
  change_desc[0] = '\0';
  size_t desc_len = 0;
  bool first = true;

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
      APPEND_CHANGE ("mode new 0%o, old 0%o", state->st_mode,
		     np->dn_stat.st_mode);
      np->dn_stat.st_mode = state->st_mode;
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
      APPEND_CHANGE ("ctime");
      // setting explicitly st_ctime value will be ignored by diskfs.
      // dn_set_ctime flag is instead used without which any update 
      // becomes (silently) ignored. 
      // So we are forced to set dn_set_ctime to get anything done!
      // st_ctime will be set to current (and no other) time by the diskfs if
      // dn_set_ctime is set to 1. We will set it a bit down for any change.
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
      JOURNAL_LOG_DEBUG ("[DRY_RUN] inode %" PRIu32
			 ": path %s, %d metadata changes would be applied: [%s]",
			 state->ino, path, changes, change_desc);
#else
      np->dn_stat_dirty = 1;
      np->dn_set_ctime = 1;
      diskfs_node_update (np, 0);
      JOURNAL_LOG_DEBUG ("inode %" PRIu32
			 ": path %s, %d metadata changes applied: [%s]",
			 state->ino, path, changes, change_desc);
#endif
    }
  else
    {
      JOURNAL_LOG_DEBUG ("inode %" PRIu32 ": path %s, no changes needed", state->ino, "");	//path);
    }

  diskfs_nput (np);
  return 0;
}
