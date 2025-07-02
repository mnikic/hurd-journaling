/* journal_format.h - Binary journal entry format definitions

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

#ifndef JOURNAL_FORMAT_H
#define JOURNAL_FORMAT_H

#include <stdint.h>
#include <sys/types.h>

#define JOURNAL_MAGIC        0x4A4E4C30  /* "JNL0" */
#define JOURNAL_VERSION      1
#define MAX_FIELD_LEN        256
#define JOURNAL_ENTRY_SIZE   4096

struct journal_entry_bin
{
  uint32_t magic;
  uint32_t version;
  uint64_t tx_id;
  uint64_t timestamp_ms;
  ino_t parent_ino;
  ino_t src_parent_ino;
  ino_t dst_parent_ino;
  ino_t ino;
  uint32_t st_mode;
  uint64_t st_size;
  uint64_t st_nlink;
  uint64_t st_blocks;
  int64_t mtime;
  int64_t ctime;
  char action[MAX_FIELD_LEN];
  char name[MAX_FIELD_LEN];
  char old_name[MAX_FIELD_LEN];
  char new_name[MAX_FIELD_LEN];
  char extra[MAX_FIELD_LEN];
  uint32_t crc32;
};

struct journal_payload
{
  const char *data;
  size_t len;
};

#endif /* JOURNAL_FORMAT_H */

