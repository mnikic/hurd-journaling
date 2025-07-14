/* journal_graph.h - Journal graph state of the filesystem metadata

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

#ifndef JOURNAL_GRAPH_H
#define JOURNAL_GRAPH_H

#include <stdint.h>
#include <stdbool.h>
#include <libdiskfs/journal_format.h>
#include <hurd/fs.h>

#define MAX_CHILDREN 32

typedef struct inode_replay_state
{
  journal_ino_t ino; // Always required during replay for identification/logging
  bool is_deleted;   // Strong signal: only set when deletion is certain (RMDIR or reliable UNLINK). Never speculative.
  char *resolved_path;

  uint64_t last_seen;     // Timestamp of the last journal event for this inode, used by replay to skip stale restores
  uint64_t last_tx;       // Transaction ID of the last modification (for ordering during replay)
  uint64_t deleted_at_tx;
  uint64_t deleted_at_timestamp;

  char name[MAX_FIELD_LEN];            // Current name of the inode (may come from SYMLINK/CREATE/RENAME)
  char symlink_target[MAX_FIELD_LEN];  // Set if inode is a symlink

  uint32_t st_mode;
  bool has_st_mode;
  uint64_t st_size;
  bool has_st_size;

  int64_t mtime;
  bool has_mtime;
  int64_t ctime;        // Updated on metadata changes (e.g., mode, ownership, size, rename). Not initialized at creation time.
  bool has_ctime;

  journal_uid_t uid;
  bool has_uid;
  journal_uid_t gid;
  bool has_gid;
} inode_replay_state_t;

/* The journal graph takes a non-owning pointer to a journal event.
   The caller retains ownership and is responsible for freeing it after replay. */
void journal_graph_add_event (const struct journal_payload_bin *ev);
void journal_graph_print (void);
char *
journal_graph_emit_restore_script(void);
error_t
scan_directory_and_update_paths (void);
void journal_graph_free (void);

#endif  // JOURNAL_GRAPH_H
