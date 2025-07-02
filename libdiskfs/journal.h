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

#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdbool.h>
#include <sys/types.h>

struct journal_entry_info
{
	const char *action;           /* "create", "unlink", "rename", etc. */
	const char *name;             /* Affected file name */
	ino_t parent_ino;             /* For actions involving directories */
	const char *old_name;         /* For rename */
	const char *new_name;         /* For rename */
	ino_t src_parent_ino;         /* For rename */
	ino_t dst_parent_ino;         /* For rename */
	uid_t uid;		      /* For chown */
	uid_t gid;		      /* For chown */
	off_t size;                   /* For truncate, extend */
	mode_t mode;	              /* For mkdir */
	const char *extra;            /* Optional free-form field (e.g. "chmod mode=0755") */
};

typedef enum journal_durability
{
	JOURNAL_DURABILITY_ASYNC,
	JOURNAL_DURABILITY_SYNC
} journal_durability_t;

void journal_init (void);
void journal_shutdown (void);
void flush_journal_to_file (void);
void journal_log_metadata (void *node_ptr, const struct journal_entry_info *info,  journal_durability_t  durability);

#endif /* JOURNAL_H */

