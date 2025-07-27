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

#define JOURNAL_MAX_REASONABLE_TIME 16725229200	/* Jan 1, 2500 */
#define JOURNAL_MIN_REASONABLE_TIME 315536400	/* Jan 1, 1980 */
#define JOURNAL_MAX_PATH_COMPONENTS 128

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

static inline uint64_t
journal_current_time_ms (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return ((uint64_t) tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

static inline const char *
journal_normalize_path (const char *input)
{
  static char normalized[JOURNAL_NORMALIZED_PATH_MAX];
  const char *components[JOURNAL_MAX_PATH_COMPONENTS];
  int depth = 0;

  if (!input || input[0] == '\0')
    return "";

  // Skip leading slashes
  while (*input == '/')
    input++;

  while (*input && depth < JOURNAL_MAX_PATH_COMPONENTS)
    {
      // Get next component
      const char *start = input;
      while (*input && *input != '/')
	input++;
      size_t len = input - start;

      // Skip over any slashes
      while (*input == '/')
	input++;

      if (len == 0)
	continue;		// repeated slashes or trailing slash

      if (len == 1 && start[0] == '.')
	continue;		// skip .

      if (len == 2 && start[0] == '.' && start[1] == '.')
	{
	  if (depth > 0)
	    depth--;		// pop one
	  continue;
	}

      // Save pointer to this component
      components[depth++] = start;
    }

  // Join components
  char *out = normalized;
  size_t remaining = JOURNAL_NORMALIZED_PATH_MAX;

  if (depth == 0)
    {
      snprintf (out, remaining, ".");
      return normalized;
    }

  for (int i = 0; i < depth; i++)
    {
      size_t len = 0;
      while (components[i][len] && components[i][len] != '/')
	len++;

      if (len + 1 >= remaining)
	break;

      *out++ = '/';
      memcpy (out, components[i], len);
      out += len;
      remaining -= (len + 1);
    }

  *out = '\0';
  return normalized;
}

static inline void
journal_combine_path_name (const char *path, const char *name,
			   char *out, size_t out_size)
{
  if (!out || out_size == 0)
    return;

  const char *fallback = "?";
  out[0] = '\0';		// always null-terminate early

  if ((!path || !*path) && (!name || !*name))
    {
      snprintf (out, out_size, "%s", fallback);
      return;
    }

  if (!name || !*name)
    {
      snprintf (out, out_size, "%s", path);
      return;
    }

  if (!path || !*path)
    {
      snprintf (out, out_size, "%s", name);
      return;
    }

  size_t path_len = strlen (path);
  size_t name_len = strlen (name);
  bool needs_slash = path[path_len - 1] != '/';

  // If already ends in name, don't append
  if (path_len >= name_len && strcmp (path + path_len - name_len, name) == 0)
    {
      snprintf (out, out_size, "%s", path);
      return;
    }

  // Manual safe concatenation (avoids warning)
  size_t remaining = out_size;
  size_t written = 0;

  written = snprintf (out, remaining, "%s", path);
  if (written >= remaining)
    return;

  remaining -= written;
  out += written;

  if (needs_slash)
    {
      written = snprintf (out, remaining, "/");
      if (written >= remaining)
	return;
      remaining -= written;
      out += written;
    }

  snprintf (out, remaining, "%s", name);	// truncate if needed
}

#endif /* LIBDISKFS_JOURNAL_UTIL_H */
