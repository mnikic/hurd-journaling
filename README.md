This is the GNU Hurd, <http://www.gnu.org/software/hurd/>. Welcome.

# GNU Hurd: JBD2 Write-Ahead Log (ext3/4)
**Status:** Merged into mainline GNU Hurd `ext2fs`
[View Official Upstream Commit on Savannah](https://cgit.git.savannah.gnu.org/cgit/hurd/hurd.git/commit/?id=bc8ac23d670ad3aeaddf693d30cdc2abbf9952af)

## TL;DR
This repository contains the architecture and implementation of a crash-consistent Write-Ahead Log (WAL) for the GNU Hurd microkernel. It is binary compatible with the Linux JBD2 standard, enabling cross-OS disk recovery using standard `ext3`/`ext4` tools. 

Fun fact: Linux tools now actually report Hurd's Ext2 partitions as Ext3 upon mounting.

This is actually my second attempt at adding a journal to Hurd's Ext2. My first attempt was a home-grown, inode-based journal tracking high-level VFS concepts (rmdir, mkdir, etc.). It was fun to build, but it had too many edge cases and architectural disadvantages, so I eventually scrapped it.

This time around, the journaling happens way down at the block level. The journal doesn't know the semantic meaning of an operation; it just knows which physical blocks were altered. Because many blocks are altered together during a logical operation, we use transactions to group them.

Hurd's Ext2 is heavily asynchronous. Disk writes normally only happen on explicit `fsync`s or when a predetermined interval passes (30 seconds by default). This required carefully re-architecting what transactions mean and exactly when they get committed.

But even this JBD2-style implementation went through phases. At first, I would just eagerly `memcpy` the contents of the whole block to the journal's buffer every time a block was altered. But blocks are often mutated multiple times a transaction (for instance, if multiple Hurd nodes live in the same physical block). Eager copying worked, but it was incredibly inefficient (we would end up copying the same block over and over again). I needed a faster way: deferred journaling. Instead of eagerly copying memory, we just record which blocks are dirty, wait, and do the actual memory copy later when its safe.

## The Concurrency Challenge: Torn Writes
Here is where it got really interesting.

Finding when is it safe to do this copy is nuanced.
Unlike Linux, which tightly couples the VFS to the block layer using physical block-level spinlocks, the Hurd architecture relies heavily on its concurrent Mach VM pager. Locking in Hurd is done at the *logical node* level, not the physical block level.

Because multiple logical nodes often share the exact same 4KB physical block, one has to be very careful with deferred journaling. If the journal simply records a dirty block ID and attempts a background memory copy later, an unrelated thread might be halfway through mutating a neighboring node in that exact same block. The result? A torn write and a corrupted WAL.

## The Solution: Using VFS Refcounts for Safe Copies
To get fast, deferred logging without torn writes, I ended up piggybacking on the natural transaction reference counting (`t_updates`) of the VFS threads.

When `t_updates == 0`, we have a strict guarantee:
1. No VFS threads are currently mutating *any* blocks in the transaction.
2. The memory for all involved blocks has fully settled into a safe, untorn state.

This became the safe spot to copy the memory. By deferring all block copying until the exact millisecond `t_updates` hits 0, (and maintaining a lightweight O(1) `needs_copy` hash map during the hot-path to prevent quadratic copying loops because `t_updates == 0` can happen multiple times during one transaction lifecycle), the overhead vanished.

The journaled `ext2fs` now performs almost on par with the unjournaled version, even under heavy loads, while providing benefits.
