/* journal_writer.h - Interface for raw journal writer
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

#ifndef LIBDISKFS_JOURNAL_WRITER_H
#define LIBDISKFS_JOURNAL_WRITER_H

#include <libdiskfs/journal_format.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Write a batch of raw journal entries to the persistent journal file.
   The entries are assumed to be fully serialized and valid.
   Returns true on success, false on error. */
bool journal_write_raw (const struct journal_payload *entries, size_t count);

/* Write a single parsed journal payload synchronously (e.g., during boot).
   Performs internal serialization and CRC generation.
   Returns true on success, false on error. */
bool journal_write_raw_sync (struct journal_payload_bin *payload);

#endif /* LIBDISKFS_JOURNAL_WRITER_H */

