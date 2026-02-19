/*
   Copyright (C) 1993, 1994 Free Software Foundation

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
   along with the GNU Hurd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.  */

/* Written by Michael I. Bushnell.  */

#include "priv.h"

/* Set on disk fields from NP->dn_stat; update ctime, atime, and mtime
   if necessary.  If WAIT is true, then return only after the physical
   media has been completely updated.  */
void diskfs_node_update (struct node *np, int wait)
{
  ino_t ino = np->dn_stat.st_ino;
  if (ino == 20197 || ino == 262147 || ino == 262148 || ino == 98506 || ino == 715276 || ino == 154465 || ino == 16257 || ino == 24660 || ino == 98139 || ino == 157138 || ino == 20195 || ino == 65661 || ino == 622678 || ino == 393236 || ino == 65538 || ino == 622595 || ino == 2883586)
    {
      fprintf (stderr, "DEBUG_PROBE: Inode %llu update. Flags: [ Atime: %d | Mtime: %d | Ctime: %d ] Mode: (Mode %o)\n",
               (unsigned long long)ino,
               (int)np->dn_set_atime,
               (int)np->dn_set_mtime,
               (int)np->dn_set_ctime,
               np->dn_stat.st_mode);
    }
  diskfs_set_node_times (np);
  if (np->dn_stat_dirty)
    diskfs_write_disknode (np, wait);
}
