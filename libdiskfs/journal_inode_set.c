#include <libdiskfs/journal_inode_set.h>

static journal_ino_t inodes[JOURNAL_MAX_DENY_INODES];
static int inode_count = 0;

void
inode_set_init (void)
{
  inode_count = 0;
}

void
inode_set_add (journal_ino_t ino)
{
  if (inode_count >= JOURNAL_MAX_DENY_INODES)
    return;

  // Prevent duplicates
  for (int i = 0; i < inode_count; i++)
    if (inodes[i] == ino)
      return;

  inodes[inode_count++] = ino;
}

bool
inode_set_contains (journal_ino_t ino)
{
  for (int i = 0; i < inode_count; i++)
    if (inodes[i] == ino)
      return true;
  return false;
}
