#ifndef JOURNAL_GLOBALS_H
#define JOURNAL_GLOBALS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#ifndef DEBUG
#define DEBUG 1
#endif

// Logging macros
#define LOG_ERROR(fmt, ...) \
	do { fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)

#if DEBUG
#define LOG_DEBUG(fmt, ...) \
	do { fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)
#else
#define LOG_DEBUG(fmt, ...) do { } while (0)
#endif

#define RAW_DEVICE_PATH "/tmp/journal-pipe"

// Global state
extern volatile size_t dropped_events;
extern volatile bool journal_device_ready;
// Queue locks, needed for coordination.
extern pthread_mutex_t queue_lock;
extern pthread_cond_t queue_cond;

#endif // JOURNAL_GLOBALS_H
