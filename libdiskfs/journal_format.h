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
#include <libdiskfs/journal_config.h>

#define JOURNAL_MAGIC    0x4A4E4C30  /* "JNL0" */
#define JOURNAL_VERSION  1
#define MAX_FIELD_LEN    256

typedef uint32_t journal_ino_t;
typedef uint32_t journal_uid_t;

/* On-disk and binary representation of a single journaled metadata event.
   This structure is packed and written to the journal device.  */
struct __attribute__((__packed__)) journal_payload_bin
{
  /* Transaction data */
  uint64_t tx_id;
  uint64_t timestamp_ms;

  /* Inode and parent relationships */
  journal_ino_t ino;
  journal_ino_t parent_ino;
  journal_ino_t src_parent_ino;
  journal_ino_t dst_parent_ino;

  /* Optional metadata */
  uint32_t st_mode;
  uint64_t st_size;
  uint64_t st_nlink;
  uint64_t st_blocks;
  int64_t mtime;
  int64_t ctime;
  int64_t atime;
  journal_uid_t uid;
  journal_uid_t gid;
  uint32_t flags;

  /* Presence flags */
  bool has_mode;
  bool has_size;
  bool has_uid;
  bool has_gid;
  bool has_flags;
  bool has_mtime;
  bool has_atime;
  bool has_ctime;

  /* Operation type */
  journal_action_t action;

  /* Associated strings */
  char name[MAX_FIELD_LEN];
  char old_name[MAX_FIELD_LEN];
  char new_name[MAX_FIELD_LEN];
  char target[MAX_FIELD_LEN];
  char extra[MAX_FIELD_LEN];
};

/* Internal wrapper used for passing raw binary payload from the async queue.  */
struct journal_payload
{
  const char *data;
  size_t len;
};

/* Header of the journal, keeping the state of iteration across reboots */
struct __attribute__((__packed__)) journal_header
{
	uint32_t magic;
	uint32_t version;
	uint64_t start_index;
	uint64_t end_index;
	uint32_t crc32;
};

/* Envelope of journal_payload_bin to include magic, version and crc32 and to pad it to JOURNAL_ENTRY_SIZE */
struct __attribute__((__packed__)) journal_entry_bin
{
	uint32_t magic;
	uint32_t version;
	struct journal_payload_bin payload;
	uint8_t padding[JOURNAL_ENTRY_SIZE - sizeof (uint32_t) - sizeof (uint32_t) -
		sizeof (struct journal_payload_bin) - sizeof (uint32_t)];
	uint32_t crc32;
};

_Static_assert(sizeof(struct journal_payload_bin) <=
               (JOURNAL_ENTRY_SIZE - sizeof(uint32_t) - sizeof(uint32_t) - sizeof(uint32_t)),
               "journal_payload_bin too large to fit in journal_entry_bin");

#endif /* LIBDISKFS_JOURNAL_FORMAT_H */

