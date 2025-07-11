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
    "/home",
    "/tmp",
    "/var/log",
    "/var/lib",
    "/etc",
    "/usr/local",
    NULL
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
      error_t err =
	dir_readdir (dir_port, &data, &datacnt, -1, -1, 0, &nentries);
      if (err)
	{
	  mach_port_deallocate (mach_task_self (), dir_port);
	  continue;
	}

      struct dirent *entry = (struct dirent *) data;
      char *end = data + datacnt;

      while ((char *) entry < end)
	{
	  if (entry->d_namlen > 0 &&
	      strcmp (entry->d_name, ".") != 0 &&
	      strcmp (entry->d_name, "..") != 0)
	    {

	      if (should_skip_directory (entry->d_name))
		{
		  entry =
		    (struct dirent *) ((char *) entry + entry->d_reclen);
		  continue;
		}

	      char full_path[MAX_PATH_LEN];
	      int written;
	      if (strcmp (path, "/") == 0)
		written =
		  snprintf (full_path, sizeof (full_path), "/%.*s",
			    MAX_PATH_LEN - 2, entry->d_name);
	      else
		written =
		  snprintf (full_path, sizeof (full_path), "%s/%.*s", path,
			    MAX_PATH_LEN - (int) strlen (path) - 2,
			    entry->d_name);
	      if (written < 0 || written >= MAX_PATH_LEN)
		{
		  LOG_DEBUG ("Truncated full_path for inode %llu",
			     entry->d_ino);
		  full_path[MAX_PATH_LEN - 1] = '\0';
		}

	      inode_state_t *s = get_inode (entry->d_ino);
	      if (s)
		{
		  if (s->resolved_path == NULL)
		    s->resolved_path = strdup (full_path);

		  if (s->name[0] == '\0')
		    strncpy (s->name, entry->d_name, MAX_FIELD_LEN - 1);
		}

	      if ((entry->d_type == DT_DIR || entry->d_type == DT_UNKNOWN)
		  && sp < MAX_STACK_DEPTH)
		{
		  file_t child_port =
		    file_name_lookup_under (dir_port, entry->d_name,
					    O_READ | O_EXEC, 0);
		  if (child_port != MACH_PORT_NULL)
		    {
		      stack[sp].dir_port = child_port;
		      snprintf (stack[sp].path, MAX_PATH_LEN, "%s",
				full_path);
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
      inode_state_t *cur = inode_hash[i];
      while (cur)
	{
	  if (!cur->resolved_path || cur->resolved_path[0] == '\0' ||
	      cur->name[0] == '\0')
	    {
	      LOG_DEBUG ("Invalid node ino %u has name '%s' and path '%s'",
			 cur->ino, cur->name[0] ? cur->name : "<empty>",
			 cur->resolved_path ? cur->resolved_path : "<null>");
	    }
	  inode_state_t *next = cur->next;
	  cur = next;
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

