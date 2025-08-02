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
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_fs_helper.h>
#include <libdiskfs/journal_policy.h>
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


static inline bool
is_path_usable (const char *path)
{
  if (!path || path[0] == '\0')
    return false;

  if (path[0] != '/')
    return false;

  if (strlen (path) < 4)
    return false;

  if (strlen (path) >= JOURNAL_NORMALIZED_PATH_MAX)
    return false;

  return true;
}
static error_t
journal_resolve_full_path(const char *path, const char *name,
                          char *out_buf, size_t buf_len)
{
  if (!is_path_usable(path))
    return EINVAL;

  if (!name || name[0] == '\0')
    {
      strncpy(out_buf, path, buf_len);
      out_buf[buf_len - 1] = '\0';
      return 0;
    }

  size_t path_len = strlen(path);
  size_t name_len = strlen(name);

  if (path_len + 1 + name_len >= buf_len)
    {
      JOURNAL_LOG_DEBUG("Path + name too long: '%s' + '%s'", path, name);
      return EINVAL;
    }

  if (path_len >= name_len)
    {
      const char *end = path + path_len - name_len;
      if (strcmp(end, name) == 0 && strchr(end, '/') == NULL)
        {
          // Path already ends with name
          strncpy(out_buf, path, buf_len);
          out_buf[buf_len - 1] = '\0';
          return 0;
        }
      else if (strcmp(end, name) != 0 && strchr(end, '/') == NULL)
        {
          JOURNAL_LOG_DEBUG("Rejected: path='%s' and name='%s' both appear to be filenames.", path, name);
          return EINVAL;
        }
    }

  // Safe concatenation without snprintf warning
  strncpy(out_buf, path, buf_len);
  out_buf[buf_len - 1] = '\0';

  if (path[path_len - 1] != '/')
    strncat(out_buf, "/", buf_len - strlen(out_buf) - 1);

  strncat(out_buf, name, buf_len - strlen(out_buf) - 1);

  return 0;
}

static error_t
find_or_create_directory(const char *full_path, uint32_t ino,
                         struct protid *cred, struct node **out)
{
  if (!journal_good_dir_path(full_path))
    {
      JOURNAL_LOG_DEBUG("Rejected: ino %u path='%s' is not a supported directory name.", ino, full_path);
      return EINVAL;
    }

  struct node *np = NULL;
  error_t err = diskfs_lookup_path(full_path, cred, &np);
  if (!err)
    {
      *out = np;
      return 0;
    }

  if (err != ENOENT)
    return err;

  JOURNAL_LOG_DEBUG("inode %u: Directory not found. Creating a new one. Path: %s", ino, full_path);
  // TODO: actually create it
  return 0;
}

static error_t
find_or_create_file(const char *full_path, uint32_t ino,
                    struct protid *cred, struct node **out)
{
  char dir_path[JOURNAL_PATH_MAX];
  char file_name[JOURNAL_FILENAME_MAX + 1];

  if (!journal_split_path(full_path, dir_path, sizeof(dir_path),
                          file_name, sizeof(file_name)))
    return EINVAL;

  if (!journal_good_dir_path(dir_path))
    {
      JOURNAL_LOG_DEBUG("Rejected: ino %u its directory path='%s' is not supported.", ino, dir_path);
      return EINVAL;
    }

  if (!journal_good_filename(file_name))
    {
      JOURNAL_LOG_DEBUG("Rejected: ino %u its file name '%s' is not supported.", ino, file_name);
      return EINVAL;
    }

  struct node *np = NULL;
  error_t err = diskfs_lookup_path(full_path, cred, &np);
  if (!err)
    {
      *out = np;
      return 0;
    }

  if (err != ENOENT)
    return err;

  JOURNAL_LOG_DEBUG("inode %u: File not found. Creating new one. Path: %s. Dir: '%s' File: '%s'",
                    ino, full_path, dir_path, file_name);
  // TODO: actually create it
  return 0;
}

static error_t
find_by_path_or_create(inode_replay_state_t *state,
                       struct node *restore_root, struct protid *cred,
                       struct node **out)
{
  char *path = state->resolved_path;
  char full_path[JOURNAL_NORMALIZED_PATH_MAX];

  error_t err = journal_resolve_full_path(path, state->name,
                                          full_path, sizeof(full_path));
  if (err)
    return err;

  uint32_t mode = state->st_mode;
  if (S_ISDIR(mode))
    return find_or_create_directory(full_path, state->ino, cred, out);
  else
    return find_or_create_file(full_path, state->ino, cred, out);
}

static error_t
find_by_path_or_create2 (inode_replay_state_t * state,
			struct node *restore_root, struct protid *cred,
			struct node **out)
{
  char *path = state->resolved_path;
  if (!is_path_usable (path))
    return EINVAL;

  char full_path[JOURNAL_NORMALIZED_PATH_MAX];

  if (state->name[0] == '\0')
    {
      // Safe copy of resolved path
      strncpy (full_path, path, sizeof (full_path));
      full_path[sizeof (full_path) - 1] = '\0';
    }
  else
    {
      size_t path_len = strlen (path);
      size_t name_len = strlen (state->name);

      // Defensive check: does path already end with a filename (but not the same one)?
      if (path_len >= name_len &&
	  strcmp (path + path_len - name_len, state->name) != 0 &&
	  strchr (path + path_len - name_len, '/') == NULL)
	{
	  JOURNAL_LOG_DEBUG
	    ("Rejected: ino %u path='%s' and name='%s' both appear to be filenames.",
	     state->ino, path, state->name);
	  return EINVAL;
	}

      // Otherwise, combine path + name safely
      snprintf (full_path, sizeof (full_path), "%s%s%s",
		path, (path[path_len - 1] == '/' ? "" : "/"), state->name);
    }

  uint32_t mode = state->st_mode;
  struct node *np = NULL;
  if (S_ISDIR (mode))
    {
      // here we treat the full_path as a directory path
      if (!journal_good_dir_path (full_path))
	{
	  JOURNAL_LOG_DEBUG
	    ("Rejected: ino %u path='%s' is not a supported directory name.",
	     state->ino, path);
	  return EINVAL;
	}
      // Lets try lookup by path if we can!
      error_t err = diskfs_lookup_path (full_path, cred, &np);
      if (!err)
	{
	  // We found it, nothing to do here.
	  goto OUT;
	}
      if (err != ENOENT)
	{
	  return err;
	}
      JOURNAL_LOG_DEBUG ("inode %" PRIu32
			 ": Directory not found. Creating a new one. Path: %s",
			 state->ino, full_path);
      //TODO lets actually go ahead and create it.
    }
  else
    {
      char dir_path[JOURNAL_PATH_MAX];
      char file_name[JOURNAL_FILENAME_MAX + 1];
      if (!journal_split_path (full_path, dir_path, sizeof (dir_path),
			       file_name, sizeof (file_name)))
	return EINVAL;
      if (!journal_good_dir_path (dir_path))
	{
	  JOURNAL_LOG_DEBUG
	    ("Rejected: ino %u it's directory path='%s' is not a supported directory name.",
	     state->ino, dir_path);
	  return EINVAL;
	}
      if (!journal_good_filename (file_name))
	{
	  JOURNAL_LOG_DEBUG
	    ("Rejected: ino %u it's file name '%s' is not a supported.",
	     state->ino, file_name);
	  return EINVAL;
	}
      // This is supposed to be a file!!!
      // Lets try lookup by path if we can!
      error_t err = diskfs_lookup_path (full_path, cred, &np);
      if (!err)
	{
	  // We found it, nothing to do here.
	  goto OUT;
	}
      if (err != ENOENT)
	{
	  return err;
	}
      JOURNAL_LOG_DEBUG ("inode %" PRIu32
			 ": File not found. Creating new one. Path: %s. Dir: '%s' and file: '%s'",
			 state->ino, full_path, dir_path, file_name);
      /*err = journal_path_recreate (path, restore_root, cred, &np);
         if (err)
         {
         JOURNAL_LOG_ERROR ("Failed to recreate file %s. Error: %s",
         path, strerror (err));
         return err;
         }
         JOURNAL_LOG_DEBUG ("inode %" PRIu32
         ": Recreated path %s. Final file ino: %"
         PRIu64, ino, path, np->dn_stat.st_ino);
       */
    }
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
      JOURNAL_LOG_DEBUG ("inode %" PRIu32
			 " found! mode is %o and links %u",
			 state->ino, np->dn_stat.st_mode, np->dn_stat.st_nlink);
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
       JOURNAL_LOG_DEBUG ("It doesn't seem file ino: %u is there. Lets see if we can find it by name.", state->ino);
      // Then action
      err = find_by_path_or_create (state, restore_root, cred, &np);
      // All has failed
      if (!np)
	return err;
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
