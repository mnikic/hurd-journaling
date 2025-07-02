#ifndef JOURNAL_IO_H
#define JOURNAL_IO_H

#include <libdiskfs/journal_globals.h>

bool journal_read_and_validate_header(int fd, struct journal_header *out);
bool journal_read_and_validate_entry(int fd, uint64_t index,
                                     struct journal_payload_bin *out);

#endif // JOURNAL_IO_H
