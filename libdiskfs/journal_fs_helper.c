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
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_fs_helper.h>
#include <pthread.h>

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
diskfs_lookup_path (const char *path, struct protid *cred,
		    struct node **out_np)
{
  if (!path)
    return EINVAL;

  // Skip leading slashes
  while (*path == '/')
    ++path;

  // Handle empty path (root directory)
  if (*path == '\0')
    {
      pthread_mutex_lock (&diskfs_root_node->lock);
      diskfs_nref (diskfs_root_node);
      *out_np = diskfs_root_node;
      return 0;
    }

  struct node *current = diskfs_root_node;
  pthread_mutex_lock (&current->lock);
  diskfs_nref (current);

  const char *p = path;
  char component[NAME_MAX + 1];

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
      if (len > NAME_MAX)
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
 * `dir` must be UNLOCKED on entry.
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

  pthread_mutex_lock (&dir->lock);

  err = diskfs_lookup (dir, filename, CREATE, &new_node, ds, cred);
  if (err == EAGAIN || err == 0)
    {
      *out = new_node;
      diskfs_drop_dirstat (dir, ds);
      pthread_mutex_unlock (&dir->lock);
      return 0;
    }
  else if (err != ENOENT)
    {
      JOURNAL_LOG_ERROR ("lookup(CREATE) failed: %s", strerror (err));
      diskfs_drop_dirstat (dir, ds);
      pthread_mutex_unlock (&dir->lock);
      return err;
    }

  mode_t mode = S_IFREG | 0644;
  err = diskfs_create_node (dir, filename, mode, &new_node, cred, ds);
  if (err)
    {
      JOURNAL_LOG_ERROR ("create_node failed: %s", strerror (err));
      diskfs_drop_dirstat (dir, ds);
      pthread_mutex_unlock (&dir->lock);
      return err;
    }

  diskfs_node_update (new_node, 1);
  *out = new_node;

  diskfs_drop_dirstat (dir, ds);
  pthread_mutex_unlock (&dir->lock);
  return 0;
}

/**
 * Create a directory named `dirname` under `root`, using Hurd diskfs APIs.
 *
 * `root` must be UNLOCKED on entry.
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

  pthread_mutex_lock (&root->lock);

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
      goto cleanup;
    }

  diskfs_node_update (new_node, 1);
  *out = new_node;

cleanup:
  diskfs_drop_dirstat (root, ds);
  pthread_mutex_unlock (&root->lock);
  return err;
}

/**
 * Recursively create all intermediate directories in a path relative to `root`.
 * Uses Hurd diskfs APIs to create directories one component at a time.
 *
 * Returns a locked node corresponding to the final path component via `*out_node`.
 * Caller must `diskfs_nput(*out_node)` after use.
 */
error_t
diskfs_mkdir_p (struct node *root, const char *path, struct protid *cred,
		struct node **out_node)
{
  if (strlen (path) >= JOURNAL_NORMALIZED_PATH_MAX)
    return ENAMETOOLONG;

  char path_copy[JOURNAL_NORMALIZED_PATH_MAX];
  strncpy (path_copy, path, JOURNAL_NORMALIZED_PATH_MAX);
  path_copy[JOURNAL_NORMALIZED_PATH_MAX - 1] = '\0';

  char *token = strtok (path_copy, "/");
  struct node *prev_node = NULL;
  if (!token)
    {
      *out_node = root;
      return 0;
    }
  diskfs_nref (root);
  while (token != NULL)
    {
      JOURNAL_LOG_DEBUG ("Token: %s", token);
      struct node *next_node = NULL;
      error_t err = diskfs_make_dir (root, token, cred, &next_node);
      if (err)
	{
	  JOURNAL_LOG_ERROR ("mkdir_p: make_dir failed on '%s' with err %d",
			     token, err);
	  if (prev_node)
	    diskfs_nput (prev_node);
	  return err;
	}

      pthread_mutex_unlock (&next_node->lock);
      if (prev_node)
	diskfs_nput (prev_node);

      prev_node = root;
      root = next_node;
      token = strtok (NULL, "/");
    }

  *out_node = root;
  return 0;
}
