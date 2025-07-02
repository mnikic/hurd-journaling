/* journal.c - Core metadata logger for toy journaling in GNU Hurd

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

#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_queue.h>
#include <libdiskfs/journal.h>
#include <diskfs.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <stdlib.h>
#include <sys/time.h>
#include <hurd/fshelp.h>
#include <sys/stat.h>
#include <pthread.h>

#define MAX_REASONABLE_TIME 4102444800  /* Jan 1, 2100 */
#define MIN_REASONABLE_TIME 946684800   /* Jan 1, 2000 */
#define IGNORE_INODE(inode) \
  ((inode) == 82814 || (inode) == 48803 || (inode) == 49144 \
   || (inode) == 49142 || (inode) == 48795 || (inode) == 48794)

static volatile uint64_t journal_tx_id = 1;
static volatile bool journal_shutting_down;
static pthread_t journal_flusher_tid;

static uint64_t
current_time_ms (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return ((uint64_t) tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

void
journal_init (void)
{
  fprintf (stderr, "Toy journaling: journal_init() called\n");
  journal_queue_init ();
  if (pthread_create (&journal_flusher_tid, NULL, journal_flusher_thread, NULL) != 0)
    {
      fprintf (stderr, "Toy journaling: failed to create a flusher thread.\n");
      journal_shutting_down = true;
    }
  fprintf (stderr, "Toy journaling: done initializing.\n");
}

void
journal_shutdown (void)
{
  fprintf (stderr, "Toy journaling: journal_shutdown() called\n");
  journal_shutting_down = true;
  journal_queue_shutdown ();
  pthread_join (journal_flusher_tid, NULL);
}

void
flush_journal_to_file (void)
{
  journal_flush_now ();
}

void
journal_log_metadata (void *node_ptr, const struct journal_entry_info *info)
{
  if (!node_ptr)
    {
      fprintf (stderr, "Toy journaling: NULL node_ptr received in journal_log_metadata, skipping.\n");
      return;
    }
  if (!info)
    {
      fprintf (stderr, "Toy journaling: NULL info pointer received in journal_log_metadata, skipping.\n");
      return;
    }

  const struct stat *st = &((struct node *) node_ptr)->dn_stat;
  if (IGNORE_INODE (st->st_ino))
    return;

  const char *action = info->action ?: "";
  const char *name = info->name ?: "";
  const char *extra = info->extra ?: "";
  const char *old_name = info->old_name ?: "";
  const char *new_name = info->new_name ?: "";

  size_t total_size = sizeof (struct journal_entry_bin);
  if (total_size > JOURNAL_ENTRY_SIZE)
    {
      fprintf (stderr, "Toy journaling: entry too large, dropped.\n");
      return;
    }

  char *buf = calloc (1, total_size);
  if (!buf)
    return;

  struct journal_entry_bin *entry = (struct journal_entry_bin *) buf;
  memset (entry, 0, sizeof (*entry));

  entry->magic = JOURNAL_MAGIC;
  entry->version = JOURNAL_VERSION;
  entry->tx_id = ++journal_tx_id;
  entry->timestamp_ms = current_time_ms ();

  entry->parent_ino = info->parent_ino;
  entry->src_parent_ino = info->src_parent_ino;
  entry->dst_parent_ino = info->dst_parent_ino;
  entry->ino = st->st_ino;

  entry->st_mode = st->st_mode;
  entry->st_size = st->st_size;
  entry->st_nlink = st->st_nlink;
  entry->st_blocks = st->st_blocks;

  entry->mtime = (st->st_mtime > MIN_REASONABLE_TIME
                  && st->st_mtime < MAX_REASONABLE_TIME) ? st->st_mtime : -1;
  entry->ctime = (st->st_ctime > MIN_REASONABLE_TIME
                  && st->st_ctime < MAX_REASONABLE_TIME) ? st->st_ctime : -1;

  strncpy (entry->action, action, sizeof (entry->action) - 1);
  strncpy (entry->name, name, sizeof (entry->name) - 1);
  strncpy (entry->extra, extra, sizeof (entry->extra) - 1);
  strncpy (entry->old_name, old_name, sizeof (entry->old_name) - 1);
  strncpy (entry->new_name, new_name, sizeof (entry->new_name) - 1);

  entry->action[sizeof (entry->action) - 1] = '\0';
  entry->name[sizeof (entry->name) - 1] = '\0';
  entry->extra[sizeof (entry->extra) - 1] = '\0';
  entry->old_name[sizeof (entry->old_name) - 1] = '\0';
  entry->new_name[sizeof (entry->new_name) - 1] = '\0';

  entry->crc32 = 0;
  journal_enqueue (buf, total_size);
  free (buf);
}

