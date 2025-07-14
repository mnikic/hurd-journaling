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
#include <stdlib.h>
#include <inttypes.h>
#include <hurd/fs.h>
#include <dirent.h>
#include <stdio.h>
#include <hurd/lookup.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <hurd/fs.h>
#include <hurd/hurd_types.h>
#include <hurd/fshelp.h>
#include <sys/stat.h>
#include <errno.h>

#define HASH_SIZE 4096
#define MAX_PATH_LEN 512
#define PRINTED_SET_SIZE 8192
#define MAX_STACK_DEPTH 8192
#define RESTORE_PATH_PREFIX "/restore"

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

typedef struct inode_graph_node
{
  journal_ino_t ino;             // Key used in graph hash table and parent-child tracking
  journal_ino_t parent_ino;

  int link_count;                // Reflects relative changes from journaled LINK/UNLINK events
  bool link_count_reliable;     // Only valid if inode was seen created. Otherwise speculative.

  journal_ino_t children[MAX_CHILDREN];
  int num_children;

  inode_replay_state_t replay;  // Final replay-relevant state (clean, minimal)

  struct inode_graph_node *next;
} inode_graph_node_t;

static inode_graph_node_t *inode_hash[HASH_SIZE];
bool printed_set[PRINTED_SET_SIZE];

static journal_ino_t
hash_ino (journal_ino_t ino)
{
  return ino % HASH_SIZE;
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
  new_node->ino = ino;
  new_node->replay.ino = ino;
  new_node->next = inode_hash[h];
  inode_hash[h] = new_node;
  return new_node;
}

static void
add_child (inode_graph_node_t *parent, journal_ino_t child_ino)
{
  if (parent->num_children < MAX_CHILDREN)
    {
      parent->children[parent->num_children++] = child_ino;
    }
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

static void
safe_strncpy (char *dst, const char *src, size_t size)
{
  if (size == 0)
    return;

  strncpy (dst, src, size - 1);
  dst[size - 1] = '\0';
}

static void
maybe_set_name (inode_replay_state_t * ino, const struct journal_payload_bin *ev)
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
 *   - In all other situations — including missing CREATE events, incomplete link history, or ambiguous deletions —
 *     is_deleted is not set.
 *
 *   - This conservative approach ensures that no speculative deletions occur. Data is preserved unless its
 *     deletion can be positively confirmed by the journal.
 */
void
journal_graph_add_event (const struct journal_payload_bin *ev)
{
  journal_action_t action = action_from_string (ev->action);
  inode_graph_node_t *ino = get_inode (ev->ino);
  inode_replay_state_t *replay = &ino->replay;
  replay->last_tx = ev->tx_id;
  replay->last_seen = ev->timestamp_ms;
  switch (action)
    {
    case ACTION_CREATE:
    case ACTION_MKDIR:
    case ACTION_MKFILE:
      {
        ino->parent_ino = ev->parent_ino;
        safe_strncpy (replay->name, ev->name, sizeof (replay->name));
        ino->link_count = 1;
        ino->link_count_reliable = true;
        replay->is_deleted = false;
        inode_graph_node_t *parent = get_inode (ev->parent_ino);
        add_child (parent, ev->ino);
        break;
      }
    case ACTION_SYMLINK:
      {
        ino->parent_ino = ev->parent_ino;
        safe_strncpy (replay->name, ev->name, sizeof (replay->name));
        safe_strncpy (replay->symlink_target, ev->target, sizeof (replay->symlink_target));
        ino->link_count = 1;
        ino->link_count_reliable = true;
        replay->is_deleted = false;
        inode_graph_node_t *parent = get_inode (ev->parent_ino);
        add_child (parent, ev->ino);
        break;
      }
    case ACTION_LINK:
      ino->link_count++;
      replay->ctime = ev->timestamp_ms;
      replay->has_ctime = true;
      break;
    case ACTION_UNLINK:
      {
        inode_graph_node_t *parent = get_inode (ev->parent_ino);
        remove_child (parent, ev->ino);
        ino->link_count--;
        replay->ctime = ev->timestamp_ms;
        replay->has_ctime = true;
        if (ino->link_count <= 0 && ino->link_count_reliable)
          {
            replay->is_deleted = true;
            ino->num_children = 0;
            replay->deleted_at_tx = ev->tx_id;
            replay->deleted_at_timestamp = ev->timestamp_ms;
          }
        break;
      }
    case ACTION_RMDIR:
      {
        inode_graph_node_t *parent = get_inode (ev->parent_ino);
        remove_child (parent, ev->ino);
        replay->is_deleted = true;
        replay->deleted_at_tx = ev->tx_id;
        replay->deleted_at_timestamp = ev->timestamp_ms;
        for (int i = 0; i < ino->num_children; i++)
          {
            inode_graph_node_t *child = get_inode (ino->children[i]);
            if (!child->replay.is_deleted)
              {
                child->replay.is_deleted = true;
                child->replay.deleted_at_tx = ev->tx_id;
                child->replay.deleted_at_timestamp = ev->timestamp_ms;
              }
          }
        ino->num_children = 0;
        break;
      }
    case ACTION_RENAME:
      {
        inode_graph_node_t *old_parent = get_inode (ev->src_parent_ino);
        inode_graph_node_t *new_parent = get_inode (ev->dst_parent_ino);
        remove_child (old_parent, ev->ino);
        add_child (new_parent, ev->ino);
        ino->parent_ino = ev->dst_parent_ino;
        safe_strncpy (replay->name, ev->new_name, sizeof (replay->name));
        replay->ctime = ev->timestamp_ms;
        replay->has_ctime = true;
        break;
      }
    case ACTION_UTIME:
      replay->mtime = ev->timestamp_ms;
      replay->has_mtime = true;
      break;
    case ACTION_CHMOD:
      if (ev->has_mode)
        {
          replay->st_mode = ev->st_mode;
          replay->has_st_mode = true;
          replay->ctime = ev->timestamp_ms;
          replay->has_ctime = true;
        }
      break;
    case ACTION_CHOWN:
      if (ev->has_uid)
        {
          replay->uid = ev->uid;
          replay->has_uid = true;
          replay->ctime = ev->timestamp_ms;
          replay->has_ctime = true;
        }
      if (ev->has_gid)
        {
          replay->gid = ev->gid;
          replay->has_gid = true;
          replay->ctime = ev->timestamp_ms;
          replay->has_ctime = true;
        }
      break;
    case ACTION_TRUNCATE:
      if (ev->has_size)
        {
          replay->st_size = ev->st_size;
          replay->has_st_size = true;
          replay->ctime = ev->timestamp_ms;
          replay->has_ctime = true;
        }
      break;
    default:
      break;
    }

  maybe_set_name (replay, ev);
}

void
journal_graph_free (void)
{
  for (int i = 0; i < HASH_SIZE; ++i)
    {
      inode_graph_node_t *cur = inode_hash[i];
      while (cur)
        {
          inode_graph_node_t *next = cur->next;
          free (cur);
          cur = next;
        }
      inode_hash[i] = NULL;
    }
}

static bool
should_skip_directory (const char *name)
{
  return strcmp (name, "lost+found") == 0 ||
    strcmp (name, "proc") == 0 ||
    strcmp (name, "sshd") == 0 ||
    strcmp (name, "dbus") == 0 ||
    strcmp (name, "crond") == 0 ||
    strcmp (name, "network") == 0 ||
    strcmp (name, "lock") == 0 ||
    strcmp (name, "shm") == 0 ||
    strstr (name, ".pid") != NULL ||
    strstr (name, ".lock") != NULL ||
    strstr (name, ".reboot") != NULL ||
    strstr (name, ".ok") != NULL || strchr (name, ':') != NULL;
}

const char *
d_type_to_str (unsigned char d_type)
{
  switch (d_type)
    {
    case DT_REG:
      return "regular file";
    case DT_DIR:
      return "directory";
    case DT_FIFO:
      return "FIFO";
    case DT_SOCK:
      return "socket";
    case DT_LNK:
      return "symlink";
    case DT_BLK:
      return "block dev";
    case DT_CHR:
      return "char dev";
    case DT_UNKNOWN:
      return "unknown";
    default:
      return "other";
    }
}


static bool
is_problematic_path (const char *path)
{
  // System process directories
  if (strncmp (path, "/proc", 5) == 0)
    return true;
  if (strncmp (path, "/sys", 4) == 0)
    return true;
  if (strncmp (path, "/dev", 4) == 0)
    return true;

  // Runtime/lock files
  if (strstr (path, ".pid") != NULL)
    return true;
  if (strstr (path, ".lock") != NULL)
    return true;
  if (strstr (path, ".socket") != NULL)
    return true;

  // Network mounts (if any)
  if (strncmp (path, "/net", 4) == 0)
    return true;
  if (strncmp (path, "/mnt", 4) == 0)
    return true;

  // Hurd-specific translators that might hang
  if (strncmp (path, "/servers", 8) == 0)
    return true;
  return false;
}

static bool
is_safe_to_traverse (const char *path, struct stat *st)
{
  // Skip device files
  if (S_ISBLK (st->st_mode) || S_ISCHR (st->st_mode))
    {
      return false;
    }

  // Skip FIFOs and sockets
  if (S_ISFIFO (st->st_mode) || S_ISSOCK (st->st_mode))
    {
      return false;
    }

  // Skip symbolic links to avoid loops
  if (S_ISLNK (st->st_mode))
    {
      return false;
    }

  // Skip known problematic paths
  if (is_problematic_path (path))
    {
      return false;
    }

  // Only traverse regular files and directories
  return S_ISREG (st->st_mode) || S_ISDIR (st->st_mode);
}

error_t
scan_directory_and_update_paths (void)
{
  const char *safe_roots[] = {
    "/home", "/tmp", "/var/log", "/var/lib", "/etc", "/usr/local", NULL
  };

  struct scan_frame
  {
    file_t dir_port;
    char path[MAX_PATH_LEN];
  };

  struct scan_frame stack[MAX_STACK_DEPTH];
  int sp = 0;

  for (int i = 0; safe_roots[i] != NULL; ++i)
    {
      file_t root_port = file_name_lookup (safe_roots[i], O_READ | O_EXEC, 0);
      if (root_port == MACH_PORT_NULL)
        {
          LOG_DEBUG ("Skipping inaccessible root: %s", safe_roots[i]);
          continue;
        }
      snprintf (stack[sp].path, MAX_PATH_LEN, "%s", safe_roots[i]);
      stack[sp].dir_port = root_port;
      sp++;
    }

  while (sp > 0)
    {
      sp--;
      file_t dir_port = stack[sp].dir_port;
      char *path = stack[sp].path;

      char *data;
      mach_msg_type_number_t datacnt;
      int nentries;
      error_t err = dir_readdir (dir_port, &data, &datacnt, -1, -1, 0, &nentries);
      if (err)
        {
          mach_port_deallocate (mach_task_self (), dir_port);
          continue;
        }

      struct dirent *entry = (struct dirent *) data;
      char *end = data + datacnt;

      while ((char *) entry < end)
        {
          if (entry->d_namlen > 0 && strcmp (entry->d_name, ".") != 0 && strcmp (entry->d_name, "..") != 0)
            {
              if (should_skip_directory (entry->d_name))
                {
                  entry = (struct dirent *) ((char *) entry + entry->d_reclen);
                  continue;
                }

              char full_path[MAX_PATH_LEN];
              int written;
              if (strcmp (path, "/") == 0)
                written = snprintf (full_path, sizeof (full_path), "/%.*s",
                                   MAX_PATH_LEN - 2, entry->d_name);
              else
                written = snprintf (full_path, sizeof (full_path), "%s/%.*s", path,
                                   MAX_PATH_LEN - (int) strlen (path) - 2, entry->d_name);

              if (written < 0 || written >= MAX_PATH_LEN)
                {
                  LOG_DEBUG ("Truncated full_path for inode %llu", entry->d_ino);
                  full_path[MAX_PATH_LEN - 1] = '\0';
                }

              inode_graph_node_t *s = get_inode (entry->d_ino);
              if (s)
                {
                  inode_replay_state_t *r = &s->replay;
                  if (r->resolved_path == NULL)
                    r->resolved_path = strdup (full_path);

                  if (r->name[0] == '\0')
                    safe_strncpy (r->name, entry->d_name, MAX_FIELD_LEN);
                }

              if ((entry->d_type == DT_DIR || entry->d_type == DT_UNKNOWN) && sp < MAX_STACK_DEPTH)
                {
                  file_t child_port = file_name_lookup_under (dir_port, entry->d_name, O_READ | O_EXEC, 0);
                  if (child_port != MACH_PORT_NULL)
                    {
                      stack[sp].dir_port = child_port;
                      snprintf (stack[sp].path, MAX_PATH_LEN, "%s", full_path);
                      sp++;
                    }
                }
            }

          entry = (struct dirent *) ((char *) entry + entry->d_reclen);
        }

      vm_deallocate (mach_task_self (), (vm_address_t) data, datacnt);
      mach_port_deallocate (mach_task_self (), dir_port);
    }

  LOG_DEBUG ("Sanity check our FS traversal.");
  for (int i = 0; i < HASH_SIZE; ++i)
    {
      inode_graph_node_t *cur = inode_hash[i];
      while (cur)
        {
          inode_replay_state_t *r = &cur->replay;
          if (!r->resolved_path || r->resolved_path[0] == '\0' || r->name[0] == '\0')
            {
              LOG_DEBUG ("Invalid node ino %u has name '%s' and path '%s'",
                         cur->ino,
                         r->name[0] ? r->name : "<empty>",
                         r->resolved_path ? r->resolved_path : "<null>");
            }
          cur = cur->next;
        }
    }
  LOG_DEBUG ("Sanity check finished.");

  return 0;
}
static error_t
safe_path_lookup (const char *path, int flags, file_t * port)
{
  *port = file_name_lookup (path, flags, 0);
  return (*port == MACH_PORT_NULL) ? ENOENT : 0;
}
