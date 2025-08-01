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
#include <libdiskfs/journal_fs_helper.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/diskfs.h>
#include <inttypes.h>
#include <stdio.h>

#define APPEND_CHANGE(fmt, ...)                                     \
  do {                                                              \
    int n = snprintf(change_desc + desc_len,                        \
                     sizeof(change_desc) - desc_len,                \
                     "%s" fmt, first ? "" : ", ", ##__VA_ARGS__);   \
    if (n > 0 && (desc_len + (size_t)n) < sizeof(change_desc))      \
      desc_len += (size_t)n;                                        \
    first = false;                                                  \
  } while (0)


static error_t
find_by_path_or_create (journal_ino_t ino, char *path,
			struct node *restore_root, struct protid *cred,
			struct node **out)
{
  //TODO make a more robust path validation!
  if (!journal_is_valid_path (path))
    {
      return EINVAL;
    }
  struct node *np = NULL;
  // Lets try lookup by path if we can!
  error_t err = diskfs_lookup_path (path, cred, &np);
  if (!err)
    {
      goto OUT;
    }
  if (err != ENOENT)
    {
      return err;
    }
  JOURNAL_LOG_DEBUG ("inode %" PRIu32
		     ": Node not found. Creating new one. Path: %s",
		     ino, path);
  err = journal_path_recreate (path, restore_root, cred, &np);
  if (err)
    {
      JOURNAL_LOG_ERROR ("Failed to recreate file %s. Error: %s",
			 path, strerror (err));
      return err;
    }
  JOURNAL_LOG_DEBUG ("inode %" PRIu32
		     ": Recreated path %s. Final file ino: %"
		     PRIu64, ino, path, np->dn_stat.st_ino);
OUT:
  *out = np;
  return 0;
}

error_t
apply_node_replay (inode_replay_state_t * state, struct node *restore_root,
		   struct protid *cred)
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
  if (!err && np->dn_stat.st_mode > 0 && np->dn_stat.st_nlink > 0)
    {
      if (!journal_is_safe_stat (&np->dn_stat))
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
      if (!journal_is_valid_path (path))
	{
	  JOURNAL_LOG_DEBUG ("Inode: %u cannot be found AND has an invalid path: %s. Skipping.",
			     state->ino, path);
	}
      return 0;
      // Then action
      //err =
      //find_by_path_or_create (state->ino, path, restore_root, cred, &np);
      // All has failed
      //if (!np)
      //return err;
    }
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

  if (state->has_st_mode &&
      (np->dn_stat.st_mode & 07777) != (state->st_mode & 07777))
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

  if (state->has_flags && np->dn_stat.st_flags != state->flags)
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
