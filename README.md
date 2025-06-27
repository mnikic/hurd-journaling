# hurd-journaling

# Toy Journaling for GNU Hurd

This is an **experimental journaling layer** for the GNU Hurd filesystem, implemented in user space. It aims to explore how journaling of metadata operations might be integrated into the Hurd stack, primarily inside `libdiskfs` and `ext2fs`.

⚠️ **This is not production code.** It is a work-in-progress meant for research, learning, and design iteration.

## ✨ Overview

- **What it does (for now):** Logs structured metadata about filesystem changes (e.g., file creation, unlinking) into a plain-text journal file.
- **What it doesn't do (yet):** Actual crash recovery, atomic replay, transaction rollback, or data journaling.
- **Where it's hooked:** Key calls like `diskfs_create_node`, `diskfs_S_dir_unlink`, and `diskfs_write_disknode` etc, but this is all subject to change.

## 📂 Journal Format

Each transaction is human readable (for now) and is bounded by `BEGIN TX` and `END TX` lines with a timestamp and unique transaction ID. Logged fields include inode, mode, size, time fields, and other metadata. Example:

```
=== BEGIN TX 42 === [2025-06-27 14:42:33.041]
action: create
name: foo.txt
parent inode: 128
inode: 3042
mode: 0100644
size: 0 bytes
nlink: 1
blocks: 0
mtime: 1722328442
ctime: 1722328442
=== END TX 42 ===
```

## 🛠 Implementation Notes

- Written in C, lives in `libdiskfs/journal.[ch]`
- Logging is buffered and flushed to `/tmp/journal.log`
- Transactions are atomic: we log header + body + footer only if the full transaction fits in the buffer
- Uses millisecond-precision timestamps
- Thread-safe via a dedicated mutex
- Can be toggled/integrated manually by calling `journal_log_metadata(node, &info)` from key places

## 🧩 Current Coverage

| Operation      | Hooked? | Notes                           |
|----------------|---------|---------------------------------|
| File creation  | ✅      | `diskfs_create_node`            |
| File unlinking | ✅      | `diskfs_S_dir_unlink`           |
| Sync to disk   | ✅      | `diskfs_write_disknode`         |
| File rename    | ❌      | TODO                            |
| Directory ops  | ❌      | TODO                            |
| Crash recovery | ❌      | Out of scope for now            |

## 🔬 Purpose

This project is intended to:
- Explore how journaling can be structured in Hurd's user-space filesystem
- Prepare the ground for future real journaling or fs change-tracking layers
- Keep things minimal, understandable, and non-invasive

## 📅 Status

Early prototype. Feedback welcome.

## 📄 License

Same as GNU Hurd: GPL-2.0-or-later


