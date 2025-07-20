/* journal_graph.c - Replay engine with directory structure and full path reconstruction

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

#include <libdiskfs/journal_globals.h>
#include <libdiskfs/journal_util.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_graph.h>
#include <libdiskfs/diskfs.h>
#include <hurd/fs.h>
#include <hurd/lookup.h>
#include <hurd/fshelp.h>
#include <hurd/hurd_types.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#define JOURNAL_HASH_SIZE 4096

static inode_graph_node_t *inode_hash[JOURNAL_HASH_SIZE];

static journal_ino_t
hash_ino (journal_ino_t ino)
{
  return ino % JOURNAL_HASH_SIZE;
}

static inode_graph_node_t *
get_inode (journal_ino_t ino)
{
  journal_ino_t h = hash_ino (ino);
  inode_graph_node_t *cur = inode_hash[h];
  while (cur)
    {
      if (cur->ino == ino)
        return cur;
      cur = cur->next;
    }
  inode_graph_node_t *new_node = calloc (1, sizeof (inode_graph_node_t));
  if (!new_node)
    return NULL;
  new_node->ino = ino;
  new_node->replay.ino = ino;
  new_node->next = inode_hash[h];
  inode_hash[h] = new_node;
  return new_node;
}

static void
add_child (inode_graph_node_t *parent, journal_ino_t child_ino)
{
  if (parent->num_children < JOURNAL_GRAPH_NODE_MAX_CHILDREN)
    parent->children[parent->num_children++] = child_ino;
}

static void
remove_child (inode_graph_node_t *parent, journal_ino_t child_ino)
{
  for (int i = 0; i < parent->num_children; ++i)
    {
      if (parent->children[i] == child_ino)
        {
          for (int j = i; j < parent->num_children - 1; ++j)
            parent->children[j] = parent->children[j + 1];
          parent->num_children--;
          break;
        }
    }
}

static void
safe_strncpy (char *dst, const char *src, size_t size)
{
  if (size == 0)
    return;
  size_t len = strnlen (src, size - 1);
  memcpy (dst, src, len);
  dst[len] = '\0';
}

static void
maybe_set_name (inode_replay_state_t *ino, const struct journal_payload_bin *ev)
{
  if (strlen (ino->name) == 0)
    {
      if (strlen (ev->name) > 0)
        safe_strncpy (ino->name, ev->name, sizeof (ino->name));
      else if (strlen (ev->new_name) > 0)
        safe_strncpy (ino->name, ev->new_name, sizeof (ino->name));
    }
}

/*
 * journal_graph_add_event:
 *   Applies a single journal event to the in-memory inode graph.
 *   It updates inode metadata, name, link count, and deletion status based on the action type.
 *
 *   Conservative Deletion Policy:
 *   - An inode is marked as deleted (is_deleted = true) in exactly two cases:
 *       1. RMDIR: The inode is a directory and a successful RMDIR was observed. This implies
 *          the directory was empty at deletion time, and we can safely mark both it and all
 *          of its children as deleted.
 *       2. Reliable UNLINK: The inode was created during the journal window (link_count_reliable == true),
 *          and its link count reaches zero due to one or more UNLINK operations.
 *
 *   - In all other situations  including missing CREATE events, incomplete link history, or ambiguous deletion
 *     is_deleted is not set.
 *
 *   - This conservative approach ensures that no speculative deletions occur. Data is preserved unless its
 *     deletion can be positively confirmed by the journal.
 */
void
journal_graph_add_event (const struct journal_payload_bin *ev)
{
  inode_graph_node_t *ino = get_inode (ev->ino);
  if (!ino)
    return;

  inode_replay_state_t *replay = &ino->replay;
  replay->last_tx = ev->tx_id;
  if (ev->timestamp_ms > replay->last_seen)
    replay->last_seen = ev->timestamp_ms;

  switch (ev->action)
    {
    case JOURNAL_ACTION_CREATE:
    case JOURNAL_ACTION_MKDIR:
    case JOURNAL_ACTION_MKFILE:
      ino->parent_ino = ev->parent_ino;
      safe_strncpy (replay->name, ev->name, sizeof (replay->name));
      ino->link_count = 1;
      ino->link_count_reliable = true;
      replay->is_deleted = false;
      add_child (get_inode (ev->parent_ino), ev->ino);
      break;

    case JOURNAL_ACTION_SYMLINK:
      ino->parent_ino = ev->parent_ino;
      safe_strncpy (replay->name, ev->name, sizeof (replay->name));
      safe_strncpy (replay->symlink_target, ev->target, sizeof (replay->symlink_target));
      ino->link_count = 1;
      ino->link_count_reliable = true;
      replay->is_deleted = false;
      add_child (get_inode (ev->parent_ino), ev->ino);
      break;

    case JOURNAL_ACTION_LINK:
      ino->link_count++;
      break;

    case JOURNAL_ACTION_UNLINK:
      remove_child (get_inode (ev->parent_ino), ev->ino);
      ino->link_count--;
      if (ino->link_count <= 0 && ino->link_count_reliable)
        {
          replay->is_deleted = true;
          ino->num_children = 0;
          replay->deleted_at_tx = ev->tx_id;
          replay->deleted_at_timestamp = ev->timestamp_ms;
        }
      break;

    case JOURNAL_ACTION_RMDIR:
      remove_child (get_inode (ev->parent_ino), ev->ino);
      replay->is_deleted = true;
      replay->deleted_at_tx = ev->tx_id;
      replay->deleted_at_timestamp = ev->timestamp_ms;
      for (int i = 0; i < ino->num_children; i++)
        {
          inode_graph_node_t *child = get_inode (ino->children[i]);
          if (child && !child->replay.is_deleted)
            {
              child->replay.is_deleted = true;
              child->replay.deleted_at_tx = ev->tx_id;
              child->replay.deleted_at_timestamp = ev->timestamp_ms;
            }
        }
      ino->num_children = 0;
      break;

    case JOURNAL_ACTION_RENAME:
      remove_child (get_inode (ev->src_parent_ino), ev->ino);
      add_child (get_inode (ev->dst_parent_ino), ev->ino);
      ino->parent_ino = ev->dst_parent_ino;
      safe_strncpy (replay->name, ev->new_name, sizeof (replay->name));
      break;

    case JOURNAL_ACTION_UTIME:
      break;

    case JOURNAL_ACTION_CHMOD:
      if (ev->has_mode)
        {
          replay->st_mode = ev->st_mode;
          replay->has_st_mode = true;
        }
      break;

    case JOURNAL_ACTION_CHOWN:
      if (ev->has_uid)
        {
          replay->uid = ev->uid;
          replay->has_uid = true;
        }
      if (ev->has_gid)
        {
          replay->gid = ev->gid;
          replay->has_gid = true;
        }
      break;

    case JOURNAL_ACTION_CHFLAGS:
      if (ev->has_flags)
        {
          replay->flags = ev->flags;
          replay->has_flags = true;
        }
      break;

    case JOURNAL_ACTION_CHAUTHOR:
      if (ev->has_uid)
        {
          replay->uid = ev->uid;
          replay->has_uid = true;
        }
      break;

    case JOURNAL_ACTION_TRUNCATE:
    case JOURNAL_ACTION_GROW:
      if (ev->has_size)
        {
          replay->st_size = ev->st_size;
          replay->has_st_size = true;
        }
      break;

    default:
      break;
    }

  if (ev->has_mtime && (!replay->has_mtime || ev->mtime > replay->mtime))
    {
      replay->has_mtime = true;
      replay->mtime = ev->mtime;
    }

  if (ev->has_ctime && (!replay->has_ctime || ev->ctime > replay->ctime))
    {
      replay->has_ctime = true;
      replay->ctime = ev->ctime;
    }

  if (ev->has_atime && (!replay->has_atime || ev->atime > replay->atime))
    {
      replay->has_atime = true;
      replay->atime = ev->atime;
    }

  maybe_set_name (replay, ev);
}

void
journal_graph_free (void)
{
  for (int i = 0; i < JOURNAL_HASH_SIZE; ++i)
    inode_hash[i] = NULL;
  // Memory is owned by arena
}

size_t
journal_graph_get_all (inode_replay_state_t ***out_list,
                       struct journal_arena *arena)
{
  size_t count = 0;
  inode_replay_state_t **result =
    journal_arena_alloc (arena, JOURNAL_NUM_ENTRIES * sizeof (*result));
  if (!result)
    {
      JOURNAL_LOG_ERROR ("journal_graph_get_all: arena out of memory");
      *out_list = NULL;
      return 0;
    }

  for (size_t i = 0; i < JOURNAL_HASH_SIZE; ++i)
    {
      inode_graph_node_t *node = inode_hash[i];
      while (node)
        {
          if (!node->replay.is_deleted)
            {
              if (count >= JOURNAL_NUM_ENTRIES)
                {
                  JOURNAL_LOG_DEBUG ("journal_graph_get_all: overflow > %llu entries",
                                     JOURNAL_NUM_ENTRIES);
                  break;
                }
              result[count++] = &node->replay;
            }
          node = node->next;
        }
    }

  *out_list = result;
  return count;
}

