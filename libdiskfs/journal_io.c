#include <libdiskfs/journal_io.h>
#include <libdiskfs/journal_globals.h>
#include <libdiskfs/crc32.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>

bool
journal_read_and_validate_header (int fd, struct journal_header *out)
{
  struct journal_header hdr = { 0 };
  ssize_t n = pread (fd, &hdr, sizeof (hdr), 0);
  if (n != sizeof (hdr))
    {
      LOG_ERROR ("journal replay: could not read journal header.");
      return false;
    }

  uint32_t expected_crc = hdr.crc32;
  hdr.crc32 = 0;
  uint32_t actual_crc = crc32 ((const void *) &hdr, sizeof (hdr));
  if (actual_crc != expected_crc
      || hdr.magic != JOURNAL_MAGIC || hdr.version != JOURNAL_VERSION)
    {
      LOG_DEBUG ("journal replay: header invalid.");
      return false;
    }

  if (hdr.start_index >= JOURNAL_NUM_ENTRIES
      || hdr.end_index >= JOURNAL_NUM_ENTRIES)
    {
      LOG_DEBUG ("journal_write_raw: header indices out of bounds.");
      return false;
    }
  *out = hdr;
  return true;
}

bool
journal_read_and_validate_entry (int fd, uint64_t index,
				 struct journal_payload_bin *out)
{
  char buf[JOURNAL_ENTRY_SIZE] = { 0 };
  uint64_t offset = index_to_offset (index);
  if (pread (fd, buf, JOURNAL_ENTRY_SIZE, (off_t) offset) !=
      JOURNAL_ENTRY_SIZE)
    {
      LOG_DEBUG ("Incomplete journal entry read at offset %ld.",
		 (long) offset);
      return false;
    }

  struct journal_entry_bin *entry = (struct journal_entry_bin *) buf;
  if (entry->magic != JOURNAL_MAGIC)
    {
      LOG_DEBUG ("Bad journal entry magic at offset %ld.", (long) offset);
      return false;
    }

  if (entry->version != JOURNAL_VERSION)
    {
      LOG_DEBUG ("Journal entry version mismatch at offset %ld",
		 (long) offset);
      return false;
    }
  uint32_t stored_crc = entry->crc32;
  entry->crc32 = 0;
  uint32_t actual_entry_crc = crc32 ((const char *) &entry->payload,
				     sizeof (struct journal_payload_bin));
  if (actual_entry_crc != stored_crc)
    {
      LOG_DEBUG ("Journal entry CRC mismatch at offset %ld.", (long) offset);
      return false;
    }
  *out = entry->payload;
  return true;
}
