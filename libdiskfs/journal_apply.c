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
#include <libdiskfs/journal_path_util.h>
#include <libdiskfs/journal_util.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_fs_helper.h>
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
			  const inode_replay_state_t *state)
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

/* Common lookup logic used by file/dir create paths. Validates match if found. */
static error_t
lookup_node_and_check_fingerprint (struct node *fs_root,
				   const char *path,
				   const inode_replay_state_t *state,
				   struct protid *cred, struct node **out)
{
  struct node *np = NULL;
  error_t err = diskfs_lookup_path (fs_root, path, cred, &np);
  if (!err && np)
    {
      if (!node_matches_fingerprint (np, state))
	{
	  JOURNAL_LOG_DEBUG ("Node %" PRIu64
			     " found for path %s but doesn't match fingerprint. Skipping.",
			     np->dn_stat.st_ino, path);
	  diskfs_nput (np);
	  *out = NULL;
	  return EINVAL;
	}
      *out = np;
      return 0;
    }

  if (err != ENOENT)
    {
      *out = NULL;
      return err;
    }

  return ENOENT;		// Explicitly signal not found
}

/* Creates directory path (mkdir -p style). */
static error_t
create_directory (struct node *restore_root, const char *dir_path,
		  struct protid *cred, struct node **out)
{
  struct node *created = NULL;
  error_t err = diskfs_mkdir_p (restore_root, dir_path, cred, &created);
  if (err)
    {
      JOURNAL_LOG_ERROR ("Failed to create directory '%s': %s",
			 dir_path, strerror (err));
      *out = NULL;
      return err;
    }

  if (created)
    diskfs_nput (created);

  err = diskfs_lookup_path (restore_root, dir_path, cred, out);
  if (err || !*out)
    {
      JOURNAL_LOG_ERROR ("Failed to re-lookup created directory '%s': %s",
			 dir_path, strerror (err));
      *out = NULL;
      return err ? err : EIO;
    }

  return 0;
}

/* Validates and locates (or creates) a directory node. */
static error_t
find_or_create_directory (struct node *fs_root, struct node *restore_root,
			  const char *dir_path,
			  const inode_replay_state_t *state,
			  struct protid *cred, struct node **out)
{
  if (!journal_good_dir_path (dir_path))
    {
      JOURNAL_LOG_DEBUG
	("Rejected: ino %u path='%s' is not a supported directory name.",
	 state->ino, dir_path);
      return EINVAL;
    }

  error_t err =
    lookup_node_and_check_fingerprint (fs_root, dir_path, state, cred, out);
  if (err == 0 || err != ENOENT)
    return err;

  JOURNAL_LOG_DEBUG ("Inode %u: directory not found. Creating: %s",
		     state->ino, dir_path);
  return create_directory (restore_root, dir_path, cred, out);
}

/* Validates and creates a file at the given path, if it does not already exist. */
static error_t
find_or_create_file (struct node *fs_root, struct node *restore_root,
		     const char *full_path, const inode_replay_state_t *state,
		     struct protid *cred, struct node **out)
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
      JOURNAL_LOG_DEBUG ("Rejected: ino %u directory path='%s' is invalid.",
			 state->ino, dir_path);
      *out = NULL;
      return EINVAL;
    }

  if (!journal_good_filename (file_name))
    {
      JOURNAL_LOG_DEBUG ("Rejected: ino %u filename '%s' is invalid.",
			 state->ino, file_name);
      *out = NULL;
      return EINVAL;
    }

  error_t err =
    lookup_node_and_check_fingerprint (fs_root, full_path, state, cred, out);
  if (err == 0 || err != ENOENT)
    return err;

  JOURNAL_LOG_DEBUG
    ("Inode %u: file not found. Creating: %s (dir: %s, name: %s)", state->ino,
     full_path, dir_path, file_name);

  struct node *dir = NULL;
  err = create_directory (restore_root, dir_path, cred, &dir);
  if (err || !dir)
    {
      *out = NULL;
      return err;
    }

  struct node *file = NULL;
  err = diskfs_make_file (dir, file_name, cred, &file);
  if (diskfs_synchronous)
    {
      diskfs_file_update (file, 1);
      diskfs_file_update (dir, 1);
    }
  diskfs_nput (dir);

  if (err)
    {
      JOURNAL_LOG_ERROR ("Inode %u: failed to create file '%s': %s",
			 state->ino, full_path, strerror (err));
      *out = NULL;
      return err;
    }

  JOURNAL_LOG_DEBUG ("Inode %u: file created at %s. New ino: %" PRIu64,
		     state->ino, full_path, file->dn_stat.st_ino);
  *out = file;
  return 0;
}

/* Determines whether the inode is a dir or file and applies appropriate creation logic. */
error_t
find_by_path_or_create (const char *full_path,
			const inode_replay_state_t *state,
			struct node *fs_root, struct node *restore_root,
			struct protid *cred, struct node **out)
{
  if (S_ISDIR (state->st_mode))
    return find_or_create_directory (fs_root, restore_root, full_path, state,
				     cred, out);
  else
    return find_or_create_file (fs_root, restore_root, full_path, state, cred,
				out);
}

static bool
should_skip_inode (const inode_replay_state_t *state, struct node *np)
{
  if (!journal_is_safe_stat (np->dn_stat.st_mode))
    return true;
  if ((int64_t) np->dn_stat.st_mtime < 0
      || (int64_t) np->dn_stat.st_ctime < 0)
    return true;
  if ((uint64_t) np->dn_stat.st_ctime >= state->last_seen)
    return true;
  return false;
}

static int
apply_metadata_changes (struct node *np, const inode_replay_state_t *state,
			const char *path)
{
  int changes = 0;
  char change_desc[128];
  change_desc[0] = '\0';
  size_t desc_len = 0;
  bool first = true;

  if (state->has_uid && state->uid != (uid_t) - 1
      && np->dn_stat.st_uid != state->uid)
    {
      APPEND_CHANGE ("uid");
      np->dn_stat.st_uid = state->uid;
      changes++;
    }

  if (state->has_gid && state->gid != (gid_t) - 1
      && np->dn_stat.st_gid != state->gid)
    {
      APPEND_CHANGE ("gid");
      np->dn_stat.st_gid = state->gid;
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
      changes++;
    }

  if (state->has_atime && np->dn_stat.st_atime < state->atime)
    {
      APPEND_CHANGE ("atime");
      np->dn_stat.st_atime = state->atime;
      changes++;
    }

  if (state->has_flags && np->dn_stat.st_flags != state->flags)
    {
      APPEND_CHANGE ("flags 0x%x", state->flags);
      np->dn_stat.st_flags = state->flags;
      changes++;
    }

  if (changes > 0)
    {
#if JOURNAL_REPLAY_DRY_RUN
      JOURNAL_LOG_DEBUG
	("[DRY_RUN] inode %u: path %s, %d metadata changes would be applied: [%s]",
	 state->ino, path, changes, change_desc);
#else
      np->dn_stat_dirty = 1;
      np->dn_set_ctime = 1;
      diskfs_node_update (np, 0);
      JOURNAL_LOG_DEBUG
	("inode %u: path %s, %d metadata changes applied: [%s]", state->ino,
	 path, changes, change_desc);
#endif
    }
  else
    {
      JOURNAL_LOG_DEBUG ("inode %u: path %s, no changes needed", state->ino,
			 path);
    }

  return changes;
}

error_t
apply_node_replay (inode_replay_state_t *state, struct node *fs_root,
		   struct node *restore_root, struct protid *cred)
{
  if (state->ino < JOURNAL_REPLAY_MIN_INO)
    {
      JOURNAL_LOG_DEBUG ("inode %u below REPLAY_MIN_INO (%d), skipping",
			 state->ino, JOURNAL_REPLAY_MIN_INO);
      return 0;
    }

  struct node *np = NULL;
  char full_path[JOURNAL_NORMALIZED_PATH_MAX] = { 0 };
  error_t err = diskfs_cached_lookup ((ino_t) state->ino, &np);

  if (!err && np && node_matches_fingerprint (np, state))
    {
      if (should_skip_inode (state, np))
	{
	  diskfs_nput (np);
	  return 0;
	}
    }
  else
    {
      if (np)
	diskfs_nput (np);
      err = journal_resolve_full_path (state->resolved_path, state->name,
				       full_path, sizeof (full_path));
      if (err)
	return err;
      err =
	find_by_path_or_create (full_path, state, fs_root, restore_root, cred,
				&np);
      if (!np)
	return err;
    }

  apply_metadata_changes (np, state,
			  full_path[0] ? full_path : state->resolved_path);
  diskfs_nput (np);
  return 0;
}
