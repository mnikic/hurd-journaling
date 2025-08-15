// hjhint.c — Prototype ext2 journaling hint helper for Hurd journaling
// Build: cc -O2 -Wall -o hjhint hjhint.c
// NOTE: Prototype quality. Primary superblock only. No backup superblocks yet.
//       Use on UNMOUNTED images/partitions; keep backups. You own the risk.

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>

#ifdef __linux__
#include <linux/fs.h>		// BLKGETSIZE64
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
// ------------------------- ext2 constants (minimal) -------------------------

static const off_t EXT2_SB_BYTE_OFFSET = 1024;

// Offsets within superblock (little-endian)
static const off_t OFF_s_inodes_count = 0x00;	// u32
static const off_t OFF_s_blocks_count = 0x04;	// u32
static const off_t OFF_s_first_data_block = 0x14;	// u32
static const off_t OFF_s_log_block_size = 0x18;	// u32
static const off_t OFF_s_magic = 0x38;	// u16 (0xEF53)
static const uint16_t EXT2_SUPER_MAGIC = 0xEF53;

// Hint layout (matches journal-hint2.sh)
static const off_t OFF_hj_start_block = 0x108;	// u32
static const off_t OFF_hj_block_count = 0x10C;	// u32
static const off_t OFF_hj_crc32 = 0x110;	// u32 (proto: 0)
static const off_t OFF_hj_magic = 0x114;	// u32 ('JNLH')
static const uint32_t HJ_MAGIC = 0x484c4e4au;	// 'JNLH' LE

// ------------------------- util -------------------------

static void
die (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  if (errno)
    fprintf (stderr, ": %s", strerror (errno));
  fputc ('\n', stderr);
  exit (1);
}

static bool
is_block_device (const char *p)
{
  struct stat st;
  if (stat (p, &st) != 0)
    return false;
  return S_ISBLK (st.st_mode);
}

static bool
appears_mounted_rw (const char *devpath)
{
  FILE *f = fopen ("/proc/mounts", "r");
  if (!f)
    return false;
  char dev[512], mnt[512], fstype[128], opts[1024];
  bool mounted = false;
  while (fscanf
	 (f, "%511s %511s %127s %1023s %*d %*d\n", dev, mnt, fstype,
	  opts) == 4)
    {
      if (strcmp (dev, devpath) == 0 && strstr (opts, "rw"))
	{
	  mounted = true;
	  break;
	}
    }
  fclose (f);
  return mounted;
}

static uint16_t
le16 (const unsigned char *p)
{
  return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static uint32_t
le32 (const unsigned char *p)
{
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
    ((uint32_t) p[3] << 24);
}

static void
wr_le32 (unsigned char *p, uint32_t v)
{
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

// global base offset added to all pread/pwrite (for images with partitions)
static uint64_t g_base_off = 0;

static uint64_t
get_device_size_bytes (int fd, const char *path)
{
  struct stat st;
  if (fstat (fd, &st) != 0)
    die ("fstat failed on %s", path);
  if (S_ISREG (st.st_mode))
    {
      uint64_t total = (uint64_t) st.st_size;
      return (g_base_off <= total) ? (total - g_base_off) : 0;
    }
#ifdef __linux__
  if (S_ISBLK (st.st_mode))
    {
      uint64_t sz = 0;
      if (ioctl (fd, BLKGETSIZE64, &sz) != 0)
	die ("ioctl(BLKGETSIZE64) failed on %s", path);
      return sz;
    }
#endif
  die ("Unsupported file type for %s (need regular file or block device)",
       path);
  return 0;
}

static void
pread_exact (int fd, void *buf, size_t n, off_t off, const char *what)
{
  ssize_t r = pread (fd, buf, n, (off_t) (g_base_off + (uint64_t) off));
  if (r != (ssize_t) n)
    die ("Failed to read %s (wanted %zu at off %" PRIu64 ", got %zd)", what,
	 n, (uint64_t) off, r);
}

static void
pwrite_exact (int fd, const void *buf, size_t n, off_t off, const char *what)
{
  ssize_t r = pwrite (fd, buf, n, (off_t) (g_base_off + (uint64_t) off));
  if (r != (ssize_t) n)
    die ("Failed to write %s (wanted %zu at off %" PRIu64 ", got %zd)", what,
	 n, (uint64_t) off, r);
}

// Run a command (e.g., e2fsck/resize2fs). Returns exit code.
static int
run_cmd (char *const argv[])
{
  pid_t pid = fork ();
  if (pid < 0)
    die ("fork");
  if (pid == 0)
    {
      execvp (argv[0], argv);
      _exit (127);
    }
  int st = 0;
  if (waitpid (pid, &st, 0) < 0)
    die ("waitpid");
  if (WIFEXITED (st))
    return WEXITSTATUS (st);
  return 128;
}

// popen helper to capture first line of stdout; returns malloc'd string or NULL
static char *
popen_first_line (const char *cmd)
{
  FILE *f = popen (cmd, "r");
  if (!f)
    return NULL;
  char buf[512];
  if (!fgets (buf, sizeof (buf), f))
    {
      pclose (f);
      return NULL;
    }
  pclose (f);
  size_t n = strcspn (buf, "\r\n");
  char *s = (char *) malloc (n + 1);
  if (!s)
    return NULL;
  memcpy (s, buf, n);
  s[n] = 0;
  return s;
}

// Setup loop device for IMAGE at offset; returns malloc'd path (e.g., "/dev/loop3")
static char *
losetup_attach (const char *img, uint64_t offset_bytes)
{
  char cmd[PATH_MAX + 64];
  snprintf (cmd, sizeof (cmd), "losetup -f --show -o %" PRIu64 " '%s'",
	    (uint64_t) offset_bytes, img);
  char *dev = popen_first_line (cmd);
  if (!dev || strncmp (dev, "/dev/loop", 9) != 0)
    {
      free (dev);
      return NULL;
    }
  return dev;
}

static void
losetup_detach (const char *loopdev)
{
  char *const argv[] = { "losetup", "-d", (char *) loopdev, NULL };
  (void) run_cmd (argv);	// best effort
}

// ------------------------- superblock I/O -------------------------

typedef struct
{
  uint32_t inodes_count;
  uint32_t blocks_count;
  uint32_t first_data_block;
  uint32_t log_block_size;
  uint16_t magic;
  // journaling hint fields
  uint32_t hj_start_block;
  uint32_t hj_block_count;
  uint32_t hj_crc32;
  uint32_t hj_magic;
} sb_info_t;

static void
read_sb (int fd, sb_info_t *out)
{
  unsigned char sb[1024];
  pread_exact (fd, sb, sizeof (sb), EXT2_SB_BYTE_OFFSET, "ext2 superblock");

  out->inodes_count = le32 (sb + OFF_s_inodes_count);
  out->blocks_count = le32 (sb + OFF_s_blocks_count);
  out->first_data_block = le32 (sb + OFF_s_first_data_block);
  out->log_block_size = le32 (sb + OFF_s_log_block_size);
  out->magic = le16 (sb + OFF_s_magic);

  out->hj_start_block = le32 (sb + OFF_hj_start_block);
  out->hj_block_count = le32 (sb + OFF_hj_block_count);
  out->hj_crc32 = le32 (sb + OFF_hj_crc32);
  out->hj_magic = le32 (sb + OFF_hj_magic);
}

static void
write_hint (int fd, uint32_t start_block, uint32_t count_blocks)
{
  unsigned char sb[1024];
  pread_exact (fd, sb, sizeof (sb), EXT2_SB_BYTE_OFFSET,
	       "ext2 superblock (for write)");

  wr_le32 (sb + OFF_hj_start_block, start_block);
  wr_le32 (sb + OFF_hj_block_count, count_blocks);
  wr_le32 (sb + OFF_hj_crc32, 0);	// prototype: no crc yet
  wr_le32 (sb + OFF_hj_magic, HJ_MAGIC);	// 'JNLH'

  pwrite_exact (fd, sb, sizeof (sb), EXT2_SB_BYTE_OFFSET,
		"ext2 superblock (write)");
  fsync (fd);
}

static void
clear_hint (int fd)
{
  unsigned char sb[1024];
  pread_exact (fd, sb, sizeof (sb), EXT2_SB_BYTE_OFFSET,
	       "ext2 superblock (for clear)");

  wr_le32 (sb + OFF_hj_start_block, 0);
  wr_le32 (sb + OFF_hj_block_count, 0);
  wr_le32 (sb + OFF_hj_crc32, 0);
  wr_le32 (sb + OFF_hj_magic, 0);

  pwrite_exact (fd, sb, sizeof (sb), EXT2_SB_BYTE_OFFSET,
		"ext2 superblock (clear)");
  fsync (fd);
}

// ------------------------- math helpers -------------------------

static uint64_t
mib_to_bytes (uint64_t mib)
{
  return mib * 1024ull * 1024ull;
}

// ----------------- auto-detect ext partition offset (images) ----------------

static bool
detect_ext_offset_via_parted (const char *img_path, uint64_t *out_off)
{
  struct stat st;
  if (stat (img_path, &st) != 0)
    return false;
  if (!S_ISREG (st.st_mode))
    return false;

  char cmd[PATH_MAX + 64];
  snprintf (cmd, sizeof (cmd), "parted -sm '%s' unit B print 2>/dev/null",
	    img_path);
  FILE *f = popen (cmd, "r");
  if (!f)
    return false;

  char line[1024];
  uint64_t last_ext_offset = 0;
  while (fgets (line, sizeof (line), f))
    {
      if (!strchr (line, ':'))
	continue;
      char *save = NULL;
      (void) strtok_r (line, ":\n", &save);	// part #
      char *startB = strtok_r (NULL, ":\n", &save);	// start
      (void) strtok_r (NULL, ":\n", &save);	// end
      (void) strtok_r (NULL, ":\n", &save);	// size
      char *fstype = strtok_r (NULL, ":\n", &save);	// fs type
      if (!startB || !fstype)
	continue;
      if (strcmp (fstype, "ext2") != 0 && strcmp (fstype, "ext4") != 0)
	continue;

      size_t len = strlen (startB);
      if (len == 0 || startB[len - 1] != 'B')
	continue;
      startB[len - 1] = '\0';
      char *endp = NULL;
      uint64_t off = strtoull (startB, &endp, 10);
      if (endp && *endp == '\0')
	last_ext_offset = off;	// keep last ext partition
    }
  pclose (f);

  if (last_ext_offset > 0)
    {
      *out_off = last_ext_offset;
      return true;
    }
  return false;
}

// ------------------------- CLI -------------------------

typedef enum
{ CMD_NONE, CMD_SHOW, CMD_PLAN, CMD_APPLY, CMD_OFF, CMD_ON } cmd_t;

typedef struct
{
  cmd_t cmd;
  const char *path;
  uint64_t size_mib;		// desired journal size (used for plan/apply/on if fields empty)
  uint64_t shrink_mib;		// optional filesystem shrink (MiB) for plan/apply
  uint64_t offset_bytes;	// base offset for superblock (images with partitions)
} options_t;

static void
usage (const char *argv0)
{
  fprintf (stderr,
	   "hjhint — prototype ext2 journaling hint tool (Hurd journaling)\n"
	   "USAGE:\n"
	   "  %s show  <img|/dev/sdXY> [--offset-bytes BYTES]\n"
	   "  %s plan  <img|/dev/sdXY> --size-mib N [--shrink-mib N] [--offset-bytes BYTES]\n"
	   "  %s apply <img|/dev/sdXY> --size-mib N [--shrink-mib N] [--offset-bytes BYTES]\n"
	   "  %s on    <img|/dev/sdXY> [--size-mib N] [--offset-bytes BYTES]\n"
	   "  %s off   <img|/dev/sdXY> [--offset-bytes BYTES]\n"
	   "\n"
	   "NOTES:\n"
	   "- Only primary superblock is updated (prototype).\n"
	   "- Refuses to modify if target appears mounted rw.\n"
	   "- For images (.img), auto-detects ext2/4 partition offset via parted when not given.\n",
	   argv0, argv0, argv0, argv0, argv0);
  exit (2);
}

static options_t
parse (int argc, char **argv)
{
  options_t o = { 0 };
  if (argc < 3)
    usage (argv[0]);
  if (strcmp (argv[1], "show") == 0)
    o.cmd = CMD_SHOW;
  else if (strcmp (argv[1], "plan") == 0)
    o.cmd = CMD_PLAN;
  else if (strcmp (argv[1], "apply") == 0)
    o.cmd = CMD_APPLY;
  else if (strcmp (argv[1], "off") == 0)
    o.cmd = CMD_OFF;
  else if (strcmp (argv[1], "on") == 0)
    o.cmd = CMD_ON;
  else
    usage (argv[0]);

  o.path = argv[2];
  for (int i = 3; i < argc; i++)
    {
      if (strcmp (argv[i], "--size-mib") == 0 && i + 1 < argc)
	{
	  o.size_mib = strtoull (argv[++i], NULL, 10);
	}
      else if (strcmp (argv[i], "--shrink-mib") == 0 && i + 1 < argc)
	{
	  o.shrink_mib = strtoull (argv[++i], NULL, 10);
	}
      else if (strcmp (argv[i], "--offset-bytes") == 0 && i + 1 < argc)
	{
	  o.offset_bytes = strtoull (argv[++i], NULL, 10);
	}
      else
	{
	  usage (argv[0]);
	}
    }
  return o;
}

int
main (int argc, char **argv)
{
  options_t opt = parse (argc, argv);

  // Decide base offset (for images)
  g_base_off = opt.offset_bytes;	// user override if provided
  if (g_base_off == 0)
    {
      uint64_t auto_off = 0;
      if (detect_ext_offset_via_parted (opt.path, &auto_off))
	{
	  fprintf (stderr,
		   "[INFO] Auto-detected ext2/4 partition offset: %llu bytes\n",
		   (unsigned long long) auto_off);
	  g_base_off = auto_off;
	}
    }

  int flags = (opt.cmd == CMD_APPLY || opt.cmd == CMD_OFF
	       || opt.cmd == CMD_ON) ? O_RDWR : O_RDONLY;
  int fd = open (opt.path, flags);
  if (fd < 0)
    die ("open(%s)", opt.path);

  // Safety: mounted check for modifying commands (block devices only; images handled via loopdev in shrink path)
  if ((opt.cmd == CMD_APPLY || opt.cmd == CMD_OFF || opt.cmd == CMD_ON) &&
      is_block_device (opt.path) && appears_mounted_rw (opt.path))
    {
      die ("Refusing: %s appears mounted rw. Unmount or mount ro first.",
	   opt.path);
    }

  // Read SB and basic info
  sb_info_t sb;
  read_sb (fd, &sb);
  if (sb.magic != EXT2_SUPER_MAGIC)
    die ("Not an ext2/ext3/ext4 filesystem (bad magic 0x%04x)", sb.magic);

  // Compute sizes (relative to partition start)
  uint64_t block_size = 1024ull << sb.log_block_size;
  uint64_t fs_blocks = sb.blocks_count;
  uint64_t fs_first = sb.first_data_block;
  uint64_t fs_end_blk = fs_first + fs_blocks;	// first block *after* ext2
  uint64_t fs_end = fs_end_blk * block_size;	// bytes
  uint64_t dev_size = get_device_size_bytes (fd, opt.path);

  // Current hint (if any)
  bool hint_present = (sb.hj_magic == HJ_MAGIC) && (sb.hj_block_count > 0);
  uint64_t hint_start = (uint64_t) sb.hj_start_block * block_size;
  uint64_t hint_size = (uint64_t) sb.hj_block_count * block_size;

  if (opt.cmd == CMD_SHOW)
    {
      printf ("Device/Image: %s%s%s\n", opt.path,
	      (g_base_off ? " (offset " : ""), (g_base_off ? "" : ""));
      if (g_base_off)
	printf ("Partition offset      : %llu\n",
		(unsigned long long) g_base_off);
      printf ("Block size           : %llu\n",
	      (unsigned long long) block_size);
      printf ("First data block     : %u\n", sb.first_data_block);
      printf ("Ext2 blocks count    : %u\n", sb.blocks_count);
      printf ("Ext2 end (bytes)     : %llu\n", (unsigned long long) fs_end);
      printf ("Device/partition size: %llu\n", (unsigned long long) dev_size);
      if (hint_present)
	{
	  printf ("Hint: PRESENT  (magic=0x%08x 'JNLH')\n", sb.hj_magic);
	  printf ("      start_block=%u  block_count=%u  crc32=0x%08x\n",
		  sb.hj_start_block, sb.hj_block_count, sb.hj_crc32);
	  printf ("      bytes: start=%llu size=%llu\n",
		  (unsigned long long) hint_start,
		  (unsigned long long) hint_size);
	}
      else
	{
	  printf ("Hint: (none)\n");
	}
      close (fd);
      return 0;
    }

  if (opt.cmd == CMD_OFF)
    {
      clear_hint (fd);
      printf ("Cleared journaling hint in primary superblock.\n");
      close (fd);
      return 0;
    }

  if (opt.cmd == CMD_ON)
    {
      // Case A: fields already populated → just set magic
      if (sb.hj_start_block && sb.hj_block_count)
	{
	  write_hint (fd, sb.hj_start_block, sb.hj_block_count);
	  printf
	    ("[OK] Enabled journal hint with existing fields (start_block=%u, block_count=%u).\n",
	     sb.hj_start_block, sb.hj_block_count);
	  close (fd);
	  return 0;
	}
      // Case B: compute fields from --size-mib (no shrink)
      if (opt.size_mib == 0)
	{
	  die
	    ("No existing start/count in superblock and no --size-mib given.\n"
	     "Either pre-fill fields (e.g., journal-hint2.sh) or run with --size-mib N.");
	}
      uint64_t want_bytes = mib_to_bytes (opt.size_mib);
      uint64_t want_blocks = (want_bytes + block_size - 1) / block_size;	// ceil
      uint64_t j_start_blk = fs_end_blk;	// immediately after ext2
      uint64_t j_end_blk = j_start_blk + want_blocks;
      uint64_t j_end = j_end_blk * block_size;

      if (j_end > dev_size)
	{
	  die ("Requested --size-mib won't fit without shrinking: end=%"
	       PRIu64 " > dev=%" PRIu64 ".\n"
	       "Use: plan/apply with --shrink-mib or choose a smaller --size-mib.",
	       j_end, dev_size);
	}
      write_hint (fd, (uint32_t) j_start_blk, (uint32_t) want_blocks);
      printf ("[OK] Enabled journal hint (computed) start_block=%" PRIu64
	      ", block_count=%" PRIu64 ".\n", j_start_blk, want_blocks);
      close (fd);
      return 0;
    }

  if (opt.cmd == CMD_PLAN || opt.cmd == CMD_APPLY)
    {
      if (opt.size_mib == 0)
	die ("--size-mib is required for plan/apply");

      // If shrinking was requested, run it now
      if (opt.shrink_mib > 0)
	{
	  if (is_block_device (opt.path))
	    {
	      // Block device path: operate directly
	      if (appears_mounted_rw (opt.path))
		die ("Refusing to shrink: %s is mounted rw", opt.path);
	      char *const e2fsck_argv[] =
		{ "e2fsck", "-f", (char *) opt.path, NULL };
	      fprintf (stderr, "[*] Running e2fsck -f %s\n", opt.path);
	      int rc = run_cmd (e2fsck_argv);
	      if (rc != 0)
		die ("e2fsck returned %d", rc);

	      uint64_t shrink_bytes = mib_to_bytes (opt.shrink_mib);
	      uint64_t shrink_blocks = shrink_bytes / block_size;
	      if (shrink_blocks == 0)
		die ("shrink_mib too small for block size");
	      if (shrink_blocks >= fs_blocks)
		die ("shrink_mib too large (would underflow FS)");
	      uint64_t new_blocks = fs_blocks - shrink_blocks;

	      char new_blocks_str[64];
	      snprintf (new_blocks_str, sizeof (new_blocks_str), "%" PRIu64,
			new_blocks);
	      char *const resize_argv[] =
		{ "resize2fs", (char *) opt.path, new_blocks_str, NULL };
	      fprintf (stderr, "[*] Running resize2fs %s %s (blocks)\n",
		       opt.path, new_blocks_str);
	      rc = run_cmd (resize_argv);
	      if (rc != 0)
		die ("resize2fs returned %d", rc);
	    }
	  else
	    {
	      // Image path: map loop device at g_base_off and operate there
	      char *loopdev = losetup_attach (opt.path, g_base_off);
	      if (!loopdev)
		die ("Failed to losetup -f --show -o %" PRIu64 " %s",
		     (uint64_t) g_base_off, opt.path);
	      fprintf (stderr,
		       "[*] Attached loop device: %s (offset=%" PRIu64 ")\n",
		       loopdev, (uint64_t) g_base_off);

	      if (appears_mounted_rw (loopdev))
		{
		  die ("Refusing to shrink: %s mounted rw", loopdev);
		  losetup_detach (loopdev);
		  free (loopdev);
		}

	      char *const e2fsck_argv[] = { "e2fsck", "-f", loopdev, NULL };
	      fprintf (stderr, "[*] Running e2fsck -f %s\n", loopdev);
	      int rc = run_cmd (e2fsck_argv);
	      if (rc != 0)
		{
		  losetup_detach (loopdev);
		  free (loopdev);
		  die ("e2fsck returned %d", rc);
		}

	      uint64_t shrink_bytes = mib_to_bytes (opt.shrink_mib);
	      uint64_t shrink_blocks = shrink_bytes / block_size;
	      if (shrink_blocks == 0)
		{
		  losetup_detach (loopdev);
		  free (loopdev);
		  die ("shrink_mib too small for block size");
		}
	      if (shrink_blocks >= fs_blocks)
		{
		  losetup_detach (loopdev);
		  free (loopdev);
		  die ("shrink_mib too large (would underflow FS)");
		}
	      uint64_t new_blocks = fs_blocks - shrink_blocks;

	      char new_blocks_str[64];
	      snprintf (new_blocks_str, sizeof (new_blocks_str), "%" PRIu64,
			new_blocks);
	      char *const resize_argv[] =
		{ "resize2fs", loopdev, new_blocks_str, NULL };
	      fprintf (stderr, "[*] Running resize2fs %s %s (blocks)\n",
		       loopdev, new_blocks_str);
	      rc = run_cmd (resize_argv);
	      if (rc != 0)
		{
		  losetup_detach (loopdev);
		  free (loopdev);
		  die ("resize2fs returned %d", rc);
		}

	      losetup_detach (loopdev);
	      free (loopdev);
	    }

	  // After shrink, re-read SB from the image/device to refresh counts
	  read_sb (fd, &sb);
	  fs_blocks = sb.blocks_count;
	  fs_first = sb.first_data_block;
	  block_size = 1024ull << sb.log_block_size;
	  fs_end_blk = fs_first + fs_blocks;
	  fs_end = fs_end_blk * block_size;
	}

      // Compute proposed region: tail-placed journal immediately after ext2
      uint64_t want_bytes = mib_to_bytes (opt.size_mib);
      uint64_t want_blocks = (want_bytes + block_size - 1) / block_size;	// ceil
      uint64_t j_start_blk = fs_end_blk;	// immediately after ext2
      uint64_t j_end_blk = j_start_blk + want_blocks;
      uint64_t j_start = j_start_blk * block_size;
      uint64_t j_end = j_end_blk * block_size;

      // Validate fits in device/partition (relative to partition start)
      if (j_end > dev_size)
	{
	  die ("Planned journal region overruns device/partition: end=%"
	       PRIu64 " > dev=%" PRIu64, j_end, dev_size);
	}
      if (j_start < fs_end)
	{
	  die ("Planned journal start is inside ext2 (start=%" PRIu64
	       ", fs_end=%" PRIu64 ")", j_start, fs_end);
	}

      // Print plan
      printf ("Plan for %s%s%s\n", opt.path, (g_base_off ? " (offset " : ""),
	      (g_base_off ? "" : ""));
      if (g_base_off)
	printf ("  partition offset : %llu\n",
		(unsigned long long) g_base_off);
      printf ("  block_size       : %llu\n", (unsigned long long) block_size);
      printf ("  fs_end (bytes)   : %llu (block %" PRIu64 ")\n",
	      (unsigned long long) fs_end, fs_end_blk);
      printf ("  dev_size         : %llu\n", (unsigned long long) dev_size);
      printf ("  journal size     : %" PRIu64 " MiB (%" PRIu64 " blocks)\n",
	      opt.size_mib, want_blocks);
      printf ("  journal start    : %" PRIu64 " (byte)  [block %" PRIu64
	      "]\n", j_start, j_start_blk);
      printf ("  journal end      : %" PRIu64 " (byte)  [block %" PRIu64
	      "]\n", j_end, j_end_blk);

      if (opt.cmd == CMD_APPLY)
	{
	  if (is_block_device (opt.path) && appears_mounted_rw (opt.path))
	    {
	      die ("Refusing to apply: %s appears mounted rw", opt.path);
	    }
	  write_hint (fd, (uint32_t) j_start_blk, (uint32_t) want_blocks);
	  printf ("[OK] Wrote journaling hint to primary superblock.\n");
	  printf ("     (Prototype: backup superblocks not updated.)\n");
	}

      close (fd);
      return 0;
    }

  usage (argv[0]);
  return 2;
}
