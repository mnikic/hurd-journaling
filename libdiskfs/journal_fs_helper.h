/* journal_fs_helper.h - Collection of helper libdiskfs functions for journaling usage.

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

#ifndef LIBDISKFS_JOURNAL_FS_HELPER_H
#define LIBDISKFS_JOURNAL_FS_HELPER_H

#include <libdiskfs/diskfs.h>

error_t
diskfs_create_creds (struct node *np, int flags, struct protid **out_cred);

error_t
diskfs_lookup_path (const struct node *np, const char *path,
		    struct protid *cred, struct node **out_np);

error_t
diskfs_make_file (struct node *dir, const char *filename, struct protid *cred,
		  struct node **out);

error_t
diskfs_mkdir_p (struct node *root, const char *path, struct protid *cred,
		struct node **out);

#endif /* LIBDISKFS_JOURNAL_FS_HELPER_H */
