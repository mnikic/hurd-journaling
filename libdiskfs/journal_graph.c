/* journal_graph.c - replay engine with directory structure and full path reconstruction

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
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_graph.h>
#include <libdiskfs/diskfs.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <inttypes.h>

#define HASH_SIZE 4096
#define MAX_CHILDREN 32
#define MAX_PATH_LEN 512
#define PRINTED_SET_SIZE 8192
#define MAX_STACK_DEPTH 8192

// Extended action enum
typedef enum
{
  ACTION_CREATE,
  ACTION_MKDIR,
  ACTION_MKFILE,
  ACTION_SYMLINK,
  ACTION_LINK,
  ACTION_UNLINK,
  ACTION_RENAME,
  ACTION_RMDIR,
  ACTION_CHMOD,
  ACTION_CHOWN,
  ACTION_UTIME,
  ACTION_TRUNCATE,
  ACTION_UNKNOWN
} journal_action_t;

typedef struct inode_state
{
  journal_ino_t ino;
  journal_ino_t parent_ino;
  char name[MAX_FIELD_LEN];
  uint64_t last_tx;
  uint64_t last_seen;
  int link_count;
  bool is_deleted;

  uint64_t deleted_at_tx;
  uint64_t deleted_at_timestamp;

  uint32_t st_mode;
  uint64_t st_size;
  int64_t mtime;
  int64_t ctime;
  journal_uid_t uid;
  journal_uid_t gid;

  char symlink_target[MAX_FIELD_LEN];

  journal_ino_t children[MAX_CHILDREN];
  int num_children;

  struct inode_state *next;
} inode_state_t;

static inode_state_t *inode_hash[HASH_SIZE];
bool printed_set[PRINTED_SET_SIZE];

static journal_ino_t
hash_ino (journal_ino_t ino)
{
  return ino % HASH_SIZE;
}

static inode_state_t *
get_inode (journal_ino_t ino)
{
  journal_ino_t h = hash_ino (ino);
  inode_state_t *cur = inode_hash[h];
  while (cur)
    {
      if (cur->ino == ino)
	return cur;
      cur = cur->next;
    }
  inode_state_t *new_node = calloc (1, sizeof (inode_state_t));
  new_node->ino = ino;
  new_node->next = inode_hash[h];
  inode_hash[h] = new_node;
  return new_node;
}

static void
add_child (inode_state_t * parent, journal_ino_t child_ino)
{
  if (parent->num_children < MAX_CHILDREN)
    {
      parent->children[parent->num_children++] = child_ino;
    }
}

static void
remove_child (inode_state_t * parent, journal_ino_t child_ino)
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
get_full_path (journal_ino_t ino, char *buf, size_t buflen)
{
  if (ino == 0 || buflen == 0)
    {
      snprintf (buf, buflen, "/?");
      return;
    }
  inode_state_t *parts[MAX_PATH_LEN / 2];
  int count = 0;

  inode_state_t *cur = get_inode (ino);
  while (cur && cur->ino != 0 && !cur->is_deleted && count < MAX_PATH_LEN / 2)
    {
      parts[count++] = cur;
      if (cur->parent_ino == 0)
	break;
      cur = get_inode (cur->parent_ino);
    }

  buf[0] = '\0';
  for (int i = count - 1; i >= 0; --i)
    {
      strncat (buf, "/", buflen - strlen (buf) - 1);
      strncat (buf, parts[i]->name, buflen - strlen (buf) - 1);
    }
  if (strlen (buf) == 0)
    snprintf (buf, buflen, "/");
}

static journal_action_t
action_from_string (const char *action_str)
{
  if (strcmp (action_str, "create") == 0)
    return ACTION_CREATE;
  if (strcmp (action_str, "mkdir") == 0)
    return ACTION_MKDIR;
  if (strcmp (action_str, "mkfile") == 0)
    return ACTION_MKFILE;
  if (strcmp (action_str, "symlink") == 0)
    return ACTION_SYMLINK;
  if (strcmp (action_str, "link") == 0)
    return ACTION_LINK;
  if (strcmp (action_str, "unlink") == 0)
    return ACTION_UNLINK;
  if (strcmp (action_str, "rename") == 0)
    return ACTION_RENAME;
  if (strcmp (action_str, "rmdir") == 0)
    return ACTION_RMDIR;
  if (strcmp (action_str, "chmod") == 0)
    return ACTION_CHMOD;
  if (strcmp (action_str, "chown") == 0)
    return ACTION_CHOWN;
  if (strcmp (action_str, "utimes") == 0
      || strcmp (action_str, "(utimes)") == 0)
    return ACTION_UTIME;
  if (strcmp (action_str, "truncate") == 0
      || strcmp (action_str, "grow") == 0)
    return ACTION_TRUNCATE;
  LOG_DEBUG ("Unknown action %s", action_str);
  return ACTION_UNKNOWN;
}

typedef struct journal_payload_bin journal_event_t;

void
journal_graph_add_event (const struct journal_payload_bin *ev)
{
  journal_action_t action = action_from_string (ev->action);
  inode_state_t *ino = get_inode (ev->ino);
  ino->last_tx = ev->tx_id;
  ino->last_seen = ev->timestamp_ms;
  if (strlen(ev->name) == 0)
  	LOG_DEBUG("action %s doesn't have name. new name = %s.", ev->action, ev->new_name);
  switch (action)
    {
    case ACTION_CREATE:
    case ACTION_MKDIR:
    case ACTION_MKFILE:
      {
	ino->parent_ino = ev->parent_ino;
	strncpy (ino->name, ev->name, sizeof (ino->name));
	ino->link_count = 1;
	ino->is_deleted = false;
	inode_state_t *parent = get_inode (ev->parent_ino);
	add_child (parent, ev->ino);
	break;
      }
    case ACTION_SYMLINK:
      {
	ino->parent_ino = ev->parent_ino;
	strncpy (ino->name, ev->name, sizeof (ino->name));
	strncpy (ino->symlink_target, ev->target,
		 sizeof (ino->symlink_target));
	ino->link_count = 1;
	ino->is_deleted = false;
	inode_state_t *parent = get_inode (ev->parent_ino);
	add_child (parent, ev->ino);
	break;
      }
    case ACTION_LINK:
      ino->link_count++;
      break;
    case ACTION_UNLINK:
    case ACTION_RMDIR:
      {
	inode_state_t *parent = get_inode (ev->parent_ino);
	remove_child (parent, ev->ino);
	if (--ino->link_count <= 0)
	  {
	    ino->is_deleted = true;
	    ino->num_children = 0;
	    ino->deleted_at_tx = ev->tx_id;
	    ino->deleted_at_timestamp = ev->timestamp_ms;
	  }
	break;
      }
    case ACTION_RENAME:
      {
	inode_state_t *old_parent = get_inode (ev->src_parent_ino);
	inode_state_t *new_parent = get_inode (ev->dst_parent_ino);
	remove_child (old_parent, ev->ino);
	add_child (new_parent, ev->ino);
	ino->parent_ino = ev->dst_parent_ino;
	strncpy (ino->name, ev->new_name, sizeof (ino->name));
	break;
      }
    case ACTION_UTIME:
      ino->mtime = ev->timestamp_ms;
      break;
    case ACTION_CHMOD:
      if (ev->has_mode)
	ino->st_mode = ev->st_mode;
      break;
    case ACTION_CHOWN:
      if (ev->has_uid)
	ino->uid = ev->uid;
      if (ev->has_gid)
	ino->gid = ev->gid;
      break;
    case ACTION_TRUNCATE:
      if (ev->has_size)
	ino->st_size = ev->st_size;
      break;
    default:
      break;
    }
  if (strlen (ino->name) == 0)
    {
      if (strlen (ev->name) > 0)
	strncpy (ino->name, ev->name, sizeof (ino->name));
      else if (strlen (ev->new_name) > 0)
	strncpy (ino->name, ev->new_name, sizeof (ino->name));
    }
}

static void
print_inode_tree (journal_ino_t root_ino)
{
  typedef struct
  {
    uint32_t ino;
    int child_index;
  } StackFrame;

  StackFrame stack[MAX_STACK_DEPTH];
  int top = 0;

  stack[top++] = (StackFrame)
  {
  root_ino, 0};

  while (top > 0)
    {
      StackFrame *frame = &stack[top - 1];
      inode_state_t *inode = get_inode (frame->ino);

      if (frame->child_index == 0
	  && !printed_set[inode->ino % PRINTED_SET_SIZE])
	{
	  printed_set[inode->ino % PRINTED_SET_SIZE] = true;
	  char path[MAX_PATH_LEN];
	  get_full_path (inode->ino, path, sizeof (path));
	  LOG_DEBUG
	    ("%s (ino: %u, mode: %u, uid: %u, gid: %u, size: %llu, mtime: %lld, ctime: %lld)",
	     path, inode->ino, inode->st_mode, inode->uid, inode->gid,
	     inode->st_size, inode->mtime, inode->ctime);
	}

      if (frame->child_index < inode->num_children)
	{
	  stack[top++] = (StackFrame)
	  {
	  inode->children[frame->child_index++], 0};
	  if (top >= MAX_STACK_DEPTH)
	    return;
	}
      else
	{
	  top--;
	}
    }
}

void
journal_graph_print (void)
{
  LOG_DEBUG ("Filesystem Tree:");
  memset (printed_set, 0, sizeof (printed_set));

  for (int i = 0; i < HASH_SIZE; ++i)
    {
      inode_state_t *cur = inode_hash[i];
      while (cur)
	{
	  if (cur->ino != 0 && cur->parent_ino == 0 && !cur->is_deleted)
	    {
	      print_inode_tree (cur->ino);
	    }
	  cur = cur->next;
	}
    }

  LOG_DEBUG ("Deleted Inodes:");
  for (int i = 0; i < HASH_SIZE; ++i)
    {
      inode_state_t *cur = inode_hash[i];
      while (cur)
	{
	  if (cur->ino != 0 && cur->is_deleted)
	    {
	      char path[MAX_PATH_LEN];
	      get_full_path (cur->ino, path, sizeof (path));
	      LOG_DEBUG ("- %s (ino: %u) deleted at tx %" PRIu64
			 ", timestamp %" PRIu64 "\n", path, cur->ino,
			 cur->deleted_at_tx, cur->deleted_at_timestamp);
	    }
	  cur = cur->next;
	}
    }
}

void
journal_graph_free (void)
{
  for (int i = 0; i < HASH_SIZE; ++i)
    {
      inode_state_t *cur = inode_hash[i];
      while (cur)
	{
	  inode_state_t *next = cur->next;
	  free (cur);		// Free each allocated inode_state
	  cur = next;
	}
      inode_hash[i] = NULL;	// Clear the bucket
    }
}

static void
build_path_recursive(inode_state_t *node, char *buf, size_t buflen)
{
  if (!node)
    {
      strncat(buf, "/[null]", buflen - strlen(buf) - 1);
      return;
    }

  if (node->ino == 2) // ext2 root inode
    return; // base case

  if (node->parent_ino == node->ino || node->parent_ino == 0)
    {
      char fallback[32];
      snprintf(fallback, sizeof(fallback), "/[ino-%" PRIu32 "]", node->ino);
      strncat(buf, fallback, buflen - strlen(buf) - 1);
      return;
    }

  inode_state_t *parent = get_inode(node->parent_ino);
  if (!parent)
    {
      char fallback[32];
      snprintf(fallback, sizeof(fallback), "/[ino-%" PRIu32 "]", node->parent_ino);
      strncat(buf, fallback, buflen - strlen(buf) - 1);
    }
  else
    {
      build_path_recursive(parent, buf, buflen);
    }

  strncat(buf, "/", buflen - strlen(buf) - 1);

  if (strlen(node->name) > 0)
    {
      char name_buf[MAX_FIELD_LEN + 1];
      strncpy(name_buf, node->name, MAX_FIELD_LEN);
      name_buf[MAX_FIELD_LEN] = '\0';
      strncat(buf, name_buf, buflen - strlen(buf) - 1);
    }
  else
    {
      char fallback[32];
      snprintf(fallback, sizeof(fallback), "[ino-%" PRIu32 "]", node->ino);
      strncat(buf, fallback, buflen - strlen(buf) - 1);
    }
}

char *
journal_graph_emit_restore_script(void)
{
  size_t buffer_size = 16384;
  size_t used = 0;
  char *script = malloc(buffer_size);
  if (!script)
    return NULL;

  used += snprintf(script + used, buffer_size - used, "#!/bin/sh\n");
  used += snprintf(script + used, buffer_size - used, "# Auto-generated restore script from journal\n\n");
  used += snprintf(script + used, buffer_size - used, "mkdir -p /restore\n");

  for (size_t i = 0; i < HASH_SIZE; ++i)
    {
      inode_state_t *node = inode_hash[i];
      while (node)
        {
          if (node->ino == 0 || node->is_deleted)
            {
              node = node->next;
              continue;
            }

          if (strlen(node->name) == 0)
            {
              fprintf(stderr, "\u26a0\ufe0f inode %" PRIu32 " has no name\n", node->ino);
            }

          char path[1024] = "/restore";
          build_path_recursive(node, path + strlen("/restore"), sizeof(path) - strlen("/restore"));

          if (strcmp(path, "/restore") == 0)
            {
              node = node->next;
              continue;
            }

          if (used > buffer_size - 512)
            {
              buffer_size *= 2;
              char *new_script = realloc(script, buffer_size);
              if (!new_script)
                {
                  free(script);
                  return NULL;
                }
              script = new_script;
            }

          mode_t mode = node->st_mode & S_IFMT;
          bool is_dir = (mode == S_IFDIR);

          if (is_dir)
            {
              used += snprintf(script + used, buffer_size - used,
                               "[ -d '%s' ] || install -d -o %d -g %d -m 0%03o '%s'\n",
                               path, node->uid, node->gid, node->st_mode & 0777, path);
            }
          else
            {
              used += snprintf(script + used, buffer_size - used,
                               "[ -f '%s' ] || install -o %d -g %d -m 0%03o /dev/null '%s'\n",
                               path, node->uid, node->gid, node->st_mode & 0777, path);
            }

          if (node->mtime)
            {
              used += snprintf(script + used, buffer_size - used,
                               "touch -d @%" PRIu64 " '%s'\n",
                               node->mtime / 1000, path);
            }

          node = node->next;
        }
    }

  return script;
}
