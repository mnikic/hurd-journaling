#include <libdiskfs/diskfs.h>
#include <hurd/fshelp.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#define MAX_PATH_LEN  1024

/**
 * Change a node field safely: locks the node, evaluates OPERATION, optionally
 * calls diskfs_node_update if synchronous mode is enabled, and unlocks.
 *
 * Usage:
 *   CHANGE_NODE_FIELD(np, {
 *     np->dn_stat.st_mode = new_mode;
 *     np->dn_set_ctime = 1;
 *   });
 */
#define CHANGE_NODE_FIELD(NODE, OPERATION)               \
  ({                                                     \
    error_t err = 0;                                     \
    if (!(NODE))                                         \
      err = EINVAL;                                      \
    else                                                 \
      {                                                  \
        pthread_mutex_lock(&(NODE)->lock);               \
        (OPERATION);                                     \
        if (diskfs_synchronous)                          \
          diskfs_node_update((NODE), 1);                 \
        pthread_mutex_unlock(&(NODE)->lock);             \
      }                                                  \
    err;                                                 \
  })

/**
 * Convert milliseconds since epoch into struct timespec.
 *
 * @param ms   Milliseconds since epoch.
 * @param out  Pointer to timespec to fill.
 */
static void
timespec_from_ms(uint64_t ms, struct timespec *out)
{
  out->tv_sec = ms / 1000;
  out->tv_nsec = (ms % 1000) * 1000000;
}


/**
 * Internal utimes that updates atime and mtime on a node without RPC or user checks.
 *
 * If tv_nsec == UTIME_NOW, updates are taken from current time.
 * If tv_nsec == UTIME_OMIT, no update is performed.
 * Otherwise, time is set explicitly.
 */
static error_t
utimes_local(struct node *np, struct timespec atime, struct timespec mtime)
{
  return CHANGE_NODE_FIELD(np, {
    diskfs_set_node_times(np);  // Flush pending updates

    if (atime.tv_nsec == UTIME_NOW)
      np->dn_set_atime = 1;
    else if (atime.tv_nsec != UTIME_OMIT)
      {
        np->dn_stat.st_atim = atime;
        np->dn_set_atime = 0;
      }

    if (mtime.tv_nsec == UTIME_NOW)
      np->dn_set_mtime = 1;
    else if (mtime.tv_nsec != UTIME_OMIT)
      {
        np->dn_stat.st_mtim = mtime;
        np->dn_set_mtime = 0;
      }

    np->dn_set_ctime = 1;

    if (np->filemod_reqs)
      diskfs_notice_filechange(np, FILE_CHANGED_META, 0, 0);
  });
}

/**
 * Create reusable diskfs protid credentials for a given node.
 * Caller must ports_port_deref(cred) when done.
 * Flags are passed to diskfs_make_peropen.
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

/**
 * Internal version of unlink that avoids RPCs.
 * Unlinks a regular file named `name` under directory `dir` using `cred`.
 * 
 * Returns 0 on success or error code.
 */
static error_t
unlink_local (struct node *dir, const char *name, struct protid *cred)
{
  error_t err;
  struct node *target = NULL;
  struct dirstat *ds = alloca (diskfs_dirstat_size);

  if (!cred)
    return EOPNOTSUPP;

  pthread_mutex_lock (&dir->lock);

  err = diskfs_lookup (dir, name, REMOVE, &target, ds, cred);
  if (err)
    {
      diskfs_drop_dirstat (dir, ds);
      pthread_mutex_unlock (&dir->lock);
      return err == EAGAIN ? EPERM : err;
    }

  if (S_ISDIR (target->dn_stat.st_mode))
    {
      // Don't unlink directories
      diskfs_nput (target);
      diskfs_drop_dirstat (dir, ds);
      pthread_mutex_unlock (&dir->lock);
      return EPERM;
    }

  err = diskfs_dirremove (dir, target, name, ds);
  if (!err)
    {
      target->dn_stat.st_nlink--;
      target->dn_set_ctime = 1;

      if (diskfs_synchronous)
	{
	  diskfs_file_update (target, 1);
	  diskfs_node_update (dir, 1);
	}
    }

  diskfs_nput (target);
  diskfs_drop_dirstat (dir, ds);
  pthread_mutex_unlock (&dir->lock);

  return err;
}

/**
 * Internal version of rmdir that avoids RPCs, using diskfs_lookup and diskfs_dirremove.
 */
static error_t
rmdir_local (struct node *dir, const char *name, struct protid *cred)
{
  error_t err;
  struct node *target = NULL;
  struct dirstat *ds = alloca (diskfs_dirstat_size);

  pthread_mutex_lock (&dir->lock);

  err = diskfs_lookup (dir, name, REMOVE, &target, ds, cred);
  if (err)
    {
      pthread_mutex_unlock (&dir->lock);
      return err == EAGAIN ? ENOTEMPTY : err;
    }

  if (!S_ISDIR (target->dn_stat.st_mode))
    {
      diskfs_nput (target);
      diskfs_drop_dirstat (dir, ds);
      pthread_mutex_unlock (&dir->lock);
      return ENOTDIR;
    }

  if (!diskfs_dirempty (target, cred))
    {
      diskfs_nput (target);
      diskfs_drop_dirstat (dir, ds);
      pthread_mutex_unlock (&dir->lock);
      return ENOTEMPTY;
    }

  err = diskfs_dirremove (dir, target, name, ds);
  diskfs_drop_dirstat (dir, ds);
  pthread_mutex_unlock (&dir->lock);
  diskfs_nput (target);
  return err;
}

/**
 * Create a file named `filename` under `dir`, using Hurd diskfs APIs.
 *
 * `dir` must be UNLOCKED on entry.
 * If the file already exists, the existing node is returned locked via `*out`.
 * If the file is created, the new node is returned locked via `*out`.
 *
 * Caller must unlock and `diskfs_nput(*out)` after use.
 */
static error_t
make_file (struct node *dir, const char *filename, struct protid *cred,
	   struct node **out)
{
  error_t err;
  struct node *new_node = NULL;
  struct dirstat *ds = alloca (diskfs_dirstat_size);

  fprintf (stderr, "[DEBUG] make_file: start\n");
  pthread_mutex_lock (&dir->lock);

  err = diskfs_lookup (dir, filename, CREATE, &new_node, ds, cred);
  if (err == EAGAIN || err == 0)
    {
      fprintf (stderr, "[DEBUG] File already exists\n");
      *out = new_node;
      diskfs_drop_dirstat (dir, ds);
      pthread_mutex_unlock (&dir->lock);
      return 0;
    }
  else if (err != ENOENT)
    {
      fprintf (stderr, "[ERROR] lookup(CREATE) failed: %d\n", err);
      diskfs_drop_dirstat (dir, ds);
      pthread_mutex_unlock (&dir->lock);
      return err;
    }

  mode_t mode = S_IFREG | 0644;
  err = diskfs_create_node (dir, filename, mode, &new_node, cred, ds);
  if (err)
    {
      fprintf (stderr, "[ERROR] create_node failed: %d\n", err);
      diskfs_drop_dirstat (dir, ds);
      pthread_mutex_unlock (&dir->lock);
      return err;
    }

  fprintf (stderr, "[DEBUG] File '%s' created\n", filename);
  diskfs_node_update (new_node, 1);
  *out = new_node;

  diskfs_drop_dirstat (dir, ds);
  pthread_mutex_unlock (&dir->lock);
  return 0;
}

/**
 * Create a directory named `dirname` under `root`, using Hurd diskfs APIs.
 *
 * `root` must be UNLOCKED on entry.
 * If the directory already exists, the existing node is returned locked via `*out`.
 * If the directory is created, the new node is returned locked via `*out`.
 *
 * Caller must unlock and `diskfs_nput(*out)` after use.
 */
static error_t
make_dir (struct node *root, const char *dirname, struct protid *cred,
	  struct node **out)
{
  error_t err = 0;
  struct node *new_node = NULL;
  struct dirstat *ds = alloca (diskfs_dirstat_size);

  fprintf (stderr, "[DEBUG] make_dir: start\n");
  pthread_mutex_lock (&root->lock);

  err = diskfs_lookup (root, dirname, CREATE, &new_node, ds, cred);
  if (err == EAGAIN || err == 0)
    {
      fprintf (stderr, "[DEBUG] Directory already exists\n");
      *out = new_node;
      err = 0;
      goto cleanup;
    }
  else if (err != ENOENT)
    {
      fprintf (stderr, "[ERROR] lookup(CREATE) failed: %d\n", err);
      goto cleanup;
    }

  mode_t mode = S_IFDIR | 0755;
  err = diskfs_create_node (root, dirname, mode, &new_node, cred, ds);
  if (err)
    {
      fprintf (stderr, "[ERROR] create_node failed: %d\n", err);
      goto cleanup;
    }

  fprintf (stderr, "[DEBUG] Directory '%s' created\n", dirname);
  diskfs_node_update (new_node, 1);
  *out = new_node;

cleanup:
  diskfs_drop_dirstat (root, ds);
  pthread_mutex_unlock (&root->lock);
  return err;
}

/**
 * Recursively create all intermediate directories in a path relative to `root`.
 * Uses Hurd diskfs APIs to create directories one component at a time.
 *
 * Returns a locked node corresponding to the final path component via `*out_node`.
 * Caller must unlock and `diskfs_nput(*out_node)` after use.
 */
static error_t
mkdir_p (struct node *root, const char *path, struct protid *cred,
	 struct node **out_node)
{
  fprintf (stderr, "[DEBUG] mkdir_p: start\n");

  if (strlen (path) >= MAX_PATH_LEN)
    return ENAMETOOLONG;

  char path_copy[MAX_PATH_LEN];
  strncpy (path_copy, path, MAX_PATH_LEN);
  path_copy[MAX_PATH_LEN - 1] = '\0';

  char *token = strtok (path_copy, "/");
  struct node *prev_node = NULL;

  while (token != NULL)
    {
      fprintf (stderr, "[DEBUG] Token: %s\n", token);
      struct node *next_node = NULL;
      error_t err = make_dir (root, token, cred, &next_node);
      if (err)
	{
	  fprintf (stderr,
		   "[ERROR] mkdir_p: make_dir failed on '%s' with err %d\n",
		   token, err);
	  if (prev_node)
	    diskfs_nput (prev_node);
	  return err;
	}

      pthread_mutex_unlock (&next_node->lock);
      if (prev_node)
	diskfs_nput (prev_node);

      prev_node = root;
      root = next_node;
      token = strtok (NULL, "/");
    }

  *out_node = root;
  return 0;
}

/**
 * Internal chmod that directly modifies the mode bits of a node.
 *
 * The node must be locked before calling. No permission checks are performed.
 */
static error_t
chmod_local(struct node *np, mode_t new_mode)
{
  return CHANGE_NODE_FIELD(np, {
    new_mode &= ~(S_IFMT | S_ISPARE | S_ITRANS);
    new_mode |= (np->dn_stat.st_mode & (S_IFMT | S_ISPARE | S_ITRANS));

    err = diskfs_validate_mode_change(np, new_mode);
    if (!err)
      {
        np->dn_stat.st_mode = new_mode;
        np->dn_set_ctime = 1;

        if (np->filemod_reqs)
          diskfs_notice_filechange(np, FILE_CHANGED_META, 0, 0);
      }
  });
}

/**
 * Internal chown that sets UID and/or GID on a node.
 *
 * Pass -1 for uid or gid to leave it unchanged.
 */
static error_t
chown_local(struct node *np, uid_t uid, gid_t gid)
{
  return CHANGE_NODE_FIELD(np, {
    if (uid != (uid_t)-1)
      {
        err = diskfs_validate_owner_change(np, uid);
        if (!err)
          {
            np->dn_stat.st_uid = uid;
            if (np->author_tracks_uid)
              np->dn_stat.st_author = uid;
          }
      }

    if (!err && gid != (gid_t)-1)
      {
        err = diskfs_validate_group_change(np, gid);
        if (!err)
          np->dn_stat.st_gid = gid;
      }

    if (!err)
      {
        np->dn_set_ctime = 1;

        if (np->filemod_reqs)
          diskfs_notice_filechange(np, FILE_CHANGED_META, 0, 0);
      }
  });
}


static void
test (void)
{
  fprintf (stderr, "[INFO] Entered test\n");
  struct protid *cred = NULL;
  struct node *root = diskfs_root_node;
  diskfs_nref (root);

  error_t err = diskfs_create_creds (root, O_READ | O_EXEC | O_WRITE, &cred);
  if (err)
    {
      fprintf (stderr, "[ERROR] create_creds failed: %d\n", err);
      diskfs_nput (root);
      return;
    }

  struct node *dir = NULL;
  err = mkdir_p (root, "hey/testdir", cred, &dir);
  if (!err && dir)
    {
      fprintf (stderr, "[INFO] testdir ino = %llu\n", dir->dn_stat.st_ino);

      struct node *file = NULL;
      err = make_file (dir, "file.txt", cred, &file);
      if (!err && file)
	{
	  fprintf (stderr, "[INFO] file.txt created with ino = %llu\n",
		   file->dn_stat.st_ino);
	  diskfs_nput (file);
	}
      else
	{
	  fprintf (stderr, "[ERROR] Failed to create file.txt\n");
	}

      // Try to create it again
      err = make_file (dir, "file.txt", cred, &file);
      if (!err && file)
	{
	  fprintf (stderr,
		   "[INFO] file.txt already existed with ino = %llu\n",
		   file->dn_stat.st_ino);
	  diskfs_nput (file);
	}

      diskfs_nput (dir);
    }
  else
    {
      fprintf (stderr, "[ERROR] Failed to create testdir\n");
    }

  fprintf (stderr, "Deleting full directory.\n");
  rmdir_local (root, "full", cred);
  fprintf (stderr, "Done deleting.\n");
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
