# Metadata Journaling (ext2fs) — Quick Start

**Scope:** ext2fs only. Stores *metadata* (uid,gid,author,mtime,ctime,atime,mode,flags) updates in a reserved raw region (e.g., 8 MiB) outside the ext2 area, then replays at early boot **before fsck**.  
**Not transactional.** Complements fsck; “do-no-harm” replay.

---

## 0) Safety
- **Work on a copy** of your disk image.
- Ensure the reserved region is **after ext2’s end** and **inside the partition** (no overlap with swap/next partition).

---

## 1) Shrink ext2 by ~8 MiB (configurable)
Find the ext2 partition start in bytes:
```bash
parted -sm debian-hurd.img unit B print
# Example output: 2:1000341504B:...:...:ext2::;
```

-Attach the ext2 partition as a loop device
```bash
sudo losetup -o 1000341504 --show -f debian-hurd.img
# This prints something like /dev/loop0 (use whatever it returns).


sudo tune2fs -l /dev/loop0 | grep 'Block count'


#Example:
#Block count:              1035776
```

Shrink by 8 MiB

8 MiB = 8192 KiB → 8192 / 4 = 2048 ext2 blocks
New block count = 1035776 − 2048 = 1033728
```bash
sudo e2fsck -f /dev/loop0 #(accept everything it asks)
sudo resize2fs /dev/loop0 1033728
```
Replace 1033728 with your calculated value.
Verify
```bash
sudo tune2fs -l /dev/loop0 | grep 'Block count'
The number should be exactly 2048 less than the original.
```
Detach loop device
```bash
sudo losetup -d /dev/loop0
```


---

## 2) Write the journaling hint into the ext2 superblock
The helper script assumes the journal lives in the last ~8 MiB of the same partition.
```bash
# Show current hint fields (magic/start/size):
bash ./contrib/journaling/journal-hint2.sh debian-hurd.img show

# Enable (writes magic + start/size pointing to the last 8 MiB):
bash ./contrib/journaling/journal-hint2.sh debian-hurd.img on

# Disable (clears magic):
bash ./contrib/journaling/journal-hint2.sh debian-hurd.img off
```
Assumption: journal-hint2.sh currently assumes a size of 8 MiB and places the journal exactly at partition end.
If you use a different size/location, adjust the script or values accordingly.


---
## 3) Build, install, reboot

Build Hurd as usual, install, and reboot. Early boot should print journal init/replay lines.
For very chatty logs, set:
```bash
// libdiskfs/journal_util.h
#define JOURNAL_DEBUG 1
```


--
## 4) Verify it’s wired correctly

On boot: look for [JOURNAL] lines before fsck runs.

After a crash test (e.g., create many files then hard reboot), verify restored metadata or files under /restore/<timestamp>/… as applicable.

## Notes

ext3/4 journals are not used; this is independent metadata journaling for ext2fs.

Relatime is respected: the journal records what ext2fs emits; it doesn’t inject extra atime updates.

Contributions and bug reports, comments welcome!
