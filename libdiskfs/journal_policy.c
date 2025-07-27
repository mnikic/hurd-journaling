#include <libdiskfs/journal_filter.h>
#include <libdiskfs/journal_policy.h>
#include <libdiskfs/journal_util.h>

// Build directory exclusions array
static const char *build_exclusions[] = {
  // Generic build directories
  "build", "Build", ".build", "builds", "_build",
  "cmake-build-debug", "cmake-build-release", "CMakeFiles",

  // Language-specific build directories  
  "target",			// Rust, Maven, Scala
  "node_modules",		// Node.js
  "dist",			// JavaScript, TypeScript, Python
  ".next", ".nuxt", ".vuepress",

  // Python
  "__pycache__", ".pytest_cache", ".tox", ".venv", "venv",
  ".env", "env", ".mypy_cache", ".coverage", "htmlcov",
  ".hypothesis",

  // Java/JVM
  "classes", "out", ".gradle", ".mvn",

  // .NET
  "bin", "obj", "packages", ".vs", ".vscode",

  // Go, PHP, Ruby
  "vendor", ".bundle", "tmp", "log",

  // Haskell, Elm, Dart
  ".stack-work", "dist-newstyle", "elm-stuff",
  ".dart_tool", ".packages",

  // Swift
  ".build", "Packages",

  // IDE and editor directories
  ".idea", ".eclipse", ".metadata",

  // Documentation build
  "_site", ".jekyll-cache", ".sass-cache", "site",

  // Test artifacts
  "coverage", ".nyc_output", "test-results", "test_results", "reports",

  // Package manager caches
  ".npm", ".yarn", ".pnpm-store",

  // Temporary and cache directories
  "temp", ".tmp", ".cache", "cache",

  NULL				// Sentinel
};

// File extensions and patterns to skip
static const char *skip_patterns[] = {
  ".swp", ".swo", ".tmp", ".log", ".pid", ".seed", ".lock",
  ".bak", ".backup", ".old", ".orig",
  ".db", ".sqlite", ".sqlite3",
  NULL
};

/**
 * Determines whether a given path should be journaled.
 * Returns true if the path should be logged, false if it should be excluded.
 */
bool
should_journal_path (const char *path)
{
  if (!path || *path == '\0')
    {
      return false;		// Don't journal empty/null paths
    }

  // System and temporary directories - exact matches and prefixes
  static const char *excluded_prefixes[] = {
    "/tmp/",			// Temporary files
    "/dev/",			// Device files
    "/proc/",			// Process filesystem
    "/sys/",			// System filesystem
    "/run/",			// Runtime data
    "/var/tmp/",		// Variable temporary files
    "/var/run/",		// Runtime variable data
    "/var/lock/",		// Lock files
    "/var/cache/",		// Cache files
    "/var/log/",		// Log files (avoid recursive logging)
    "/boot/",			// Boot files (usually read-only)
    NULL
  };

  // Check excluded prefixes
  for (int i = 0; excluded_prefixes[i] != NULL; i++)
    {
      if (strncmp (path, excluded_prefixes[i], strlen (excluded_prefixes[i]))
	  == 0)
	{
	  return false;
	}
    }

  // Build artifacts and temporary files by extension
  const char *filename = strrchr (path, '/');
  if (filename)
    {
      filename++;		// Skip the '/'
    }
  else
    {
      filename = path;		// No directory separator found
    }

  // Skip if filename is empty
  if (*filename == '\0')
    {
      return true;
    }

  // Hidden files that are commonly temporary or cache
  if (filename[0] == '.')
    {
      static const char *excluded_hidden[] = {
	".DS_Store",		// macOS
	".Thumbs.db",		// Windows
	".directory",		// KDE
	".gvfs",		// GNOME VFS
	".cache",		// Generic cache
	".tmp",			// Temporary
	".temp",		// Temporary
	".lock",		// Lock files
	".pid",			// Process ID files
	".swap",		// Swap files
	".swp",			// Vim swap files
	".swo",			// Vim swap files
	".swn",			// Vim swap files
	NULL
      };

      for (int i = 0; excluded_hidden[i] != NULL; i++)
	{
	  if (strcmp (filename, excluded_hidden[i]) == 0)
	    {
	      return false;
	    }
	}

      // Vim temporary files pattern .filename.swp
      size_t len = strlen (filename);
      if (len > 4 && strcmp (filename + len - 4, ".swp") == 0)
	{
	  return false;
	}
      if (len > 4 && strcmp (filename + len - 4, ".swo") == 0)
	{
	  return false;
	}
      if (len > 4 && strcmp (filename + len - 4, ".swn") == 0)
	{
	  return false;
	}
    }

  // Build artifacts and compiler output
  static const char *excluded_extensions[] = {
    ".o",			// Object files
    ".a",			// Static libraries
    ".so",			// Shared libraries (be careful - some are important)
    ".pyc",			// Python bytecode
    ".pyo",			// Python optimized bytecode
    ".pyd",			// Python dynamic libraries
    ".class",			// Java bytecode
    ".jar",			// Java archives (might want to keep some)
    ".war",			// Web application archives
    ".ear",			// Enterprise application archives
    ".tmp",			// Generic temporary
    ".temp",			// Generic temporary
    ".bak",			// Backup files
    ".backup",			// Backup files
    ".orig",			// Original files (patch/merge)
    ".rej",			// Rejected patches
    ".log",			// Log files
    ".pid",			// Process ID files
    ".lock",			// Lock files
    ".cache",			// Cache files
    ".dmp",			// Dump files
    ".core",			// Core dumps
    ".stackdump",		// Stack dumps
    ".obj",			// MSVC object files
    ".exe",			// Windows executables (if cross-compiling)
    ".dll",			// Windows DLLs
    ".pdb",			// Debug databases
    ".ilk",			// Incremental linker files
    ".idb",			// Debug databases
    ".tlog",			// Build logs
    ".lastbuildstate",		// Build state
    ".unsuccessfulbuild",	// Build markers
    ".manifest",		// Manifest files (some contexts)
    NULL
  };

  const char *ext = strrchr (filename, '.');
  if (ext)
    {
      for (int i = 0; excluded_extensions[i] != NULL; i++)
	{
	  if (strcmp (ext, excluded_extensions[i]) == 0)
	    {
	      return false;
	    }
	}
    }

  // Build directories - check if any path component matches
  static const char *excluded_dirs[] = {
    "node_modules",		// Node.js dependencies
    "__pycache__",		// Python cache
    "CMakeFiles",		// CMake build files
    ".cmake",			// CMake cache
    "build",			// Generic build directory
    "Build",			// Generic build directory (capitalized)
    "BUILD",			// Generic build directory (all caps)
    "target",			// Maven/Rust build output
    "dist",			// Distribution files
    "out",			// Output directory
    "bin",			// Binary directory (in project contexts)
    "obj",			// Object file directory
    ".deps",			// Dependency tracking
    ".libs",			// Libtool files
    "autom4te.cache",		// Autotools cache
    ".tox",			// Python tox environments
    ".venv",			// Python virtual environments
    "venv",			// Python virtual environments
    "env",			// Environment directories
    ".env",			// Environment directories
    "coverage",			// Test coverage output
    ".nyc_output",		// NYC coverage
    ".pytest_cache",		// Pytest cache
    "__tests__",		// Test directories
    ".tests",			// Test directories
    "tests",			// Test directories (be selective)
    ".coverage",		// Coverage files
    NULL
  };

  // Check each path component
  char *path_copy = strdup (path);
  if (!path_copy)
    {
      return true;		// If we can't allocate, err on the side of logging
    }

  char *token = strtok (path_copy, "/");
  bool should_exclude = false;

  while (token != NULL)
    {
      for (int i = 0; excluded_dirs[i] != NULL; i++)
	{
	  if (strcmp (token, excluded_dirs[i]) == 0)
	    {
	      should_exclude = true;
	      break;
	    }
	}
      if (should_exclude)
	break;
      token = strtok (NULL, "/");
    }

  free (path_copy);

  if (should_exclude)
    {
      return false;
    }

  // Additional patterns for build tools

  // Gradle build files
  if (strstr (path, "/.gradle/") || strstr (path, "/build/"))
    {
      return false;
    }

  // Maven build files
  if (strstr (path, "/.m2/repository/"))
    {
      return false;
    }

  // Cargo (Rust) build files
  if (strstr (path, "/target/debug/") || strstr (path, "/target/release/"))
    {
      return false;
    }

  // Go build cache
  if (strstr (path, "/go/pkg/mod/") || strstr (path, "/.gocache/"))
    {
      return false;
    }

  // npm/yarn cache
  if (strstr (path, "/.npm/") || strstr (path, "/.yarn/"))
    {
      return false;
    }

  // Temporary vim files in any directory
  size_t filename_len = strlen (filename);
  if (filename_len > 0 && filename[filename_len - 1] == '~')
    {
      return false;		// Vim backup files ending with ~
    }

  // Emacs temporary files
  if (filename[0] == '#' && filename[filename_len - 1] == '#')
    {
      return false;		// Emacs auto-save files #file#
    }

  return true;			// Default: journal the file
}

static bool
should_skip_directory (const char *name)
{
  if (strcmp (name, "lost+found") == 0 ||
      strcmp (name, "proc") == 0 ||
      strcmp (name, "sshd") == 0 ||
      strcmp (name, "crond") == 0 ||
      strcmp (name, "dbus") == 0 ||
      strcmp (name, "dev") == 0 ||
      strcmp (name, "network") == 0 ||
      strcmp (name, "lock") == 0 ||
      strcmp (name, "shm") == 0 ||
      strstr (name, ".pid") != NULL ||
      strstr (name, ".lock") != NULL ||
      strstr (name, ".reboot") != NULL ||
      strstr (name, ".ok") != NULL || strchr (name, ':') != NULL)
    return true;

  // Check against build exclusions array
  for (int i = 0; build_exclusions[i]; i++)
    {
      if (strcmp (name, build_exclusions[i]) == 0)
	{
	  return true;
	}
    }

  // Check for common build directory patterns
  if (strstr (name, "build") || strstr (name, "Build"))
    return true;
  if (strstr (name, "cache") || strstr (name, "Cache"))
    return true;
  if (strstr (name, "tmp") || strstr (name, "temp"))
    return true;
  if (strstr (name, "dist") || strstr (name, "distribution"))
    return true;
  if (strstr (name, "target"))
    return true;
  if (strstr (name, "output") || strstr (name, "Output"))
    return true;
  if (strstr (name, "generated") || strstr (name, "Generated"))
    return true;

  // Skip hidden directories that are typically build artifacts
  if (name[0] == '.' && strlen (name) > 1)
    {
      if (strstr (name, "build") || strstr (name, "cache") ||
	  strstr (name, "tmp") || strstr (name, "test"))
	{
	  return true;
	}
    }

  // Check for file patterns (in case this is called on files too)
  for (int i = 0; skip_patterns[i]; i++)
    {
      if (strstr (name, skip_patterns[i]) != NULL)
	{
	  return true;
	}
    }


  // Skip OS-specific files
  if (strcmp (name, ".DS_Store") == 0 ||
      strcmp (name, "Thumbs.db") == 0 || strcmp (name, "desktop.ini") == 0)
    {
      return true;
    }


  // Skip common temporary files
  if (name[strlen (name) - 1] == '~')
    {				// Ends with ~
      return true;
    }

  return false;
}

static inline time_t
safe_max_timestamp (time_t atime, time_t mtime, time_t ctime)
{
  time_t result = 0;

  if (atime >= JOURNAL_MIN_REASONABLE_TIME
      && atime <= JOURNAL_MAX_REASONABLE_TIME)
    result = atime;

  if (mtime >= JOURNAL_MIN_REASONABLE_TIME
      && mtime <= JOURNAL_MAX_REASONABLE_TIME)
    result = (result > mtime) ? result : mtime;

  if (ctime >= JOURNAL_MIN_REASONABLE_TIME
      && ctime <= JOURNAL_MAX_REASONABLE_TIME)
    result = (result > ctime) ? result : ctime;

  return result;
}

bool
journal_should_log_event (const struct node *np,
			  const struct journal_entry_info *info,
			  journal_inode_denylist_t * ino_denylist,
			  char *full_path)
{
  if (!np)
    {
      JOURNAL_LOG_ERROR
	("NULL node_ptr received in journal_log_metadata, skipping.");
      return false;
    }

  if (!info)
    {
      JOURNAL_LOG_ERROR
	("NULL info pointer received in journal_log_metadata, skipping.");
      return false;
    }

  const struct stat *st = &np->dn_stat;

  if (journal_inode_denylist_contains
      (ino_denylist, (journal_ino_t) st->st_ino))
    {
      return false;
    }
  if (info->parent_ino && journal_inode_denylist_contains
      (ino_denylist, (journal_ino_t) info->parent_ino))
    {
      return false;
    }

  if (!journal_is_safe_stat (st))
    {
      JOURNAL_LOG_DEBUG ("Skipped inode %llu (mode %o) as unsafe.",
			 st->st_ino, st->st_mode);
      return false;
    }

  time_t ts = safe_max_timestamp (st->st_atime, st->st_ctime, st->st_mtime);
  bool ignore_time = false;
  /* If one of the timestamps changed, check if it's worth logging */
  if (ts)
    ignore_time = !journal_filter_should_log (st->st_ino, ts);

  /* If we are ignoring this and the only change was atime/utime, skip it */
  if (ignore_time &&
      (info->action == JOURNAL_ACTION_ATIME ||
       info->action == JOURNAL_ACTION_UTIME))
    {
      return false;
    }

  return true; // should_journal_path (full_path);
}
