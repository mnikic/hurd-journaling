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
