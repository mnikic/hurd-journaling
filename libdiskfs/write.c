#include <libdiskfs/diskfs.h>
#include <hurd/fshelp.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#define MAX_PATH_LEN  1024
static void
make_dir_safe (struct node *root, const char *dirname)
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

  // STEP 1: Try LOOKUP to see if it already exists
  err = diskfs_lookup (root, dirname, LOOKUP, &new_node, NULL, cred);
  if (err == 0 && new_node != NULL)
    {
      fprintf (stderr, "[DEBUG] Directory already exists\n");
      pthread_mutex_unlock (&new_node->lock);
      fprintf (stderr, "[DEBUG] Unlocked new node %llu\n",
	       new_node->dn_stat.st_ino);
      diskfs_nput (new_node);
      fprintf (stderr, "[DEBUG] After nput %llu\n", new_node->dn_stat.st_ino);
      pthread_mutex_unlock (&root->lock);
      fprintf (stderr, "[DEBUG] After unlick root %llu\n",
	       root->dn_stat.st_ino);
      goto cleanup;
    }
  else if (err != ENOENT)
    {
      fprintf (stderr, "[ERROR] lookup(LOOKUP) failed: %d\n", err);
      pthread_mutex_unlock (&root->lock);
      goto cleanup;
    }

  // STEP 2: Use CREATE with dirstat to set up for create_node
  err = diskfs_lookup (root, dirname, CREATE, NULL, ds, cred);
  if (err)
    {
      fprintf (stderr, "[ERROR] lookup(CREATE) failed: %d\n", err);
      pthread_mutex_unlock (&root->lock);
      diskfs_drop_dirstat (root, ds);
      goto cleanup;
    }

  // STEP 3: Actually create the node
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
  if (cred)
    {
      ports_port_deref (cred);
      fprintf (stderr, "[DEBUG] cleaned up cred.\n");
    }
  fprintf (stderr, "[DEBUG] make_dir: success\n");
}

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
  fprintf (stderr, " Entered test \n");

  struct node *root = diskfs_root_node;
  diskfs_nref (root);

  make_dir (root, "hey");
  make_dir (root, "hey");
  make_dir (root, "hey2");
  make_dir (root, "hey2");
  make_dir (root, "hey3");
  make_dir (root, "hey");
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
