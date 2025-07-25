/* journal_util.h - Journaling macros, helper functions and compilation flags.

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

#ifndef LIBDISKFS_JOURNAL_UTIL_H
#define LIBDISKFS_JOURNAL_UTIL_H

#include <libdiskfs/journal_format.h>
#include <libdiskfs/diskfs.h>
#include <libdiskfs/journal_inode_denylist.h>
#include <libdiskfs/crc32.h>
#include <libdiskfs/journal_globals.h>

#include <stdio.h>

#ifndef JOURNAL_DEBUG
#define JOURNAL_DEBUG 1		/* Set to enable (very chatty) debug messages. */
#endif

#define JOURNAL_LOG_ERROR(fmt, ...)                            \
	do                                                           \
{                                                          \
	fprintf (stderr, "[JOURNAL][ERROR] " fmt "\n", ##__VA_ARGS__); \
	fflush (stderr);                                         \
}                                                          \
while (0)

#if JOURNAL_DEBUG
#define JOURNAL_LOG_DEBUG(fmt, ...)                            \
	do                                                           \
{                                                          \
	fprintf (stderr, "[JOURNAL][DEBUG] " fmt "\n", ##__VA_ARGS__); \
	fflush (stderr);                                         \
}                                                          \
while (0)
#else
#define JOURNAL_LOG_DEBUG(fmt, ...) do { } while (0)
#endif

/* Compute the byte offset of a journal entry given its index.  */
static inline uint64_t
index_to_offset (const uint64_t index)
{
  return JOURNAL_RESERVED_SPACE
    + (index % (uint64_t) JOURNAL_NUM_ENTRIES)
    * (uint64_t) JOURNAL_ENTRY_SIZE;
}

static inline uint32_t
journal_compute_header_crc32 (const journal_header_t * hdr)
{
  if (!hdr)
    return 0;

  return crc32 ((const uint8_t *) hdr, offsetof (journal_header_t, crc32));
}

static inline uint32_t
journal_compute_payload_crc32 (const journal_payload_bin_t * payload)
{
  if (!payload)
    return 0;
  return crc32 ((const char *) payload, sizeof (journal_payload_bin_t));
}

/* Check if a given stat structure describes a journal-safe file.  */
static inline bool
journal_is_safe_stat (const struct stat *st)
{
  if (st->st_mode == 0)
    return false;

  if (S_ISBLK (st->st_mode) || S_ISCHR (st->st_mode))
    return false;

  if (S_ISFIFO (st->st_mode) || S_ISSOCK (st->st_mode))
    return false;

  if (S_ISLNK (st->st_mode))
    return false;

  return S_ISREG (st->st_mode) || S_ISDIR (st->st_mode);
}

#endif /* LIBDISKFS_JOURNAL_UTIL_H */
