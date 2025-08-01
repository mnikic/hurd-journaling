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

/* Expects name to be non null and non empty! */
static bool
should_journal_name(const char *name)
{
  static const char *excluded_hidden[] = {
    ".DS_Store", ".Thumbs.db", ".directory", ".gvfs",
    ".cache", ".tmp", ".temp", ".lock", ".pid", ".swp", ".swo", ".swn",
    ".swx", NULL
  };

  size_t len = strlen(name);

  for (int i = 0; excluded_hidden[i]; i++)
    {
      size_t suffix_len = strlen(excluded_hidden[i]);
      if (len >= suffix_len &&
          strcmp(name + len - suffix_len, excluded_hidden[i]) == 0)
        return false;
    }

  static const char *excluded_extensions[] = {
    ".o", ".a", ".pyc", ".pyo", ".pyd", ".class", ".war", ".ear",
    ".tmp", ".temp", ".bak", ".backup", ".orig", ".rej", ".log",
    ".pid", ".lock", ".cache", ".dmp", ".core", ".stackdump",
    ".pdb", ".ilk", ".idb", ".tlog", ".lastbuildstate",
    ".unsuccessfulbuild", ".manifest", ".dpkg-new", ".dpkg-tmp",
    ".s", ".lo", ".la", ".so", ".ko", ".mod", ".cmd", ".sym", ".map", ".bin",
    ".elf", ".gz", ".xz", ".zst", ".d", NULL
  };

  const char *ext = strrchr(name, '.');
  if (ext)
    for (int i = 0; excluded_extensions[i]; i++)
      if (strcmp(ext, excluded_extensions[i]) == 0)
        return false;

  // Vim/Emacs backup patterns
  if ((len > 0 && name[len - 1] == '~') ||
      (name[0] == '#' && len > 1 && name[len - 1] == '#'))
    return false;

  return true;
}

/* Expects path to be non null and non empty! */
static bool
should_journal_dir_path(const char *path)
{
  // Directory prefix filtering
  for (int i = 0; journal_excluded_prefixes[i]; i++)
    if (strncmp(path, journal_excluded_prefixes[i],
                strlen(journal_excluded_prefixes[i])) == 0)
      return false;

  static const char *excluded_dirs[] = {
    "node_modules", "__pycache__", "CMakeFiles", ".cmake",
    "build", "Build", "BUILD", "target", "dist", "out",
    "bin", "obj", ".deps", ".libs", "autom4te.cache",
    ".tox", ".venv", "venv", "env", ".env", "coverage",
    ".nyc_output", ".pytest_cache", "__tests__", ".tests", "tests",
    NULL
  };

  char *path_copy = strdup(path);
  if (!path_copy)
    return true;

  char *token = strtok(path_copy, "/");
  while (token)
    {
      for (int i = 0; excluded_dirs[i]; i++)
        if (strcmp(token, excluded_dirs[i]) == 0)
          {
            free(path_copy);
            return false;
          }
      token = strtok(NULL, "/");
    }
  free(path_copy);

  // Build system-specific subpaths
  if (strstr(path, "/.gradle/") ||
      strstr(path, "/build/") ||
      strstr(path, "/.m2/repository/") ||
      strstr(path, "/target/debug/") ||
      strstr(path, "/target/release/") ||
      strstr(path, "/go/pkg/mod/") ||
      strstr(path, "/.gocache/") ||
      strstr(path, "/.npm/") ||
      strstr(path, "/.yarn/"))
    return false;

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

static inline bool 
should_journal_filename_fallback (const char *name, const char *path)
{
  const char *filename = NULL;

  if (name && name[0])
    filename = name;
  else if (path && path[0])
    {
      const char *slash = strrchr(path, '/');
      filename = slash ? slash + 1 : path;
    }

  if (!filename || !filename[0])
    return true;

  return should_journal_name(filename);
}

bool
journal_should_log_event (const struct node *np,
			  const struct journal_entry_info *info,
			  journal_inode_denylist_t * ino_denylist,
			  const char *full_path)
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
      JOURNAL_LOG_DEBUG ("Skipped node %llu (mode %o) as unsafe.",
			 st->st_ino, st->st_mode);
      return false;
    }

  /* Low signal here, lets skip */
  if ((!full_path || full_path[0] == '\0') &&
      (info->action == JOURNAL_ACTION_ATIME ||
       info->action == JOURNAL_ACTION_UTIME ||
       info->action == JOURNAL_ACTION_WRITE))
    {
      //JOURNAL_LOG_DEBUG ("Skipped node %llu low-value event with no path",
	//		 st->st_ino);
      return false;
    }

  if (info->action == JOURNAL_ACTION_WRITE
      && !journal_filter_should_log (&write_filter, st->st_ino, time (NULL)))
    {
      return false;
    }
  if (full_path && full_path[0] != '\0')
  {
    if (!should_journal_dir_path(full_path))
      {
      JOURNAL_LOG_DEBUG ("Skipped node %llu path %s is rejected.",
			 st->st_ino, full_path);
        return false; // Explicitly reject
      }
  }
  if (!should_journal_filename_fallback(info->name, full_path))
    return false;

  /* Please keep time filtering last. If any event is recorded timestamps are updated. 
     So we need to update timestamp_filter when things pass eveything else. */
  time_t ts = safe_max_timestamp (st->st_atime, st->st_ctime, st->st_mtime);
  bool ignore_time = false;
  /* If one of the timestamps changed, check if it's worth logging */
  if (ts)
    ignore_time =
      !journal_filter_should_log (&timestamp_filter, st->st_ino, ts);

  /* If we don't think its worth logging and the change was only atime/utime, skip it */
  if (ignore_time &&
      (info->action == JOURNAL_ACTION_ATIME ||
       info->action == JOURNAL_ACTION_UTIME))
    {
      return false;
    }

  return true;
}
