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
#include <sys/time.h>
#include <string.h>

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
#define JOURNAL_LOG_DEBUG(fmt, ...)                                          \
	do {                                                                       \
		fprintf(stderr, "[JOURNAL][DEBUG] " fmt "\n", ##__VA_ARGS__);           \
		fflush(stderr);                                                         \
	} while (0)
#else
#define JOURNAL_LOG_DEBUG(fmt, ...) do { } while (0)
#endif

#define JOURNAL_MAX_REASONABLE_TIME 16725229200	/* Jan 1, 2500 */
#define JOURNAL_MIN_REASONABLE_TIME 315536400	/* Jan 1, 1980 */

static inline void
safe_strncpy (char *dst, const char *src, size_t size)
{
  if (size == 0)
    return;
  size_t len = strnlen (src, size - 1);
  memcpy (dst, src, len);
  dst[len] = '\0';
}

static inline uint32_t
journal_compute_header_crc32 (const journal_header_t *hdr)
{
  if (!hdr)
    return 0;

  return crc32 ((const uint8_t *) hdr, offsetof (journal_header_t, crc32));
}

static inline uint32_t
journal_compute_payload_crc32 (const journal_payload_bin_t *payload)
{
  if (!payload)
    return 0;
  return crc32 ((const char *) payload, sizeof (journal_payload_bin_t));
}

/* Check if a given stat structure describes a journal-safe file.  */
static inline bool
journal_is_safe_stat (const uint32_t mode)
{
  if (mode == 0)
    return false;

  if (S_ISBLK (mode) || S_ISCHR (mode))
    return false;

  if (S_ISFIFO (mode) || S_ISSOCK (mode))
    return false;

  if (S_ISLNK (mode))
    return false;

  return S_ISREG (mode) || S_ISDIR (mode);
}

static inline uint64_t
journal_current_time_ms (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return ((uint64_t) tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

#endif /* LIBDISKFS_JOURNAL_UTIL_H */
