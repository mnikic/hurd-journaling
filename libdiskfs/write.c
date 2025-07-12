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
  struct node *new_node = NULL;
  struct protid *cred = NULL;
  struct peropen *po = NULL;
  struct dirstat *ds = alloca (diskfs_dirstat_size);

  fprintf (stderr, "[DEBUG] make_dir: start\n");

  err = diskfs_make_peropen (root, O_READ | O_EXEC | O_WRITE, 0, &po);
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

  err = diskfs_lookup (root, dirname, CREATE, NULL, ds, cred);
  if (err == EAGAIN || err == 0)
    {
      fprintf (stderr, "[DEBUG] Directory already exists\n");
      pthread_mutex_unlock (&root->lock);
      diskfs_drop_dirstat (root, ds);
      goto cleanup;
    }
  else if (err != ENOENT)
    {
      fprintf (stderr, "[ERROR] lookup(CREATE) failed: %d\n", err);
      pthread_mutex_unlock (&root->lock);
      diskfs_drop_dirstat (root, ds);
      goto cleanup;
    }

  mode_t mode = S_IFDIR | 0755;
  err = diskfs_create_node (root, dirname, mode, &new_node, cred, ds);
  if (err)
    {
      fprintf (stderr, "[ERROR] create_node failed: %d\n", err);
      pthread_mutex_unlock (&root->lock);
      diskfs_drop_dirstat (root, ds);
      goto cleanup;
    }

  fprintf (stderr, "[DEBUG] Directory '%s' created\n", dirname);
  diskfs_node_update (new_node, 1);
  pthread_mutex_unlock (&new_node->lock);
  diskfs_nput (new_node);

  pthread_mutex_unlock (&root->lock);
  diskfs_drop_dirstat (root, ds);

cleanup:
  ports_port_deref (cred);
  fprintf (stderr, "[DEBUG] make_dir: success\n");
}

static struct node *
mkdir_p (struct node *root, const char *path)
{

  error_t err;
  struct protid *cred = NULL;
  struct peropen *po = NULL;

  fprintf (stderr, "[DEBUG] makedir_p: start\n");

  err = diskfs_make_peropen (root, O_READ | O_EXEC | O_WRITE, 0, &po);
  if (err)
    {
      fprintf (stderr, "[ERROR] make_peropen failed: %d\n", err);
      return NULL;
    }

  err = diskfs_create_protid (po, 0, &cred);
  if (err)
    {
      fprintf (stderr, "[ERROR] create_protid failed: %d\n", err);
      ports_port_deref (po);
      return NULL;
    }

  char path_copy[MAX_PATH_LEN];
  strncpy (path_copy, path, MAX_PATH_LEN);

  path_copy[MAX_PATH_LEN - 1] = '\0';

  char *token = strtok (path_copy, "/");
  struct node *prev_node = NULL;
  while (token != NULL)
    {

      fprintf (stderr, "[DEBUG] Token: %s\n", token);
      make_dir (root, token);

      struct node *next_node = NULL;
      error_t err = diskfs_lookup (root, token, LOOKUP, &next_node, 0, cred);
      fprintf (stderr, "[DEBUG] After lookup for token  %s\n", token);
      if (err || !next_node)
	{
	  fprintf (stderr,
		   "[ERROR] mkdir_p: failed to lookup '%s' after creation\n",
		   token);
	  if (prev_node)
	    diskfs_nput (prev_node);

	  ports_port_deref (cred);
	  return NULL;
	}

      fprintf (stderr, "[DEBUG] Found a node. Gonna try unlock.  %s\n", token);
      pthread_mutex_unlock (&next_node->lock);
      fprintf (stderr, "[DEBUG] Unlocked it.  %s\n", token);
      if (prev_node)
	diskfs_nput (prev_node);
      prev_node = root;
      root = next_node;

      token = strtok (NULL, "/");
    }

  ports_port_deref (cred);
  return root;
}

static void
test (void)
{
  fprintf (stderr, " Entered test \n");

  struct node *root = diskfs_root_node;
  diskfs_nref (root);

  mkdir_p (root, "hey/there/you/too");

  diskfs_nput (root);
}

static void *
delayed_test_thread (void *arg)
{
  sleep (30);			// delay to ensure system is writable and stable
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
