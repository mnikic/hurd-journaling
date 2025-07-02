/* journal_graph.h - In-memory inode graph for journal replay

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

#ifndef LIBDISKFS_JOURNAL_GRAPH_H
#define LIBDISKFS_JOURNAL_GRAPH_H

#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_arena.h>
#include <stdint.h>
#include <stdbool.h>

#define JOURNAL_GRAPH_NODE_MAX_CHILDREN 32

/* Final resolved state for a single inode, used during replay */
typedef struct inode_replay_state
{
  journal_ino_t ino;         /* Always required during replay */
  bool is_deleted;           /* Strong signal: only set when deletion is certain. Never speculative. */
  char *resolved_path;       /* Set by path resolution scan */

  uint64_t last_seen;        /* Last event timestamp (to skip stale entries) */
  uint64_t last_tx;          /* Last transaction affecting this inode */
  uint64_t deleted_at_tx;
  uint64_t deleted_at_timestamp;

  char name[MAX_FIELD_LEN];
  char symlink_target[MAX_FIELD_LEN];

  uint32_t st_mode;
  bool has_st_mode;

  uint64_t st_size;
  bool has_st_size;

  int64_t mtime;
  bool has_mtime;

  int64_t ctime;
  bool has_ctime;

  journal_uid_t uid;
  bool has_uid;

  journal_uid_t gid;
  bool has_gid;

  uint32_t flags;
  bool has_flags;
} inode_replay_state_t;

/* Internal graph node used to track parent/child relations and final replay state */
typedef struct inode_graph_node
{
  journal_ino_t ino;
  journal_ino_t parent_ino;

  int link_count;
  bool link_count_reliable;

  journal_ino_t children[JOURNAL_GRAPH_NODE_MAX_CHILDREN];
  int num_children;

  inode_replay_state_t replay;

  struct inode_graph_node *next;
} inode_graph_node_t;

/* Add a journal event to the graph. Caller retains ownership of the event. */
void journal_graph_add_event (const struct journal_payload_bin *ev);

/* Free internal graph structures. */
void journal_graph_free (void);

/* Return a list of all resolved replay states (allocated in arena).
   Caller gets ownership of the list. */
size_t journal_graph_get_all (inode_replay_state_t ***out_list,
                              struct journal_arena *arena);

#endif /* LIBDISKFS_JOURNAL_GRAPH_H */

