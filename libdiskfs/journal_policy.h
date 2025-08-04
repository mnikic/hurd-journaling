/* journal_policy.h - Journal policy engine for events eligibility desicions.

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

#ifndef LIBDISKFS_JOURNAL_POLICY_H
#define LIBDISKFS_JOURNAL_POLICY_H

#include <stdbool.h>
#include <libdiskfs/journal_inode_denylist.h>
#include <libdiskfs/diskfs.h>
#include <journal.h>

// Prefixes to exclude from journaling based on path
static const char *const journal_excluded_prefixes[] = {
  "/tmp",
  "/hurd",
  "/dev",
  "/proc",
  "/sys",
  "/run",
  "/var/tmp",
  "/var/run",
  "/var/lock",
  "/var/cache",
  "/var/lib/dpkg",
  "/var/log",
  "/usr/lib",
  "/lib",
  "/boot",
  NULL
};

bool
journal_should_log_event (const struct node *np,
			  const journal_entry_info_t *info,
			  const journal_inode_denylist_t * ino_denylist,
			  const char *full_path);

bool
journal_good_dir_path (const char *path);

bool
journal_good_filename (const char *path);

#endif /*  LIBDISKFS_JOURNAL_POLICY_H */
