#include <libdiskfs/journal_filter.h>
#include <libdiskfs/journal_policy.h>
#include <libdiskfs/journal_util.h>

#define FILTER_TABLE_ENTRIES 1024

static journal_filter_entry_t write_table[FILTER_TABLE_ENTRIES];
static journal_filter_entry_t timestamp_table[FILTER_TABLE_ENTRIES];

static journal_filter_instance_t write_filter = {
  .size = FILTER_TABLE_ENTRIES,
  .min_delta_sec = 10,
  .table = write_table,
};

static journal_filter_instance_t timestamp_filter = {
  .size = FILTER_TABLE_ENTRIES,
  .min_delta_sec = 1,
  .table = timestamp_table,
};

static bool
should_journal_path (const char *path)
{
  if (!path || *path == '\0')
    return true;		// Best-effort journaling fallback

  for (int i = 0; journal_excluded_prefixes[i]; i++)
    if (strncmp
	(path, journal_excluded_prefixes[i],
	 strlen (journal_excluded_prefixes[i])) == 0)
      return false;

  const char *filename = strrchr (path, '/');
  filename = filename ? filename + 1 : path;

  if (*filename == '\0')
    return true;

  static const char *excluded_hidden[] = {
    ".DS_Store", ".Thumbs.db", ".directory", ".gvfs",
    ".cache", ".tmp", ".temp", ".lock", ".pid", ".swp", ".swo", ".swn",
    ".swx",
    NULL
  };

  if (filename[0] == '.')
    for (int i = 0; excluded_hidden[i]; i++)
      if (strcmp (filename, excluded_hidden[i]) == 0)
	return false;

  static const char *excluded_extensions[] = {
    ".o", ".a", ".pyc", ".pyo", ".pyd", ".class", ".war", ".ear",
    ".tmp", ".temp", ".bak", ".backup", ".orig", ".rej", ".log",
    ".pid", ".lock", ".cache", ".dmp", ".core", ".stackdump",
    ".pdb", ".ilk", ".idb", ".tlog", ".lastbuildstate",
    ".unsuccessfulbuild", ".manifest", ".dpkg-new", ".dpkg-tmp",
    NULL
  };

  const char *ext = strrchr (filename, '.');
  if (ext)
    for (int i = 0; excluded_extensions[i]; i++)
      if (strcmp (ext, excluded_extensions[i]) == 0)
	return false;

  static const char *excluded_dirs[] = {
    "node_modules", "__pycache__", "CMakeFiles", ".cmake",
    "build", "Build", "BUILD", "target", "dist", "out",
    "bin", "obj", ".deps", ".libs", "autom4te.cache",
    ".tox", ".venv", "venv", "env", ".env", "coverage",
    ".nyc_output", ".pytest_cache", "__tests__", ".tests", "tests",
    NULL
  };

  char *path_copy = strdup (path);
  if (!path_copy)
    return true;

  char *token = strtok (path_copy, "/");
  while (token)
    {
      for (int i = 0; excluded_dirs[i]; i++)
	if (strcmp (token, excluded_dirs[i]) == 0)
	  {
	    free (path_copy);
	    return false;
	  }
      token = strtok (NULL, "/");
    }
  free (path_copy);

  // Build tool and language-specific cache patterns
  if (strstr (path, "/.gradle/") || strstr (path, "/build/") ||
      strstr (path, "/.m2/repository/") ||
      strstr (path, "/target/debug/") || strstr (path, "/target/release/") ||
      strstr (path, "/go/pkg/mod/") || strstr (path, "/.gocache/") ||
      strstr (path, "/.npm/") || strstr (path, "/.yarn/"))
    return false;

  size_t len = strlen (filename);
  if ((len > 0 && filename[len - 1] == '~') ||
      (filename[0] == '#' && len > 1 && filename[len - 1] == '#'))
    return false;		// Vim/Emacs backups

  return true;
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

  if (!journal_is_safe_stat (st))
    {
      JOURNAL_LOG_DEBUG ("Skipped inode %llu (mode %o) as unsafe.",
			 st->st_ino, st->st_mode);
      return false;
    }

  /* Low signal here, lets skip */
  if ((!full_path || full_path[0] == '\0') &&
      (info->action == JOURNAL_ACTION_ATIME ||
       info->action == JOURNAL_ACTION_UTIME ||
       info->action == JOURNAL_ACTION_WRITE))
    {
      JOURNAL_LOG_DEBUG ("Skipped low-value event with no path");
      return false;
    }

  if (info->action == JOURNAL_ACTION_WRITE
      && !journal_filter_should_log (&write_filter, st->st_ino, time (NULL)))
    {
      return false;
    }
  if (!should_journal_path (full_path))
    {
      return false;
    }

  /* Please keep time filtering last. If any event is recorded timestamps are updated. 
     So we need to update timestamp_filter. */
  time_t ts = safe_max_timestamp (st->st_atime, st->st_ctime, st->st_mtime);
  bool ignore_time = false;
  /* If one of the timestamps changed, check if it's worth logging */
  if (ts)
    ignore_time =
      !journal_filter_should_log (&timestamp_filter, st->st_ino, ts);

  /* If we are ignoring this and the only change was atime/utime, skip it */
  if (ignore_time &&
      (info->action == JOURNAL_ACTION_ATIME ||
       info->action == JOURNAL_ACTION_UTIME))
    {
      return false;
    }

  return true;
}
