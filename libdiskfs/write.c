#include <libdiskfs/diskfs.h>
#include <hurd/fshelp.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#define MAX_PATH_LEN  1024

static void
make_dir (struct node *root, const char *dirname)
{
  error_t err;
  struct node *dir_node = NULL;
  struct protid *cred = NULL;
  struct peropen *po = NULL;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  fprintf (stderr, "[DEBUG] test_early_boot_write: start\n");

  err = diskfs_make_peropen (root, O_WRITE, 0, &po);
  if (err)
    {
      fprintf (stderr, "[ERROR] make_peropen failed: %d\n", err);
      return;
    }

  err = diskfs_create_protid (po, 0, &cred);
  if (err)
    {
      fprintf (stderr, "[ERROR] create_protid failed: %d\n", err);
      ports_port_deref (po);
      return;
    }

  pthread_mutex_lock (&root->lock);

  /* Follow the same pattern as diskfs_S_dir_mkdir */
  err = diskfs_lookup (root, dirname, CREATE, 0, ds, cred);
  if (err == EAGAIN)
    err = EEXIST;
  if (!err)
    err = EEXIST;

  if (err != ENOENT)
    {
      if (err == EEXIST)
	fprintf (stderr, "[DEBUG] Directory already exists\n");
      else
	fprintf (stderr, "[ERROR] lookup(CREATE) failed: %d\n", err);

      /* Unlock first, then clean up dirstat */
      pthread_mutex_unlock (&root->lock);
      diskfs_drop_dirstat (root, ds);
      ports_port_deref (cred);
      ports_port_deref (po);
      return;
    }

  fprintf (stderr, "[DEBUG] Directory not found, creating...\n");
  mode_t mode = (S_IFDIR | 0755) & ~(S_ISPARE | S_IFMT | S_ITRANS);
  mode |= S_IFDIR;

  err = diskfs_create_node (root, dirname, mode, &dir_node, cred, ds);
  if (err)
    {
      fprintf (stderr, "[ERROR] create_node failed: %d\n", err);
      diskfs_drop_dirstat (root, ds);
      pthread_mutex_unlock (&root->lock);
      ports_port_deref (cred);
      ports_port_deref (po);
      return;
    }

  fprintf (stderr, "[DEBUG] Directory '%s' created\n", dirname);
  diskfs_node_update (dir_node, 1);
  pthread_mutex_unlock (&dir_node->lock);
  diskfs_nput (dir_node);
  diskfs_drop_dirstat (root, ds);
  pthread_mutex_unlock (&root->lock);
  ports_port_deref (cred);
  ports_port_deref (po);
  fprintf (stderr, "[DEBUG] test_early_boot_write: success\n");
}

static struct node *
mkdir_p (struct node *root, const char *path)
{
  char path_copy[MAX_PATH_LEN];
  strncpy (path_copy, path, MAX_PATH_LEN);

  path_copy[MAX_PATH_LEN - 1] = '\0';

  char *token = strtok (path_copy, "/");
  struct node *prev_node = NULL;
  while (token != NULL)
    {
      make_dir (root, token);

      struct node *next_node = NULL;
      error_t err =
	diskfs_lookup (root, token, LOOKUP, &next_node, NULL, NULL);
      if (err || !next_node)
	{
	  fprintf (stderr,
		   "[ERROR] mkdir_p: failed to lookup '%s' after creation\n",
		   token);
	  if (prev_node)
	    diskfs_nput (prev_node);
	  return NULL;
	}

      if (prev_node)
	diskfs_nput (prev_node);
      prev_node = root;
      root = next_node;

      token = strtok (NULL, "/");
    }

  return root;
}

static void
test (void)
{
  struct node *root = diskfs_root_node;
  make_dir (root, "full-taker86");
  make_dir (root, "full-taker85");
  diskfs_nput (root);
}

static void *
delayed_test_thread (void *arg)
{
  sleep (10);			// delay to ensure system is writable and stable
  test ();
  return NULL;
}

void
start_test_thread (void)
{
  static volatile int already_run = 0;
  if (already_run)
    {
      return;
    }
  already_run = 1;
  pthread_t tid;
  int err = pthread_create (&tid, NULL, delayed_test_thread, NULL);
  if (err)
    fprintf (stderr, "[ERROR] Failed to create test thread: %d\n", err);
}

