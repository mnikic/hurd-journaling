/* journal_inode_apply.c - replay engine with directory structure and full path reconstruction

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

#include <libdiskfs/journal_globals.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_graph.h>
#include <libdiskfs/diskfs.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <hurd/fs.h>
#include <dirent.h>
#include <stdio.h>
#include <hurd/lookup.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <hurd/fs.h>
#include <hurd/hurd_types.h>
#include <hurd/fshelp.h>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>
#include <pthread.h>

#define HASH_SIZE 4096
#define MAX_CHILDREN 32
#define MAX_PATH_LEN 512
#define PRINTED_SET_SIZE 8192
#define MAX_STACK_DEPTH 8192
#define DIRBLKSIZ block_size

static error_t
mkdir_p (const char *path, mode_t mode)
{
  LOG_DEBUG ("mkdir_p: inside: %s", path);

  char tmp[MAX_PATH_LEN];
  snprintf (tmp, sizeof (tmp), "%s", path);

  char *p = tmp;
  while (*p == '/')
    p++;

  if (*p == '\0')
    {
      LOG_DEBUG ("mkdir_p: path was root or empty after trimming: %s", path);
      return 0;
    }

  struct node *current_node = diskfs_root_node;
  diskfs_nref (current_node);

  char *token = strtok (p, "/");
  while (token)
    {
      LOG_DEBUG ("mkdir_p: processing token: %s", token);

      struct node *child_node = NULL;
      struct dirstat *ds = alloca (diskfs_dirstat_size);

      error_t err =
	diskfs_lookup_hard (current_node, token, LOOKUP, &child_node, NULL,
			    NULL);
      if (err == ENOENT)
	{
	  LOG_DEBUG ("mkdir_p: creating directory: %s", token);
	  mode_t dir_mode = mode | S_IFDIR;

	  err =
	    diskfs_create_node (current_node, token, dir_mode, &child_node,
				NULL, ds);
	  if (err && err != EEXIST)
	    {
	      LOG_DEBUG ("mkdir_p: failed to create directory %s: %s", token,
			 strerror (err));
	      diskfs_nrele (current_node);
	      return err;
	    }

	  if (!err && diskfs_synchronous)
	    {
	      diskfs_file_update (current_node, 1);
	      diskfs_file_update (child_node, 1);
	    }
	}
      else if (err)
	{
	  LOG_DEBUG ("mkdir_p: lookup failed for %s: %s", token,
		     strerror (err));
	  diskfs_nrele (current_node);
	  return err;
	}

      diskfs_nrele (current_node);
      current_node = child_node;
      token = strtok (NULL, "/");
    }

  diskfs_nrele (current_node);
  return 0;
}

static error_t
build_restore_path (const inode_state_t * inode, char *buf, size_t buflen)
{
  if (!inode || !inode->resolved_path || inode->resolved_path[0] != '/')
    return EINVAL;

  size_t prefix_len = strlen (RESTORE_DEVICE_PREFIX);
  size_t path_len = strlen (inode->resolved_path);

  if (prefix_len + path_len + 1 > buflen)
    return ENAMETOOLONG;
  snprintf (buf, buflen, "%s%s", RESTORE_DEVICE_PREFIX, inode->resolved_path);
  return 0;
}

error_t
apply_inode_state_hurd (const inode_state_t * inode)
{
  if (!inode || inode->name[0] == '\0')
    return EINVAL;

  char path[MAX_PATH_LEN];
  error_t err = build_restore_path (inode, path, sizeof (path));
  if (err)
    return err;

  struct stat st;
  if (!inode->is_deleted && stat (path, &st) == 0)
    {
      time_t file_mtime = st.st_mtime;
      if ((time_t) (inode->last_seen / 1000) < file_mtime)
	{
	  LOG_DEBUG
	    ("Stoping file %s newer (fs mtime %ld > journal last_seen %llu)",
	     path, (long) file_mtime, inode->last_seen / 1000);
	  return 0;
	}
    }

  if (inode->is_deleted)
    {
      unlink (path);
      return 0;
    }

  char *path_copy = strdup (path);
  if (!path_copy)
    return ENOMEM;
  char *parent = dirname (path_copy);
  err = mkdir_p (parent, 0755);
  free (path_copy);
  if (err)
    return err;

  LOG_DEBUG ("Skipping file creation due to internal fs constraints: %s",
	     path);
  return 0;
}

error_t
apply_inode_state_hurd2 (const inode_state_t * inode)
{
  LOG_DEBUG ("Inside apply");
  if (!inode || inode->name[0] == '\0')
    return EINVAL;

  char path[MAX_PATH_LEN];

  LOG_DEBUG ("before restore path");
  error_t err = build_restore_path (inode, path, sizeof (path));
  if (err)
    return err;

  LOG_DEBUG ("before checking is deleted etc.");
  // Skip if live file is newer than journal entry
  struct stat st;
  if (!inode->is_deleted && stat (path, &st) == 0)
    {
      time_t file_mtime = st.st_mtime;
      if ((time_t) (inode->last_seen / 1000) < file_mtime)
	{
	  LOG_DEBUG
	    ("Stoping file %s newer (fs mtime %ld > journal last_seen %llu)",
	     path, (long) file_mtime, inode->last_seen / 1000);
	  return 0;
	}
    }


  LOG_DEBUG ("If deleted");
  // Handle deletions
  if (inode->is_deleted)
    {
      unlink (path);		// safe, even if not found
      return 0;
    }


  LOG_DEBUG ("path copy");
  // Ensure full path parent directories exist
  char *path_copy = strdup (path);
  if (!path_copy)
    return ENOMEM;
  char *parent = dirname (path_copy);
  LOG_DEBUG ("after dirnam");
  mkdir_p (parent, 0755);

  LOG_DEBUG ("getting dir port");

  // Get port to parent dir
  file_t dir_port = file_name_lookup (parent, O_DIRECTORY | O_RDWR, 0);
  free (path_copy);
  if (dir_port == MACH_PORT_NULL)
    {
      return ENOENT;
    }

  file_t newfile = MACH_PORT_NULL;
  retry_type lookup_retry;
  char retry_name[1024] = { 0 };

  if (S_ISREG (inode->st_mode) || S_ISDIR (inode->st_mode))
    {

      LOG_DEBUG ("before lookup");
      err = dir_lookup (dir_port,
			(char *) inode->name,
			O_CREAT | O_RDWR,
			inode->st_mode, &lookup_retry, retry_name, &newfile);
    }
  else if (S_ISLNK (inode->st_mode))
    {
      // Create symlink
      unlink (path);		// replace if already exists
      err = symlink (inode->symlink_target, path);
      goto skip_port;
    }
  else
    {
      LOG_DEBUG ("Unsupported inode type: %s, mode: %ui", path,
		 inode->st_mode);
      goto skip_port;
    }

  if (err || newfile == MACH_PORT_NULL)
    {
      LOG_DEBUG ("Failed to create %s: %s.", path, strerror (err));
      goto skip_port;
    }

  // Set mode
  file_chmod (newfile, inode->st_mode);

  // Set ownership
  file_chown (newfile, inode->uid, inode->gid);

  // Set timestamps
  struct time_value atime = {.seconds = inode->mtime,.microseconds = 0 };
  struct time_value mtime = {.seconds = inode->ctime,.microseconds = 0 };

  file_utimes (newfile, atime, mtime);

  // Truncate regular file
  if (S_ISREG (inode->st_mode))
    {
      struct stat st;
      err = io_stat (newfile, &st);
      if (!err && st.st_size != inode->st_size)
	{
	  file_set_size (newfile, inode->st_size);
	}
    }

skip_port:
  if (newfile != MACH_PORT_NULL)
    mach_port_deallocate (mach_task_self (), newfile);
  if (dir_port != MACH_PORT_NULL)
    mach_port_deallocate (mach_task_self (), dir_port);

  return 0;
}
