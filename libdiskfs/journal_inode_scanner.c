#include <hurd.h>
#include <hurd/fs.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <libdiskfs/journal_inode_denylist.h>
#include <libdiskfs/journal_util.h>
#include <libdiskfs/journal_fs_helper.h>
#include <libdiskfs/diskfs.h>
#include <dirent.h>

#define MAX_PATH_LEN 256
#define MAX_STACK_DEPTH 64

typedef struct
{
  struct node *dir_node;
  char path[MAX_PATH_LEN];
} inode_stack_frame_t;

typedef struct
{
  inode_stack_frame_t frames[MAX_STACK_DEPTH];
  int top;
} inode_stack_t;

static inode_stack_t stack;

static void
stack_init (void)
{
  stack.top = 0;
}

static bool
stack_push (struct node *np, const char *path)
{
  if (stack.top >= MAX_STACK_DEPTH)
    return false;

  stack.frames[stack.top].dir_node = np;
  snprintf (stack.frames[stack.top].path, MAX_PATH_LEN, "%s", path);
  stack.frames[stack.top].path[MAX_PATH_LEN - 1] = '\0';
  stack.top++;
  return true;
}

static bool
stack_pop (struct node **np_out, char *path_out)
{
  if (stack.top == 0)
    return false;

  stack.top--;
  *np_out = stack.frames[stack.top].dir_node;
  if (path_out != NULL)
    {
      strncpy (path_out, stack.frames[stack.top].path, MAX_PATH_LEN - 1);
      path_out[MAX_PATH_LEN - 1] = '\0';	// ensure null-termination
    }
  return true;
}

error_t
journal_scan_path_for_inos (const char *root_path,
			    journal_inode_denylist_builder_t * builder)
{
  struct protid *cred = NULL;
  struct node *start_np = NULL;
  error_t err = 0;
  size_t count = 0;
  struct node *root = diskfs_root_node;
  diskfs_nref (root);

  err = diskfs_create_creds (root, O_READ | O_EXEC | O_WRITE, &cred);
  if (err)
    {
      JOURNAL_LOG_ERROR ("create_creds failed: %d", err);
      goto cleanup_root;
    }

  err = diskfs_lookup_path (root_path, cred, &start_np);
  if (err)
    {
      JOURNAL_LOG_DEBUG ("scan_path_for_inos: failed to open '%s'",
			 root_path);
      goto cleanup_creds;
    }

  if (!S_ISDIR (start_np->dn_stat.st_mode))
    {
      JOURNAL_LOG_DEBUG ("scan_path_for_inos: '%s' is not a directory",
			 root_path);
      diskfs_nput (start_np);
      err = ENOTDIR;
      goto cleanup_creds;
    }

  stack_init ();
  if (!stack_push (start_np, root_path))
    {
      JOURNAL_LOG_ERROR ("Failed to initialize traversal stack");
      diskfs_nput (start_np);
      err = ENOMEM;
      goto cleanup_creds;
    }

  while (stack_pop (&start_np, NULL))
    {
      if ((start_np->dn_stat.st_mode & S_IFMT) != S_IFDIR ||
	  start_np->dn_stat.st_size == 0)
	{
	  diskfs_nput (start_np);
	  continue;
	}

      char *data = NULL;
      mach_msg_type_number_t datacnt = 0;
      int nentries = 0;

      err =
	diskfs_get_directs (start_np, 0, -1, &data, &datacnt, 0, &nentries);
      if (err || !data)
	{
	  JOURNAL_LOG_DEBUG ("diskfs_get_directs failed: %s", strerror (err));
	  diskfs_nput (start_np);
	  continue;
	}

      struct dirent *entry = (struct dirent *) data;
      char *end = data + datacnt;

      while ((char *) entry < end)
	{
	  char name[NAME_MAX + 1];
	  strncpy (name, entry->d_name, entry->d_namlen);
	  name[entry->d_namlen] = '\0';

	  if (strcmp (name, ".") == 0 || strcmp (name, "..") == 0)
	    {
	      entry = (struct dirent *) ((char *) entry + entry->d_reclen);
	      continue;
	    }

	  struct node *child_np = NULL;
	  error_t cerr =
	    diskfs_lookup_hard (start_np, name, LOOKUP, &child_np, NULL,
				cred);
	  if (cerr || !child_np)
	    {
	      entry = (struct dirent *) ((char *) entry + entry->d_reclen);
	      continue;
	    }

	  mode_t mode = child_np->dn_stat.st_mode;
	  journal_ino_t ino = (journal_ino_t) child_np->dn_stat.st_ino;

	  journal_inode_denylist_builder_add (builder, ino);
	  JOURNAL_LOG_DEBUG ("denylist: found node %u (%s)", (unsigned) ino,
			     name);
	  count++;

	  if (S_ISDIR (mode))
	    {
	      if (!stack_push (child_np, name))
		{
		  diskfs_nput (child_np);
		  JOURNAL_LOG_DEBUG
		    ("Stack overflow, skipping subdirectory: %s", name);
		}
	      // else: ownership of child_np is now with the stack
	    }
	  else
	    {
	      diskfs_nput (child_np);
	    }

	  entry = (struct dirent *) ((char *) entry + entry->d_reclen);
	}

      vm_deallocate (mach_task_self (), (vm_address_t) data, datacnt);
      diskfs_nput (start_np);
    }

  JOURNAL_LOG_DEBUG ("scan_path_for_inos: complete. Found %u inos.", count);

  struct node *remaining_np = NULL;
  while (stack_pop (&remaining_np, NULL))
    diskfs_nput (remaining_np);

cleanup_creds:
  if (cred)
    ports_port_deref (cred);
cleanup_root:
  diskfs_nput (root);
  return err;
}
