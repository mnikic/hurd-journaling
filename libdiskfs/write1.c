/* Structure to hold reusable credentials */
struct mkdir_context {
  struct protid *cred;
  struct peropen *po;
};

/* Initialize credentials once */
static error_t
mkdir_context_init (struct node *root, struct mkdir_context *ctx)
{
  error_t err;
  
  ctx->cred = NULL;
  ctx->po = NULL;
  
  err = diskfs_make_peropen (root, O_WRITE, 0, &ctx->po);
  if (err)
    {
      fprintf(stderr, "[ERROR] make_peropen failed: %d\n", err);
      return err;
    }
  
  err = diskfs_create_protid (ctx->po, 0, &ctx->cred);
  if (err)
    {
      fprintf(stderr, "[ERROR] create_protid failed: %d\n", err);
      ports_port_deref (ctx->po);
      ctx->po = NULL;
      return err;
    }
  
  return 0;
}

/* Clean up credentials */
static void
mkdir_context_cleanup (struct mkdir_context *ctx)
{
  if (ctx->cred)
    {
      ports_port_deref (ctx->cred);
      ctx->cred = NULL;
    }
  if (ctx->po)
    {
      ports_port_deref (ctx->po);
      ctx->po = NULL;
    }
}

/* Create a single directory with fresh credentials */
static error_t
make_dir_safe (struct node *parent, const char *dirname)
{
  error_t err;
  struct node *dir_node = NULL;
  struct protid *cred = NULL;
  struct peropen *po = NULL;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  
  /* Create fresh credentials for this operation */
  err = diskfs_make_peropen (parent, O_WRITE, 0, &po);
  if (err)
    {
      fprintf(stderr, "[ERROR] make_peropen failed: %d\n", err);
      return err;
    }
  
  err = diskfs_create_protid (po, 0, &cred);
  if (err)
    {
      fprintf(stderr, "[ERROR] create_protid failed: %d\n", err);
      ports_port_deref (po);
      return err;
    }
  
  pthread_mutex_lock (&parent->lock);
  
  /* Follow the same pattern as diskfs_S_dir_mkdir */
  err = diskfs_lookup (parent, dirname, CREATE, 0, ds, cred);
  if (err == EAGAIN)
    err = EEXIST;
  if (!err)
    err = EEXIST;
  
  if (err != ENOENT)
    {
      if (err == EEXIST)
        fprintf (stderr, "[DEBUG] Directory '%s' already exists\n", dirname);
      else
        fprintf (stderr, "[ERROR] lookup(CREATE) failed for '%s': %d\n", dirname, err);
      
      /* Unlock first, then clean up dirstat */
      pthread_mutex_unlock (&parent->lock);
      diskfs_drop_dirstat (parent, ds);
      ports_port_deref (cred);
      ports_port_deref (po);
      return (err == EEXIST) ? 0 : err;  /* Treat EEXIST as success */
    }
  
  fprintf (stderr, "[DEBUG] Creating directory '%s'...\n", dirname);
  mode_t mode = (S_IFDIR | 0755) & ~(S_ISPARE | S_IFMT | S_ITRANS);
  mode |= S_IFDIR;
  
  err = diskfs_create_node (parent, dirname, mode, &dir_node, cred, ds);
  if (err)
    {
      fprintf (stderr, "[ERROR] create_node failed for '%s': %d\n", dirname, err);
      diskfs_drop_dirstat (parent, ds);
      pthread_mutex_unlock (&parent->lock);
      ports_port_deref (cred);
      ports_port_deref (po);
      return err;
    }
  
  fprintf (stderr, "[DEBUG] Directory '%s' created successfully\n", dirname);
  diskfs_node_update (dir_node, 1);
  pthread_mutex_unlock (&dir_node->lock);
  diskfs_nput (dir_node);
  diskfs_drop_dirstat (parent, ds);
  pthread_mutex_unlock (&parent->lock);
  
  /* Clean up credentials */
  ports_port_deref (cred);
  ports_port_deref (po);
  
  return 0;
}

/* Lookup a directory node safely */
static error_t
lookup_dir_safe (struct node *parent, const char *dirname, struct node **result)
{
  error_t err;
  struct protid *cred = NULL;
  struct peropen *po = NULL;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  struct node *found_node = NULL;
  
  *result = NULL;
  
  /* Create fresh credentials for this operation */
  err = diskfs_make_peropen (parent, O_READ, 0, &po);
  if (err)
    {
      fprintf(stderr, "[ERROR] make_peropen failed for lookup: %d\n", err);
      return err;
    }
  
  err = diskfs_create_protid (po, 0, &cred);
  if (err)
    {
      fprintf(stderr, "[ERROR] create_protid failed for lookup: %d\n", err);
      ports_port_deref (po);
      return err;
    }
  
  pthread_mutex_lock (&parent->lock);
  err = diskfs_lookup (parent, dirname, LOOKUP, 0, ds, cred);
  if (err)
    {
      fprintf (stderr, "[DEBUG] Directory '%s' not found: %d\n", dirname, err);
      diskfs_drop_dirstat (parent, ds);
      pthread_mutex_unlock (&parent->lock);
      ports_port_deref (cred);
      ports_port_deref (po);
      return err;
    }
  
  /* Get the node and add a reference */
  found_node = ds->here;
  if (!found_node)
    {
      fprintf (stderr, "[ERROR] No node returned for directory '%s'\n", dirname);
      diskfs_drop_dirstat (parent, ds);
      pthread_mutex_unlock (&parent->lock);
      ports_port_deref (cred);
      ports_port_deref (po);
      return ENOENT;
    }
  
  diskfs_nref (found_node);
  *result = found_node;
  
  /* Clean up */
  diskfs_drop_dirstat (parent, ds);
  pthread_mutex_unlock (&parent->lock);
  ports_port_deref (cred);
  ports_port_deref (po);
  
  return 0;
}

/* Create a full directory path like /home/user/pictures - SAFE VERSION */
static error_t
make_dir_path (struct node *root, const char *path)
{
  error_t err;
  char *path_copy = NULL;
  char *start, *end;
  struct node *current_node = root;
  struct node *next_node = NULL;
  char dirname[256];  /* Buffer for individual directory names */
  
  fprintf (stderr, "[DEBUG] Creating directory path: %s\n", path);
  
  /* Make a copy of the path for parsing */
  path_copy = strdup (path);
  if (!path_copy)
    {
      err = ENOMEM;
      goto cleanup;
    }
  
  /* Skip leading slash if present */
  start = path_copy;
  if (start[0] == '/')
    start++;
  
  /* Parse each directory component manually */
  while (*start != '\0')
    {
      /* Find the end of the current component */
      end = start;
      while (*end != '\0' && *end != '/')
        end++;
      
      /* Extract the directory name */
      size_t len = end - start;
      if (len == 0)
        {
          /* Skip empty components (double slashes) */
          if (*end == '/')
            start = end + 1;
          else
            start = end;
          continue;
        }
      
      if (len >= sizeof(dirname))
        {
          fprintf (stderr, "[ERROR] Directory name too long: %.*s\n", (int)len, start);
          err = ENAMETOOLONG;
          goto cleanup;
        }
      
      /* Copy the directory name */
      memcpy (dirname, start, len);
      dirname[len] = '\0';
      
      /* Create the directory in the current node - fresh credentials each time */
      err = make_dir_safe (current_node, dirname);
      if (err)
        {
          fprintf (stderr, "[ERROR] Failed to create directory '%s': %d\n", dirname, err);
          goto cleanup;
        }
      
      /* Look up the directory we just created - fresh credentials */
      err = lookup_dir_safe (current_node, dirname, &next_node);
      if (err)
        {
          fprintf (stderr, "[ERROR] Failed to lookup created directory '%s': %d\n", dirname, err);
          goto cleanup;
        }
      
      /* Move to the next directory level */
      if (current_node != root)
        diskfs_nput (current_node);  /* Release reference to previous node */
      
      current_node = next_node;
      next_node = NULL;
      
      /* Move to the next component */
      if (*end == '/')
        start = end + 1;
      else
        start = end;
    }
  
  fprintf (stderr, "[DEBUG] Successfully created directory path: %s\n", path);
  
cleanup:
  if (next_node)
    diskfs_nput (next_node);
  if (current_node != root)
    diskfs_nput (current_node);
  if (path_copy)
    free (path_copy);
  return err;
}

/* Simple wrapper for single directory creation - SAFE VERSION */
static void
make_dir (struct node *root, const char *dirname)
{
  error_t err;
  
  fprintf (stderr, "[DEBUG] test_early_boot_write: start\n");
  
  err = make_dir_safe (root, dirname);
  if (err)
    fprintf (stderr, "[ERROR] Failed to create directory: %d\n", err);
  else
    fprintf (stderr, "[DEBUG] test_early_boot_write: success\n");
}
