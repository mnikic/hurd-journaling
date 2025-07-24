/* journal.h - Public interface for journaling metadata events

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

#ifndef LIBDISKFS_JOURNAL_H
#define LIBDISKFS_JOURNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <hurd/store.h>

/* Journaling actions representing metadata changes.  */
typedef enum
{
  JOURNAL_ACTION_CREATE,
  JOURNAL_ACTION_MKDIR,
  JOURNAL_ACTION_MKFILE,
  JOURNAL_ACTION_SYMLINK,
  JOURNAL_ACTION_LINK,
  JOURNAL_ACTION_UNLINK,
  JOURNAL_ACTION_RENAME,
  JOURNAL_ACTION_RMDIR,
  JOURNAL_ACTION_CHMOD,
  JOURNAL_ACTION_CHOWN,
  JOURNAL_ACTION_UTIME,
  JOURNAL_ACTION_TRUNCATE,
  JOURNAL_ACTION_GROW,
  JOURNAL_ACTION_CHAUTHOR,
  JOURNAL_ACTION_CHFLAGS,
  JOURNAL_ACTION_ATIME,
  JOURNAL_ACTION_UNKNOWN,
} journal_action_t;

/* Metadata event structure passed to the journaling system.  */
struct journal_entry_info
{
  /* Identity and naming.  */
  journal_action_t action;	/* e.g. "create", "unlink", "rename" */
  const char *name;		/* Affected file name */
  ino_t parent_ino;		/* Parent directory inode */

  /* Rename-specific fields.  */
  const char *old_name;
  const char *new_name;
  ino_t src_parent_ino;
  ino_t dst_parent_ino;

  /* Ownership.  */
  bool has_uid;
  uid_t uid;
  bool has_gid;
  uid_t gid;

  /* Size and permissions.  */
  bool has_size;
  off_t size;
  bool has_mode;
  mode_t mode;
  bool has_flags;
  uint32_t flags;

  /* Symlink target.  */
  const char *target;

  /* Optional string for debugging or structured extras.  */
  const char *extra;
};

/* Journaling durability mode.  */
typedef enum journal_durability
{
  JOURNAL_DURABILITY_ASYNC,
  JOURNAL_DURABILITY_SYNC
} journal_durability_t;

/* Initialize the journaling system.  */
void journal_init (struct store *store);

/* Shutdown and cleanup journaling resources.  */
void journal_shutdown (void);

/* Log a metadata operation for journaling.
   NODE_PTR is a filesystem node (e.g. struct node *).
   INFO describes the additional detals about the metadata event.
   DURABILITY controls sync/async mode.  */
void journal_log_metadata (void *node_ptr,
			   const struct journal_entry_info *info,
			   journal_durability_t durability);

/* Perform early journal replay before RPCs are enabled.  */
void journal_restore (void);

#endif /* LIBDISKFS_JOURNAL_H */

