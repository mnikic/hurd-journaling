/* journal_diskfs_helper.h - Wrappers for selected libdiskfs functions with journaling safety.

   These are simplified or defensive wrappers around lower-level diskfs functions
   used by the journaling subsystem for safe and consistent inode restoration.

   Copyright (C) 2025 Free Software Foundation, Inc.
   Written by Milos Nikic.
*/

#ifndef LIBDISKFS_JOURNAL_DISKFS_HELPER_H
#define LIBDISKFS_JOURNAL_DISKFS_HELPER_H

#include <libdiskfs/diskfs.h>

/* Create protid credentials suitable for use with the given node.
   Flags may control things like O_WRITE. */
error_t
diskfs_create_creds (struct node *np, int flags, struct protid **out_cred);

/* Look up `path` relative to locked `np`. The returned node is locked.
   Caller is responsible for nputting the node. */
error_t
diskfs_lookup_path (const struct node *np, const char *path,
		    struct protid *cred, struct node **out_np);

/* Create a file inside locked directory `dir` named `filename`.
 * the returned node is locked.
   Caller is responsible for nputting the returned node. */
error_t
diskfs_make_file (struct node *dir, const char *filename,
		  struct protid *cred, struct node **out);

/* Recursively create directories along `path`, relative to `root`.

   NOTE: Does NOT return a usable node. For all uses, re-lookup the path
   manually if the node is needed. Mirrors the behavior of diskfs_S_dir_mkdir.
 */
error_t
diskfs_mkdir_p (struct node *root, const char *path, struct protid *cred);

#endif /* LIBDISKFS_JOURNAL_DISKFS_HELPER_H */
