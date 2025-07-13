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

typedef struct inode_state
{
	journal_ino_t ino;
	journal_ino_t parent_ino;
	char name[MAX_FIELD_LEN];
	uint64_t last_tx;
	uint64_t last_seen;
	int link_count; // Reflects relative changes from journaled LINK/UNLINK events. Only meaningful if link_count_reliable == true (i.e., inode was created during journal window).
	bool link_count_reliable; // Only valid if inode was seen created. Link count is speculative otherwise and must not be used to infer deletion.
	bool is_deleted; // Strong signal: only set when deletion is certain (RMDIR or reliable UNLINK). Never speculative.

	uint64_t deleted_at_tx;
	uint64_t deleted_at_timestamp;

	uint32_t st_mode;
	bool has_st_mode;
	uint64_t st_size;
	bool has_st_size;
	int64_t mtime;
	bool has_mtime;
	int64_t ctime; // Updated on metadata changes (e.g., mode, ownership, size, rename). Not initialized at creation time.
	bool has_ctime; // Set when metadata (mode, ownership, size, etc.) changes
	journal_uid_t uid;
	bool has_uid;
	journal_uid_t gid;
	bool has_gid;

	char symlink_target[MAX_FIELD_LEN];

	journal_ino_t children[MAX_CHILDREN];
	int num_children;
	char *resolved_path;

	struct inode_state *next;
} inode_state_t;

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
