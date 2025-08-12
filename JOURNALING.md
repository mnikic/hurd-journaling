# Metadata Journaling (branch: journal-main)
Scope: ext2fs only. Stores metadata updates in reserved raw space; replays at early boot before fsck.
# Setup: 
see [README](/contrib/journaling/README.md)  and use a [SCRIPT](/contrib/journaling/journal-hint2.sh)  for superblock hint + carve-out.
Stability: metadata-only (atime,mtime, ctime (~with caveats), uid,gid, author, flags, mode); not transactional; do-no-harm replay.
