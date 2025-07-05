#ifndef JOURNAL_GLOBALS_H
#define JOURNAL_GLOBALS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#ifndef DEBUG
#define DEBUG 1
#endif

#define LOG_ERROR(fmt, ...) \
	do { fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)

#if DEBUG
#define LOG_DEBUG(fmt, ...) \
	do { fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)
#else
#define LOG_DEBUG(fmt, ...) do { } while (0)
#endif

#define RAW_DEVICE_PATH "/tmp/journal-pipe"
#define RAW_DEVICE_SIZE (8 * 1024 * 1024)	/* 8MB */
#define JOURNAL_ENTRY_SIZE 4096ULL
#define JOURNAL_RESERVED_SPACE 4096ULL	/* Leave room for future header growth */
#define JOURNAL_DATA_CAPACITY (RAW_DEVICE_SIZE - JOURNAL_RESERVED_SPACE)
#define JOURNAL_NUM_ENTRIES (JOURNAL_DATA_CAPACITY / JOURNAL_ENTRY_SIZE)

// Global state
extern volatile size_t dropped_events;
extern volatile bool journal_device_ready;

// Queue locks, needed for coordination.
extern pthread_mutex_t queue_lock;
extern pthread_cond_t queue_cond;

struct __attribute__((__packed__)) journal_header
{
	uint32_t magic;
	uint32_t version;
	uint64_t start_index;
	uint64_t end_index;
	uint32_t crc32;
};

struct __attribute__((__packed__)) journal_entry_bin
{
	uint32_t magic;
	uint32_t version;
	struct journal_payload_bin payload;
	uint8_t padding[JOURNAL_ENTRY_SIZE - sizeof (uint32_t) - sizeof (uint32_t) -
		sizeof (struct journal_payload_bin) - sizeof (uint32_t)];
	uint32_t crc32;
};

	static inline uint64_t
index_to_offset (uint64_t index)
{
	return JOURNAL_RESERVED_SPACE +
		(index % (uint64_t) JOURNAL_NUM_ENTRIES) * (uint64_t) JOURNAL_ENTRY_SIZE;
}

#endif // JOURNAL_GLOBALS_H
