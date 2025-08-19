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

#ifndef LIBDISKFS_JOURNAL_FORMAT_H
#define LIBDISKFS_JOURNAL_FORMAT_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <libdiskfs/journal.h>
#include <libdiskfs/journal_internal.h>
#include <libdiskfs/journal_config.h>

#define JOURNAL_MAGIC    0x4A4E4C30	/* "JNL0" */
#define JOURNAL_VERSION  1
#define MAX_FIELD_LEN    256
#define JOURNAL_ENTRY_SIZE 4096ULL
#define JOURNAL_HEADER_SIZE 4096ULL
#define JOURNAL_NORMALIZED_PATH_MAX 1024

typedef uint32_t journal_ino_t;
typedef uint32_t journal_uid_t;
typedef struct journal_header journal_header_t;
typedef struct journal_entry_bin journal_entry_bin_t;
typedef struct journal_payload_bin journal_payload_bin_t;

/* On-disk and binary representation of a single journaled metadata event.
   This structure is packed and written to the journal device.  */
struct __attribute__((__packed__)) journal_payload_bin
{
  /* Transaction data */
  uint64_t tx_id;
  uint64_t timestamp_ms;
  journal_record_type_t type;

  /* Inode and parent relationships */
  journal_ino_t ino;
  journal_ino_t parent_ino;
  journal_ino_t src_parent_ino;
  journal_ino_t dst_parent_ino;

  uint32_t st_mode;
  uint64_t st_size;
  uint64_t st_nlink;
  uint64_t st_blocks;
  uint32_t st_gen;
  int64_t mtime;
  int64_t ctime;
  int64_t atime;

  journal_uid_t uid;
  journal_uid_t gid;
  journal_uid_t author;
  uint32_t flags;

  /* Presence flags */
  bool has_atime;
  bool has_ctime;
  bool has_mtime;

  /* Operation type */
  journal_action_t action;

  /* Associated strings */
  char name[MAX_FIELD_LEN];
  char old_name[MAX_FIELD_LEN];
  char new_name[MAX_FIELD_LEN];
  char target[MAX_FIELD_LEN];
  char path[JOURNAL_NORMALIZED_PATH_MAX];
  char extra[MAX_FIELD_LEN];
};

/* Header of the journal, keeping the state of iteration across reboots */
struct __attribute__((packed, aligned (JOURNAL_HEADER_SIZE))) journal_header
{
  uint32_t magic;
  uint32_t version;
  uint64_t start_index;
  uint64_t end_index;
  uint32_t crc32;

  uint8_t padding[JOURNAL_HEADER_SIZE
		  - (sizeof (uint32_t) * 3 + sizeof (uint64_t) * 2)];
};

_Static_assert (sizeof (journal_header_t) == JOURNAL_HEADER_SIZE,
		"journal_header must be JOURNAL_HEADER_SIZE bytes.");

struct
  __attribute__((__packed__, aligned (JOURNAL_ENTRY_SIZE))) journal_entry_bin
{
  uint32_t magic;
  uint32_t version;
  struct journal_payload_bin payload;
  uint32_t crc32;

  uint8_t padding[JOURNAL_ENTRY_SIZE - sizeof (uint32_t) * 2 -
		  sizeof (journal_payload_bin_t) - sizeof (uint32_t)];
};

_Static_assert (sizeof (journal_payload_bin_t) <=
		(JOURNAL_ENTRY_SIZE - sizeof (uint32_t) - sizeof (uint32_t) -
		 sizeof (uint32_t)),
		"journal_payload_bin too large to fit in journal_entry_bin");

_Static_assert (sizeof (journal_entry_bin_t) == JOURNAL_ENTRY_SIZE,
		"journal_entry_bin must be JOURNAL_ENTRY_SIZE bytes");

#endif /* LIBDISKFS_JOURNAL_FORMAT_H */
