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
#include <libdiskfs/journal_internal.h>
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
node_lookup (journal_ino_t ino)
{
  inode_graph_node_t *cur = inode_hash[hash_ino (ino)];
  while (cur)
    {
      if (cur->ino == ino)
	return cur;
      cur = cur->next;
    }
  return NULL;
}

static inode_graph_node_t *
get_inode (journal_ino_t ino, struct journal_arena *arena)
{
  journal_ino_t h = hash_ino (ino);
  inode_graph_node_t *cur = inode_hash[h];
  while (cur)
    {
      if (cur->ino == ino)
	return cur;
      cur = cur->next;
    }
  inode_graph_node_t *new_node =
    journal_arena_alloc (arena, sizeof (inode_graph_node_t));
  if (!new_node)
    {
      JOURNAL_LOG_ERROR
	("Out of memory. Not able to allocate a graph node. Aborting.");
      return NULL;
    }
  memset (new_node, 0, sizeof (inode_graph_node_t));
  new_node->ino = ino;
  new_node->replay.ino = ino;
  new_node->is_real = false;
  new_node->next = inode_hash[h];
  inode_hash[h] = new_node;
  return new_node;
}

static bool
add_child (inode_graph_node_t * parent, journal_ino_t child_ino,
	   struct journal_arena *arena)
{
  for (size_t i = 0; i < parent->num_children; ++i)
    if (parent->children[i] == child_ino)
      return true;

  if (parent->num_children == parent->children_capacity)
    {
      size_t new_capacity =
	parent->children_capacity == 0 ? 4 : parent->children_capacity * 2;
      size_t bytes = new_capacity * sizeof (journal_ino_t);
      journal_ino_t *new_array = journal_arena_alloc (arena, bytes);
      if (!new_array)
	{
	  JOURNAL_LOG_ERROR
	    ("add_child: out of memory expanding children array for inode %u",
	     parent->ino);
	  return false;
	}
      if (parent->children)
	memcpy (new_array, parent->children,
		parent->num_children * sizeof (journal_ino_t));
      parent->children = new_array;
      parent->children_capacity = new_capacity;
    }

  parent->children[parent->num_children++] = child_ino;
  return true;
}

static void
remove_child (inode_graph_node_t * parent, journal_ino_t child_ino)
{
  for (size_t i = 0; i < parent->num_children; ++i)
    {
      if (parent->children[i] == child_ino)
	{
	  // Swap with last element
	  parent->children[i] = parent->children[parent->num_children - 1];
	  parent->num_children--;
	  return;
	}
    }
  JOURNAL_LOG_DEBUG ("remove_child: child %u not found in parent %u",
		     child_ino, parent->ino);
}

/*
* Marks the node, and all of its descendants dead.
*/
static bool
mark_inode_dead (journal_ino_t root_ino)
{
  journal_ino_t stack[4096];
  int top = 0;

  stack[top++] = root_ino;

  while (top > 0)
    {
      journal_ino_t ino = stack[--top];
      inode_graph_node_t *node = node_lookup (ino);
      if (!node)
	continue;

      for (size_t i = 0; i < node->num_children; i++)
	{
	  if (top >= 4096)
	    {
	      JOURNAL_LOG_ERROR ("delete_inode_iterative: stack overflow");
	      return false;
	    }
	  stack[top++] = node->children[i];
	}
      node->num_children = 0;
      node->is_dead = true;
    }
  return true;
}

bool
journal_graph_add_event (const struct journal_payload_bin *ev,
			 struct journal_arena *arena)
{
  if (ev->st_nlink == 0 || ev->action == JOURNAL_ACTION_TOMBSTONE)
    {
      return mark_inode_dead (ev->ino);
    }

  inode_graph_node_t *ino = get_inode (ev->ino, arena);
  if (!ino)
    return false;
  if (ino->is_dead)
    {
      JOURNAL_LOG_DEBUG ("Ino: %u Skipping updates to the tombstone node",
			 ino->ino);
      return true;
    }
  if (ino->is_real && ev->st_gen != ino->replay.st_gen)
    {
      JOURNAL_LOG_DEBUG
	("Deleting inode %u from metadata. Generation is different. Event: %u. Expected get: %u, encountered gen: %u.",
	 ev->ino, ev->action, ino->replay.st_gen, ev->st_gen);
      return mark_inode_dead (ev->ino);
    }
  bool is_resize_action =
    (ev->action == JOURNAL_ACTION_GROW ||
     ev->action == JOURNAL_ACTION_TRUNCATE ||
     ev->action == JOURNAL_ACTION_WRITE);
  if (ino->is_real && !is_resize_action && ev->st_size != ino->replay.st_size)
    {
      JOURNAL_LOG_DEBUG
	("Deleting inode %u from metadata. Size changed on a non size changing event. Event: %u. Expected size: %llu, encountered size: %llu.",
	 ev->ino, ev->action, ino->replay.st_size, ev->st_size);
      return mark_inode_dead (ev->ino);
    }
  inode_replay_state_t *replay = &ino->replay;
  if (ev->timestamp_ms < replay->last_seen)
    {
      JOURNAL_LOG_DEBUG
	("Skipping out-of-order event for inode %u: timestamp %llu < last_seen %llu",
	 ev->ino, ev->timestamp_ms, replay->last_seen);
      //return mark_inode_dead (ev->ino);
      return true;
    }

  switch (ev->action)
    {
    case JOURNAL_ACTION_SYMLINK:
      safe_strncpy (replay->symlink_target, ev->target,
		    sizeof (replay->symlink_target));
      // falls through
    case JOURNAL_ACTION_CREATE:
    case JOURNAL_ACTION_MKDIR:
    case JOURNAL_ACTION_MKFILE:
      ino->parent_ino = ev->parent_ino;
      if (!add_child (get_inode (ev->parent_ino, arena), ev->ino, arena))
	return false;
      break;

    case JOURNAL_ACTION_RMDIR:
      return mark_inode_dead (ev->ino);

    case JOURNAL_ACTION_RENAME:
      inode_graph_node_t * dst = get_inode (ev->dst_parent_ino, arena);
      if (!dst || dst->is_dead)
	return mark_inode_dead (ev->ino);
      inode_graph_node_t *src = get_inode (ev->src_parent_ino, arena);
      remove_child (src, ev->ino);
      if (!add_child (dst, ev->ino, arena))
	return false;
      ino->parent_ino = ev->dst_parent_ino;
    default:
    }
  replay->st_size = ev->st_size;
  ino->is_real = true;
  replay->last_tx = ev->tx_id;
  replay->last_seen = ev->timestamp_ms;
  replay->st_blocks = ev->st_blocks;
  replay->st_nlink = ev->st_nlink;
  replay->st_mode = ev->st_mode;
  replay->st_gen = ev->st_gen;

  if (ev->path[0] != '\0')
    {
      safe_strncpy (replay->resolved_path, ev->path,
		    JOURNAL_NORMALIZED_PATH_MAX);
      safe_strncpy (replay->name, ev->name, sizeof (replay->name));
    }

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
  if (ev->has_flags)
    {
      replay->flags = ev->flags;
      replay->has_flags = true;
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

  return true;
}

void
journal_graph_free (void)
{
  for (int i = 0; i < JOURNAL_HASH_SIZE; ++i)
    inode_hash[i] = NULL;	// Memory owned by arena
}

size_t
journal_graph_get_all (inode_replay_state_t *** out_list,
		       struct journal_arena *arena)
{
  size_t count = 0;
  inode_replay_state_t **result = journal_arena_alloc (arena,
						       journal_layout.num_entries
						       * sizeof (*result));

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
	  if (!node->is_dead && node->is_real)
	    result[count++] = &node->replay;
	  node = node->next;
	}
    }

  *out_list = result;
  return count;
}
