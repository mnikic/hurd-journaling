/*
 * journal.c - Experimental journaling layer for Hurd's ext2fs/libdiskfs
 *
 * This is a work-in-progress implementation of a toy journaling layer
 * intended for exploration and learning purposes. It logs basic metadata
 * about file changes into a shared in-memory buffer, which is periodically
 * flushed to a file (/tmp/journal.log).
 *
 * Features:
 *   - Logs inode metadata (mode, size, nlink, mtime, ctime, etc.)
 *   - Each log entry is wrapped in a transaction with a unique ID and timestamp
 *   - Uses a fixed-size in-memory buffer with auto-flushing on overflow
 *   - Timestamp includes millisecond precision
 *   - Thread-safe using a mutex
 *
 * Missing / Not Implemented Yet:
 *   - Write barriers or guarantees of ordering with actual FS operations
 *   - Integration at a lower level to capture all metadata changes (not just sync hooks)
 *   - Actual recovery mechanisms or replays from the journal
 *   - Logging of inode or block bitmap changes
 *   - File name resolution (only available if passed manually)
 *   - UID/GID or finer-grained permission changes
 *   - Disk-backed circular journal buffer for continuous logging
 *   - Atomicity guarantees across flush boundaries (currently only soft protection)
 *
 * Warning:
 *   This code is experimental and not suitable for production.
 *   It is designed to support incremental development and learning.
 *
 * Author: Milos Nikic, 2025
 */
#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <libdiskfs/journal.h>
#include <diskfs.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <pthread.h>

#define JOURNAL_DIR_PATH "/tmp"
#define JOURNAL_LOG_PATH JOURNAL_DIR_PATH "/journal.log"
#define JOURNAL_BUF_SIZE (64 * 1024) 
#define MAX_REASONABLE_TIME 4102444800  /* Jan 1, 2100 */
#define MIN_REASONABLE_TIME 946684800   /* Jan 1, 2000 */

static pthread_mutex_t journal_lock = PTHREAD_MUTEX_INITIALIZER;
static char journal_buf[JOURNAL_BUF_SIZE];
static size_t journal_buf_used = 0;
static uint64_t journal_tx_id = 1;

static void get_current_time_string(char *buf, size_t bufsize)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    snprintf(buf, bufsize, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm.tm_year + 1900,
             tm.tm_mon + 1,
             tm.tm_mday,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec,
             (long)tv.tv_usec / 1000); // convert microseconds to milliseconds
}

static inline bool try_add_to_buffer(const char *msg, size_t msg_len)
{
    size_t total_len = msg_len + 1; // +1 for newline

    if (journal_buf_used + total_len < JOURNAL_BUF_SIZE) {
        memcpy(&journal_buf[journal_buf_used], msg, msg_len);
        journal_buf_used += msg_len;
        journal_buf[journal_buf_used++] = '\n';
	return true;
    } 
    return false;
}

static void journal_log_tx(const char *body)
{
    char time_str[128];
    get_current_time_string(time_str, sizeof(time_str));

    // Prepare transaction header/footer strings here to know length before locking
    char header[128];
    char footer[64];
    uint64_t tx_id;

    // We need body length for buffer size checks
    size_t body_len = strlen(body);
    size_t header_len, footer_len, total_len;

    // Lock scope to get tx_id and prepare header/footer lengths
    pthread_mutex_lock(&journal_lock);
    tx_id = journal_tx_id + 1;

    header_len = snprintf(header, sizeof(header), "=== BEGIN TX %" PRIu64 " === [%s]", tx_id, time_str);
    footer_len = snprintf(footer, sizeof(footer), "=== END TX %" PRIu64 " ===", tx_id);

    total_len = header_len + 1 + body_len + 1 + footer_len + 1; // +1 for each newline

    // If transaction bigger than buffer, drop it
    if (total_len >= JOURNAL_BUF_SIZE) {
        pthread_mutex_unlock(&journal_lock);
	fprintf(stderr, "Toy journaling: tx %" PRIu64 " too large (%zu bytes), dropping\n", tx_id, total_len);
        return;
    }

    if (journal_buf_used + total_len >= JOURNAL_BUF_SIZE) {
        pthread_mutex_unlock(&journal_lock);
	fprintf(stderr, "Toy journaling: tx %" PRIu64 " could not be written - buffer full and flush disabled\n", tx_id);
        return;
    }

    try_add_to_buffer(header, header_len);
    try_add_to_buffer(body, body_len);
    try_add_to_buffer(footer, footer_len);

    journal_tx_id = tx_id;
    pthread_mutex_unlock(&journal_lock);
}

bool flush_journal_to_file(void)
{
    if (journal_buf_used == 0) {
        fprintf(stderr, "Toy journaling: Nothing to flush. Skipping.\n");
	return false;
    }
    struct stat st;
    if (stat(JOURNAL_DIR_PATH, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Toy journaling: %s not accessible or not a directory. Skipping flush.\n", JOURNAL_DIR_PATH);
        return false;
    }
    FILE *f = fopen(JOURNAL_LOG_PATH, "a");
    if (f) {
        fprintf(stderr, "Toy journaling: Writing to %zu chars to %s file.\n", journal_buf_used, JOURNAL_LOG_PATH);
	size_t written = fwrite(journal_buf, 1, journal_buf_used, f);
	bool success = written == journal_buf_used;
	if (!success) {
	    fprintf(stderr, "Toy journaling: fwrite to %s failed: %s\n", JOURNAL_LOG_PATH, strerror(errno));
	}
	if (fclose(f) != 0) {
	    fprintf(stderr, "Toy journaling: fclose failed: %s\n", strerror(errno));
	}
        journal_buf_used = 0;
	return success;
    } else {
	fprintf(stderr, "Toy journaling: Failed to open %s: %s. Skipping flush.\n",
                JOURNAL_LOG_PATH, strerror(errno));
	return false;
    }
}

void journal_init(void)
{
    fprintf(stderr, "Toy journaling: journal_init() called\n");
}

void journal_shutdown(void)
{
    fprintf(stderr, "Toy journaling: journal_shutdown() called\n");
}

struct tx_buffer {
    char buf[2048];
    size_t used;
};


static void tx_printf(struct tx_buffer *tx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    size_t available = sizeof(tx->buf) - tx->used;
    int written = vsnprintf(tx->buf + tx->used, available, fmt, ap);
    if (written > 0 && (size_t)written < available) {
        tx->used += written;
    } else {
        fprintf(stderr, "Toy journaling: tx_printf truncated output (wanted %d bytes, had %zu)\n",
                written, available);
        tx->used = sizeof(tx->buf) - 1;
    }

    va_end(ap);
}

static void
tx_log_time_field(struct tx_buffer *tx, const char *label, time_t value)
{
    if (value > MIN_REASONABLE_TIME && value < MAX_REASONABLE_TIME)
        tx_printf(tx, "%s: %ld\n", label, (long)value);
    else
        tx_printf(tx, "%s: [invalid or uninitialized: %ld]\n", label, (long)value);
}

void
journal_log_metadata(void *node_ptr, const struct journal_entry_info *info)
{
    struct node *np = (struct node *) node_ptr;
    struct tx_buffer tx = { .used = 0 };

    if (!np) {
        fprintf(stderr, "Toy journaling: Null node passed. Skipping.\n");
        return;
    }

    const struct stat *st = &np->dn_stat;

    const char *action = info && info->action ? info->action : "unknown";
    const char *name = info && info->name ? info->name : "(unknown)";
    ino_t parent_ino = info ? info->parent_ino : 0;

    tx_printf(&tx, "action: %s\n", action);
    tx_printf(&tx, "name: %s\n", name);
    tx_printf(&tx, "parent inode: %" PRIuMAX "\n", (uintmax_t)parent_ino);
    tx_printf(&tx, "inode:        %" PRIuMAX "\n", (uintmax_t) st->st_ino);

    if (st->st_mode == 0)
        tx_printf(&tx, "mode:         (unset)\n");
    else
        tx_printf(&tx, "mode:         0%o\n", st->st_mode);

    if ((ssize_t)st->st_size < 0)
        tx_printf(&tx, "size:         (invalid: negative)\n");
    else
        tx_printf(&tx, "size:         %" PRIdMAX " bytes\n", (intmax_t) st->st_size);

    if (st->st_nlink == 0) {
        tx_printf(&tx, "nlink:        0 (file may have been unlinked, skipping rest)\n");
	journal_log_tx(tx.buf);
        return;
    }

    tx_printf(&tx, "nlink:        %" PRIuMAX "\n", (uintmax_t) st->st_nlink);
    tx_printf(&tx, "blocks:       %" PRIuMAX "\n", (uintmax_t) st->st_blocks);

    tx_log_time_field(&tx, "mtime", st->st_mtime);
    tx_log_time_field(&tx, "ctime", st->st_ctime);

    journal_log_tx(tx.buf);
}
