/* journal_fs_helper.c - Collection of helper libdiskfs functions for journaling usage.

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

#include <libdiskfs/journal_util.h>
#include <libdiskfs/journal_path_util.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_diskfs_helper.h>
#include <pthread.h>
#include <string.h>

/**
* Create reusable diskfs protid credentials for a given node.
* Caller must ports_port_deref(cred) when done.
* Flags are passed to diskfs_make_peropen.
*/
error_t
diskfs_create_creds (struct node *np, int flags, struct protid **out_cred)
{
  error_t err;
  struct peropen *po = NULL;

  err = diskfs_make_peropen (np, flags, 0, &po);
  if (err)
    return err;

  err = diskfs_create_protid (po, 0, out_cred);
  if (err)
    {
      ports_port_deref (po);
      return err;
    }

  return 0;
}

error_t
diskfs_lookup_path (const struct node *np, const char *path,
		    struct protid *cred, struct node **out_np)
{
  if (!path)
    return EINVAL;

  // Skip leading slashes
  while (*path == '/')
    ++path;

  // Handle empty path (root directory)
  if (*path == '\0')
    {
      diskfs_nref ((struct node *) np);
      *out_np = (struct node *) np;
      return 0;
    }

  struct node *current = (struct node *) np;
  diskfs_nref (current);

  const char *p = path;
  char component[JOURNAL_FILENAME_MAX + 1];

  while (*p)
    {
      const char *slash = strchr (p, '/');
      size_t len = slash ? (size_t) (slash - p) : strlen (p);

      if (len == 0)
	{
	  // Skip empty components (consecutive slashes)
	  p = slash + 1;
	  while (*p == '/')
	    ++p;
	  continue;
	}
      if (len > JOURNAL_FILENAME_MAX)
	{
	  diskfs_nput (current);
	  return ENAMETOOLONG;
	}

      memcpy (component, p, len);
      component[len] = '\0';

      struct node *next = NULL;
      error_t err =
	diskfs_lookup_hard (current, component, LOOKUP, &next, NULL, cred);

      // Release current node before checking error
      diskfs_nput (current);

      if (err)
	return err;

      // next is already locked and referenced by diskfs_lookup_hard
      current = next;

      if (!slash)
	break;

      p = slash + 1;
      // Skip consecutive slashes
      while (*p == '/')
	++p;
    }

  // current is already locked and referenced
  *out_np = current;
  return 0;
}

/**
 * Create a file named `filename` under `dir`, using Hurd diskfs APIs.
 *
 * `dir` must be LOCKED on entry.
 * If the file already exists, the existing node is returned locked via `*out`.
 * If the file is created, the new node is returned locked via `*out`.
 *
 * Caller must unlock and `diskfs_nput(*out)` after use.
 */
error_t
diskfs_make_file (struct node *dir, const char *filename, struct protid *cred,
		  struct node **out)
{
  error_t err;
  struct node *new_node = NULL;
  struct dirstat *ds = alloca (diskfs_dirstat_size);

  err = diskfs_lookup (dir, filename, CREATE, &new_node, ds, cred);
  if (err == EAGAIN || err == 0)
    {
      *out = new_node;
      diskfs_drop_dirstat (dir, ds);
      return 0;
    }
  else if (err != ENOENT)
    {
      JOURNAL_LOG_ERROR ("lookup(CREATE) failed: %s", strerror (err));
      diskfs_drop_dirstat (dir, ds);
      return err;
    }

  mode_t mode = S_IFREG | 0644;
  err = diskfs_create_node (dir, filename, mode, &new_node, cred, ds);
  if (err)
    {
      JOURNAL_LOG_ERROR ("create_node failed: %s", strerror (err));
      diskfs_drop_dirstat (dir, ds);
      return err;
    }

  diskfs_node_update (new_node, 1);
  *out = new_node;

  diskfs_drop_dirstat (dir, ds);
  return 0;
}

/**
 * Create a directory named `dirname` under `root`, using Hurd diskfs APIs.
 *
 * `root` must be LOCKED on entry.
 * If the directory already exists, the existing node is returned locked via `*out`.
 * If the directory is created, the new node is returned locked via `*out`.
 *
 * Caller must `diskfs_nput(*out)` after use.
 */
error_t
diskfs_make_dir (struct node *root, const char *dirname, struct protid *cred,
		 struct node **out)
{
  error_t err = 0;
  struct node *new_node = NULL;
  struct dirstat *ds = alloca (diskfs_dirstat_size);

  err = diskfs_lookup (root, dirname, CREATE, &new_node, ds, cred);
  if (err == EAGAIN || err == 0)
    {
      JOURNAL_LOG_DEBUG ("Directory already exists.");
      *out = new_node;
      err = 0;
      goto cleanup;
    }
  else if (err != ENOENT)
    {
      JOURNAL_LOG_ERROR ("lookup(CREATE) failed: %s.", strerror (err));
      goto cleanup;
    }

  mode_t mode = S_IFDIR | 0755;
  err = diskfs_create_node (root, dirname, mode, &new_node, cred, ds);
  if (err)
    {
      JOURNAL_LOG_ERROR ("create_node failed: %s.", strerror (err));
      if (new_node)
	diskfs_nput (new_node);
      goto cleanup;
    }

  diskfs_node_update (new_node, 1);
  *out = new_node;

cleanup:
  diskfs_drop_dirstat (root, ds);
  return err;
}

/**
 * Recursively create all intermediate directories in a path relative to `root`.
 * Caller must ensure `root` is LOCKED.
 *
 * We nref the initial root once, then iteratively walk and nput previous nodes.
 * Each call to `diskfs_make_dir()` returns a locked+referenced node.
 */
error_t
diskfs_mkdir_p (struct node *root, const char *path, struct protid *cred)
{
  if (strlen (path) >= JOURNAL_NORMALIZED_PATH_MAX)
    return ENAMETOOLONG;

  char path_copy[JOURNAL_NORMALIZED_PATH_MAX];
  strncpy (path_copy, path, JOURNAL_NORMALIZED_PATH_MAX);
  path_copy[JOURNAL_NORMALIZED_PATH_MAX - 1] = '\0';
  char *token = strtok (path_copy, "/");

  if (!token)
    return 0;

  diskfs_nref (root);
  while (token != NULL)
    {
      JOURNAL_LOG_DEBUG ("Token: %s", token);
      struct node *next_node = NULL;
      error_t err = diskfs_make_dir (root, token, cred, &next_node);
      if (err)
	{
	  JOURNAL_LOG_ERROR
	    ("diskfs_mkdir_p: make_dir failed on '%s' with err %d", token,
	     err);
	  diskfs_nput (root);
	  return err;
	}
      diskfs_nput (root);
      root = next_node;
      token = strtok (NULL, "/");
    }
  diskfs_nput (root);
  return 0;
}
