/* journal_apply.h - Replay journaled inode metadata updates

   Copyright (C) 2025 Free Software Foundation, Inc.

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

#ifndef LIBDISKFS_JOURNAL_APPLY_H
#define LIBDISKFS_JOURNAL_APPLY_H

#include <libdiskfs/journal_graph.h>
#include <libdiskfs/diskfs.h>
#include <stdint.h>
#include <errno.h>

/* Apply final metadata state from journal replay to a single inode.
   Updates mode, size, timestamps, ownership, etc. if different. */
error_t apply_node_replay (inode_replay_state_t * state,
			   struct node *restore_root, struct protid *cred);

#endif /* LIBDISKFS_JOURNAL_APPLY_H */
