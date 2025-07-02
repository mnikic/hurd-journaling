/* journal_queue.c - Asynchronous queue for toy journaling system

   Copyright (C) 2025 Free Software Foundation, Inc.

   Written by Milos Nikic.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   The GNU Hurd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with the GNU Hurd; if not, see <https://www.gnu.org/licenses/>.  */

#include <libdiskfs/journal_queue.h>
#include <libdiskfs/journal_format.h>
#include <libdiskfs/journal_writer.h>
#include <libdiskfs/journal_globals.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <hurd/fshelp.h>
#include <errno.h>

#define JOURNAL_FLUSH_TIMEOUT_MS 500
#define JOURNAL_QUEUE_MAX 4096

struct journal_queue_entry
{
  char data[sizeof (struct journal_payload_bin)];
  size_t len;
  bool used;
};

static struct journal_queue_entry journal_queue[JOURNAL_QUEUE_MAX];
static size_t head = 0, tail = 0, count = 0;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
static bool shutdown_in_progress = false;

void
journal_queue_init (void)
{
  shutdown_in_progress = false;
  head = tail = count = 0;
  for (size_t i = 0; i < JOURNAL_QUEUE_MAX; i++)
    {
      journal_queue[i].len = 0;
      journal_queue[i].used = false;
    }
}

void
journal_queue_shutdown (void)
{
  pthread_mutex_lock (&queue_lock);
  shutdown_in_progress = true;
  pthread_cond_signal (&queue_cond);
  pthread_mutex_unlock (&queue_lock);
}

bool
journal_enqueue (const char *data, size_t len)
{
  if (len != sizeof (struct journal_payload_bin))
    return false;

  pthread_mutex_lock (&queue_lock);

  if (count >= JOURNAL_QUEUE_MAX)
    {
      pthread_mutex_unlock (&queue_lock);
      return false;
    }

  struct journal_queue_entry *e = &journal_queue[tail];
  memcpy (e->data, data, len);
  e->len = len;
  e->used = true;

  tail = (tail + 1) % JOURNAL_QUEUE_MAX;
  count++;

  pthread_cond_signal (&queue_cond);
  pthread_mutex_unlock (&queue_lock);
  return true;
}

void
journal_flush_now (void)
{
  pthread_mutex_lock (&queue_lock);
  pthread_cond_signal (&queue_cond);
  pthread_mutex_unlock (&queue_lock);
}

void *
journal_flusher_thread (void *arg)
{
  while (1)
    {
      // Wait until the journal device is ready
      while (!journal_device_ready && !shutdown_in_progress)
	{
	  usleep (100 * 1000);	// Sleep 100ms
	}

      pthread_mutex_lock (&queue_lock);

      while (count == 0 && !shutdown_in_progress)
	pthread_cond_wait (&queue_cond, &queue_lock);

      if (shutdown_in_progress && count == 0)
	{
	  pthread_mutex_unlock (&queue_lock);
	  break;
	}

      struct timespec start;
      clock_gettime (CLOCK_REALTIME, &start);

      struct timespec deadline = start;
      deadline.tv_nsec += (JOURNAL_FLUSH_TIMEOUT_MS % 1000) * 1000000;
      deadline.tv_sec +=
	JOURNAL_FLUSH_TIMEOUT_MS / 1000 + deadline.tv_nsec / 1000000000;
      deadline.tv_nsec %= 1000000000;

      while (count < JOURNAL_QUEUE_MAX && !shutdown_in_progress)
	{
	  struct timespec now;
	  clock_gettime (CLOCK_REALTIME, &now);
	  if (now.tv_sec > deadline.tv_sec
	      || (now.tv_sec == deadline.tv_sec
		  && now.tv_nsec >= deadline.tv_nsec))
	    break;
	  pthread_cond_timedwait (&queue_cond, &queue_lock, &deadline);
	}

      // If the device went away again, skip flushing
      if (!journal_device_ready)
	{
	  pthread_mutex_unlock (&queue_lock);
	  continue;
	}

      size_t batch_count = count;
      struct journal_payload batch[JOURNAL_QUEUE_MAX];

      for (size_t i = 0; i < batch_count; i++)
	{
	  batch[i].data = journal_queue[head].data;
	  batch[i].len = journal_queue[head].len;
	  journal_queue[head].used = false;
	  head = (head + 1) % JOURNAL_QUEUE_MAX;
	}
      count = 0;

      pthread_mutex_unlock (&queue_lock);
      journal_write_raw (batch, batch_count);
    }
  return NULL;
}
