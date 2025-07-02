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

/* The journal graph takes a non-owning pointer to a journal event.
   The caller retains ownership and is responsible for freeing it after replay. */
void journal_graph_add_event (const struct journal_payload_bin *ev);
void journal_graph_print (void);
char *
journal_graph_emit_restore_script(void);
void journal_graph_free (void);

#endif  // JOURNAL_GRAPH_H
