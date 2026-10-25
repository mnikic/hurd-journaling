/*
   Copyright (C) 1994,95,96,97,98,99,2001,2003 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#include <error.h>
#include "diskfs.h"
#include "priv.h"


/* Check if source directory is in the path of the target directory.
   We get target locked, source unlocked but with a reference.  When
   we return, nothing is locked, and target has lost its reference.
   This routine assumes that no renames of directories will happen
   while it is running; as a result, ufs_rename serializes all renames
   of directories.  */
static error_t
checkpath(struct node *source,
	  struct node *target,
	  struct protid *cred)
{
  error_t err;
  struct node *np, *newnp;

  for (newnp = target, err = 0;
       /* nothing */;
       /* This special lookup does a diskfs_nput on its first argument
	  when it succeeds. */
       err = diskfs_lookup (np, "..", LOOKUP | SPEC_DOTDOT, &newnp, 0, cred))
    {
      if (err)
	{
	  diskfs_nput (np);
	  return err;
	}
      np = newnp;

      if (np == source)
	{
	  diskfs_nput (np);
	  return EINVAL;
	}

      if (np == diskfs_root_node || np == cred->po->shadow_root)
	{
	  diskfs_nput (np);
	  return 0;
	}
    }
}

/* Set *CHAIN to the inode numbers of DP and the directories above it, up
   to the root, and *N to how many there are.  Called with nothing
   locked.  Directory renames are serialized, so the chain stays as it is
   while the caller runs.  */
static error_t
ancestors (struct node *dp, struct protid *cred, ino_t **chain, size_t *n)
{
  error_t err;
  struct node *np, *newnp;
  size_t alloc = 16;

  *n = 0;
  *chain = malloc (alloc * sizeof **chain);
  if (!*chain)
    return ENOMEM;

  pthread_mutex_lock (&dp->lock);
  diskfs_nref (dp);
  for (newnp = dp, err = 0;
       /* nothing */;
       /* This special lookup does a diskfs_nput on its first argument
	  when it succeeds. */
       err = diskfs_lookup (np, "..", LOOKUP | SPEC_DOTDOT, &newnp, 0, cred))
    {
      if (err)
	{
	  diskfs_nput (np);
	  free (*chain);
	  *chain = NULL;
	  return err;
	}
      np = newnp;

      if (*n == alloc)
	{
	  ino_t *more = realloc (*chain, (alloc *= 2) * sizeof **chain);
	  if (!more)
	    {
	      diskfs_nput (np);
	      free (*chain);
	      *chain = NULL;
	      return ENOMEM;
	    }
	  *chain = more;
	}
      (*chain)[(*n)++] = np->cache_id;

      if (np == diskfs_root_node || np == cred->po->shadow_root)
	{
	  diskfs_nput (np);
	  return 0;
	}
    }
}

static int
in_chain (const ino_t *chain, size_t n, struct node *np)
{
  for (size_t i = 0; i < n; i++)
    if (chain[i] == np->cache_id)
      return 1;
  return 0;
}

/* Point the ".." of FNP back from TDP to FDP, and give FDP back the link
   it had: undo step 2 of diskfs_rename_dir.  All three are locked.  */
static error_t
undo_dotdot (struct node *fnp, struct node *fdp, struct node *tdp,
	     struct protid *cred)
{
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  struct node *np;
  error_t err;

  err = diskfs_lookup (fnp, "..", RENAME | SPEC_DOTDOT, &np, ds, cred);
  if (err)
    {
      diskfs_drop_dirstat (fnp, ds);
      return err;
    }
  assert_backtrace (np == tdp);

  err = diskfs_dirrewrite (fnp, tdp, fdp, "..", ds);
  diskfs_file_update (fnp, diskfs_synchronous);
  if (err)
    return err;

  fdp->dn_stat.st_nlink++;
  fdp->dn_set_ctime = 1;
  diskfs_node_update (fdp, diskfs_synchronous);
  assert_backtrace (tdp->dn_stat.st_nlink > 1);
  tdp->dn_stat.st_nlink--;
  tdp->dn_set_ctime = 1;
  diskfs_node_update (tdp, diskfs_synchronous);
  return 0;
}

/* Rename directory node FNP (whose parent is FDP, and which has name
   FROMNAME in that directory) to have name TONAME inside directory
   TDP.  None of these nodes are locked, and none should be locked
   upon return.  This routine is serialized, so it doesn't have to be
   reentrant.  Directories will never be renamed except by this
   routine.  FROMCRED and TOCRED are the users responsible for
   FDP/FNP and TDP respectively.  If EXCL is set, then fail if TONAME
   already exists inside directory TDP. */
error_t
diskfs_rename_dir (struct node *fdp, struct node *fnp, const char *fromname,
		   struct node *tdp, const char *toname,
		   struct protid *fromcred, struct protid *tocred, int excl)
{
  error_t err;
  struct node *tnp, *tmpnp;
  void *buf = alloca (diskfs_dirstat_size);
  struct dirstat *ds;
  struct dirstat *tmpds;
  diskfs_transaction_t *txn;

  pthread_mutex_lock (&tdp->lock);
  diskfs_nref (tdp);		/* reference and lock will get consumed by
				   checkpath */
  err = checkpath (fnp, tdp, tocred);

  if (err)
    return err;

  /* Lookups lock a directory before those below it, so the parents are
     locked in that order too: TDP is not below FNP (checkpath), but it can
     be above FDP, and so can the target.  */
  ino_t *chain;
  size_t nchain;
  err = ancestors (fdp, fromcred, &chain, &nchain);
  if (err)
    return err;
  int tdp_above = fdp != tdp && in_chain (chain, nchain, tdp);

  txn = diskfs_journal_start_transaction ();
  ds = buf;
  if (tdp_above)
    {
      /* TDP first, then the target, below it, then FDP.  That is only
	 the lookups' order when the target is a directory in another
	 branch than FDP; otherwise give up before locking FDP.  A target
	 that is FDP or above it holds FNP somewhere below, so it is not
	 empty.  One that is not a directory can be a file linked in FDP
	 too, which a lookup in FDP locks after FDP.  */
      pthread_mutex_lock (&tdp->lock);
      err = diskfs_lookup (tdp, toname, RENAME, &tnp, ds, tocred);
      if (!err && tnp && (in_chain (chain, nchain, tnp)
			  || !S_ISDIR (tnp->dn_stat.st_mode)))
	{
	  error_t why = (excl ? EEXIST
			 : S_ISDIR (tnp->dn_stat.st_mode) ? ENOTEMPTY : ENOTDIR);
	  diskfs_drop_dirstat (tdp, ds);
	  diskfs_nput (tnp);
	  pthread_mutex_unlock (&tdp->lock);
	  free (chain);
	  diskfs_journal_stop_transaction (txn);
	  return why;
	}
      pthread_mutex_lock (&fdp->lock);
    }
  else
    {
      /* FDP is above TDP, or neither is above the other.  */
      pthread_mutex_lock (&fdp->lock);
      if (fdp != tdp)
	pthread_mutex_lock (&tdp->lock);

      /* 1: Lookup target; if it exists, make sure it's an empty
	 directory. */
      err = diskfs_lookup (tdp, toname, RENAME, &tnp, ds, tocred);
    }
  free (chain);
  assert_backtrace (err != EAGAIN);	/* <-> assert_backtrace (TONAME != "..") */
  /* FNP is not locked yet: OUT must not unlock it.  */
  if (err && err != ENOENT)
    {
      fnp = NULL;
      goto out;
    }
  /* TDP was removed (see diskfs_direnter): fail before ".." changes.  */
  if (tdp->dn_stat.st_nlink == 0)
    {
      err = ENOENT;
      fnp = NULL;
      goto out;
    }
  if (tnp && excl)
    {
      err = EEXIST;
      fnp = NULL;
      goto out;
    }

  if (tnp == fnp)
    {
      diskfs_drop_dirstat (tdp, ds);
      diskfs_nput (tnp);
      pthread_mutex_unlock (&tdp->lock);
      if (fdp != tdp)
	pthread_mutex_unlock (&fdp->lock);
      diskfs_journal_stop_transaction (txn);
      return 0;
    }

  /* A target that is not a directory fails, before FNP is locked: it can
     be a file linked in FNP as well, locked here before FNP, while a
     lookup in FNP locks FNP and then the file.  */
  if (tnp && !S_ISDIR (tnp->dn_stat.st_mode))
    {
      err = ENOTDIR;
      fnp = NULL;
      goto out;
    }

  /* Check permissions to remove FROMNAME and lock FNP.  */
  tmpds = alloca (diskfs_dirstat_size);
  err = diskfs_lookup (fdp, fromname, REMOVE, &tmpnp, tmpds, fromcred);
  assert_backtrace (!tmpnp || tmpnp == fnp);
  if (tmpnp)
    diskfs_nrele (tmpnp);
  diskfs_drop_dirstat (fdp, tmpds);
  if (err)
    {
      assert_backtrace (!tmpnp);
      /* diskfs_lookup has not locked fnp then, do not unlock it. */
      fnp = NULL;
      goto out;
    }

  if (tnp)
    {
      if (! S_ISDIR(tnp->dn_stat.st_mode))
	err = ENOTDIR;
      else if (!diskfs_dirempty (tnp, tocred))
	err = ENOTEMPTY;
    }

  if (err && err != ENOENT)
    goto out;

  /* The link counts to raise are checked before anything changes.  */
  if ((fdp != tdp && tdp->dn_stat.st_nlink == diskfs_link_max - 1)
      || fnp->dn_stat.st_nlink == diskfs_link_max - 1)
    {
      err = EMLINK;
      goto out;
    }

  /* 2: Set our .. to point to the new parent */
  if (fdp != tdp)
    {
      tdp->dn_stat.st_nlink++;
      tdp->dn_set_ctime = 1;
      diskfs_node_update (tdp, diskfs_synchronous);

      tmpds = alloca (diskfs_dirstat_size);
      err = diskfs_lookup (fnp, "..", RENAME | SPEC_DOTDOT,
			   &tmpnp, tmpds, fromcred);
      assert_backtrace (err != ENOENT);
      if (err)
	{
	  assert_backtrace (tdp->dn_stat.st_nlink > 0);
	  tdp->dn_stat.st_nlink--;
	  if (tdp->dn_stat.st_nlink == 0)
	    diskfs_orphan_add (tdp);
	  tdp->dn_set_ctime = 1;
          diskfs_node_update (tdp, diskfs_synchronous);
	  diskfs_drop_dirstat (fnp, tmpds);
	  goto out;
	}
      assert_backtrace (tmpnp == fdp);

      err = diskfs_dirrewrite (fnp, fdp, tdp, "..", tmpds);
      diskfs_file_update (fnp, diskfs_synchronous);
      if (err)
	{
	  assert_backtrace (tdp->dn_stat.st_nlink > 0);
	  tdp->dn_stat.st_nlink--;
	  if (tdp->dn_stat.st_nlink == 0)
	    diskfs_orphan_add (tdp);
	  tdp->dn_set_ctime = 1;
          diskfs_node_update (tdp, diskfs_synchronous);

	  goto out;
	}

      fdp->dn_stat.st_nlink--;
      if (fdp->dn_stat.st_nlink == 0)
	diskfs_orphan_add (fdp);

      fdp->dn_set_ctime = 1;
      diskfs_node_update (fdp, diskfs_synchronous);
    }


  /* 3: Increment the link count on the node being moved and rewrite
     tdp. */
  fnp->dn_stat.st_nlink++;
  fnp->dn_set_ctime = 1;
  diskfs_node_update (fnp, diskfs_synchronous);

  if (tnp)
    {
      err = diskfs_dirrewrite (tdp, tnp, fnp, toname, ds);
      ds = 0;
      if (!err)
	{
	  tnp->dn_stat.st_nlink--;
	  if (tnp->dn_stat.st_nlink == 0)
	    diskfs_orphan_add (tnp);
	  tnp->dn_set_ctime = 1;
	  diskfs_clear_directory (tnp, tdp, tocred);
	  diskfs_file_update (tnp, diskfs_synchronous);
	}
    }
  else
    {
      /* diskfs_direnter consumes DS, whether it succeeds or not.  */
      err = diskfs_direnter (tdp, toname, fnp, ds, tocred);
      ds = 0;
      diskfs_file_update (tdp, diskfs_synchronous);
    }

  if (err)
    {
      assert_backtrace (fnp->dn_stat.st_nlink > 0);
      fnp->dn_stat.st_nlink--;
      if (fnp->dn_stat.st_nlink == 0)
	diskfs_orphan_add (fnp);
      fnp->dn_set_ctime = 1;
      /* fnp is locked, so this is safe */
      diskfs_node_update (fnp, diskfs_synchronous);

      /* FNP stays in FDP: undo step 2, so that its ".." points there
	 again.  Entering the name can fail for want of a block (ENOSPC);
	 a rewrite needs none.  */
      if (fdp != tdp)
	{
	  error_t undo = undo_dotdot (fnp, fdp, tdp, fromcred);
	  if (undo)
	    error (0, undo, "rename: cannot point \"..\" of directory "
		   "%llu back", (unsigned long long) fnp->cache_id);
	}
      goto out;
    }

  /* 4: Remove the entry in fdp. */
  ds = buf;
  pthread_mutex_unlock (&fnp->lock);
  err = diskfs_lookup (fdp, fromname, REMOVE, &tmpnp, ds, fromcred);
  assert_backtrace (!tmpnp || tmpnp == fnp);
  if (tmpnp)
    diskfs_nrele (tmpnp);
  if (err)
    {
      assert_backtrace (!tmpnp);
      /* diskfs_lookup has not locked fnp then, do not unlock it. */
      fnp = NULL;
      goto out;
    }

  diskfs_dirremove (fdp, fnp, fromname, ds);
  ds = 0;
  fnp->dn_stat.st_nlink--;
  if (fnp->dn_stat.st_nlink == 0)
    diskfs_orphan_add (fnp);
  fnp->dn_set_ctime = 1;
  diskfs_file_update (fdp, diskfs_synchronous);
  diskfs_node_update (fnp, diskfs_synchronous);

 out:
  if (tdp)
    pthread_mutex_unlock (&tdp->lock);
  if (tnp)
    diskfs_nput (tnp);
  if (fdp && fdp != tdp)
    pthread_mutex_unlock (&fdp->lock);
  if (fnp)
    pthread_mutex_unlock (&fnp->lock);
  if (ds)
    diskfs_drop_dirstat (tdp, ds);

  /* FINALIZE TRANSACTION */
  diskfs_journal_end_transaction (txn, !err, diskfs_synchronous);
  return err;
}
