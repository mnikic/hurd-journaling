#include <libdiskfs/diskfs.h>
#include <hurd/fshelp.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#define MAX_PATH_LEN  1024
/**
 * Create reusable diskfs protid credentials for a given node.
 * Caller must ports_port_deref(cred) when done.
 */
static error_t
diskfs_create_creds (struct node *np, int flags, struct protid **out_cred)
{
  error_t err;
  struct peropen *po = NULL;

  err = diskfs_make_peropen (np, flags, 0, &po);
  if (err)
    return err;

  err = diskfs_create_protid (po, 0, out_cred);
  if (err)
    {
      ports_port_deref (po);
      return err;
    }

  return 0;
}

static void
make_dir (struct node *root, const char *dirname, struct protid *cred)
{
  error_t err;
  struct node *new_node = NULL;
  struct dirstat *ds = alloca (diskfs_dirstat_size);

  fprintf (stderr, "[DEBUG] make_dir: start\n");
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
  fprintf (stderr, "[DEBUG] make_dir: success\n");
}

static struct node *
mkdir_p (struct node *root, const char *path, struct protid *cred)
{

  error_t err;

  fprintf (stderr, "[DEBUG] makedir_p: start\n");

  char path_copy[MAX_PATH_LEN];
  strncpy (path_copy, path, MAX_PATH_LEN);

  path_copy[MAX_PATH_LEN - 1] = '\0';

  char *token = strtok (path_copy, "/");
  struct node *prev_node = NULL;
  while (token != NULL)
    {

      fprintf (stderr, "[DEBUG] Token: %s\n", token);
      make_dir (root, token, cred);

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

      fprintf (stderr, "[DEBUG] Found a node. Gonna try unlock.  %s\n",
	       token);
      pthread_mutex_unlock (&next_node->lock);
      fprintf (stderr, "[DEBUG] Unlocked it.  %s\n", token);
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
  struct protid *cred = NULL;
  struct node *root = diskfs_root_node;
  diskfs_nref (root);
  error_t err = diskfs_create_creds (root, O_READ | O_EXEC | O_WRITE, &cred);
  if (err)
    {
      fprintf (stderr, "[ERROR] make_peropen failed: %d\n", err);
      return;
    }
  mkdir_p (root, "hey/there/me/too", cred);

  diskfs_nput (root);
  ports_port_deref (cred);
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
