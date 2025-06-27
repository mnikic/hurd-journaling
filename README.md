# hurd-journaling

# Raw metadata journaling prototype

This is an **experimental journaling layer** for the GNU Hurd filesystem, implemented in user space. It aims to explore how journaling of metadata operations might be integrated into the Hurd stack, primarily inside `libdiskfs` and `ext2fs`.

⚠️ **This is not production code.** It is a work-in-progress meant for research, learning, and design iteration.

## 📄 License

Same as GNU Hurd: GPL-2.0-or-later
