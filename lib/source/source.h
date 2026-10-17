/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_SOURCE_H
#define FSIMG_SOURCE_H

#include "host.h"
#include "writer.h"

#define FSIMG_SOURCE_HEADER 80u

/* Host records contain ten little-endian u64 fields: root ID, device, inode,
 * length, mtime seconds/nanoseconds, ctime seconds/nanoseconds, kind, version;
 * a NUL-terminated relative path follows. Signed seconds use modulo-2^64
 * encoding. These offsets are unrelated to the entry's stable catalog ID.
 */

struct fsimg_source_root {
	fsimg_source_root_t *next;
	fsimg_host_file_t identity;
	uint64_t	 id;
	uint64_t	 mount_id;
	int		 fd;
};

/* Source state is embedded in the writer; callers retain no cursor handle.
 * Finish validates after the final read and closes it on every outcome.
 * Writer failure/destruction also closes it. Source records have a separate
 * buffer, so source operations preserve the catalog's current snapshot.
 * Reads clip their requested length to EOF and report confirmed bytes; callers
 * copying an exact range must check the returned length. Past-EOF offsets fail.
 */
void fsimg_sources_release(fsimg_writer_t *);
/* Internal startup consumes an already-reserved root FD; scanning occurs here. */
fsimg_status_t fsimg_writer_adopt_tree(fsimg_writer_t *, int, const char *,
    fsimg_error_t *);
fsimg_status_t fsimg_source_open(fsimg_writer_t *, uint64_t, fsimg_error_t *);
fsimg_status_t fsimg_source_read(fsimg_writer_t *, uint64_t, void *,
    size_t, size_t *, fsimg_error_t *);
fsimg_status_t fsimg_source_range(fsimg_writer_t *, uint64_t,
    fsimg_range_t *, fsimg_error_t *);
fsimg_status_t fsimg_source_finish(fsimg_writer_t *, fsimg_error_t *);
fsimg_status_t fsimg_source_scan(fsimg_writer_t *, fsimg_source_root_t *,
    const char *, fsimg_error_t *);
bool fsimg_source_identity_equal(const fsimg_host_file_t *,
    const fsimg_host_file_t *);
fsimg_status_t fsimg_source_check_object(fsimg_writer_t *, int,
    const fsimg_host_file_t *, const fsimg_source_root_t *, fsimg_error_t *);

#endif
