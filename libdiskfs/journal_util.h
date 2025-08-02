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
#define JOURNAL_MAX_PATH_COMPONENTS 128
#define JOURNAL_FILENAME_MAX NAME_MAX
#define JOURNAL_PATH_MAX (JOURNAL_NORMALIZED_PATH_MAX - JOURNAL_FILENAME_MAX - 2)

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

// Simple hash function (FNV-1a variant)
static inline uint32_t
journal_hash_path (const char *path)
{
  uint32_t hash = 2166136261U;
  while (*path)
    {
      hash ^= (uint32_t) * path++;
      hash *= 16777619U;
    }
  return hash;
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

  const char *fallback = "";
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


/**
 * Splits a full absolute path into directory path and filename.
 * - `dir_out` receives the parent directory (e.g., "/foo/bar")
 * - `file_out` receives the final filename (e.g., "baz.txt")
 * - Handles edge cases like "/file.txt" → dir="/", file="file.txt"
 *
 * Returns true on success, false on invalid input or truncation.
 */
static inline bool
journal_split_path(const char *full_path,
                   char *dir_out, size_t dir_len,
                   char *file_out, size_t file_len)
{
  if (!full_path || full_path[0] != '/')
    {
      JOURNAL_LOG_DEBUG("journal_split_path: path is NULL or not absolute: '%s'", full_path);
      return false;
    }

  const char *last_slash = strrchr(full_path, '/');

  // Reject root path "/"
  if (last_slash == full_path && full_path[1] == '\0')
    {
      JOURNAL_LOG_DEBUG("journal_split_path: cannot split root path '/'");
      return false;
    }

  // Case: "/file"
  if (!last_slash || last_slash == full_path)
    {
      size_t file_part_len = strlen(full_path + 1);

      if (dir_len < 2 || file_len <= file_part_len)
        {
          JOURNAL_LOG_DEBUG("journal_split_path: buffer too small for '/file' case");
          return false;
        }

      strcpy(dir_out, "/");
      strncpy(file_out, full_path + 1, file_len - 1);
      file_out[file_len - 1] = '\0';
      return true;
    }

  // Normal case: "/path/to/file"
  size_t dir_part_len = last_slash - full_path;
  size_t file_part_len = strlen(last_slash + 1);

  if (dir_part_len >= dir_len || file_part_len >= file_len)
    {
      JOURNAL_LOG_DEBUG("journal_split_path: buffer too small for full_path='%s'", full_path);
      return false;
    }

  strncpy(dir_out, full_path, dir_part_len);
  dir_out[dir_part_len] = '\0';

  strncpy(file_out, last_slash + 1, file_len - 1);
  file_out[file_len - 1] = '\0';

  return true;
}

#endif /* LIBDISKFS_JOURNAL_UTIL_H */
