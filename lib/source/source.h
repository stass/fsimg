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
 * a NUL-terminated relative path follows, empty exactly when the root is the
 * regular file itself. Signed seconds use modulo-2^64
 * encoding. These offsets are unrelated to the entry's stable catalog ID.
 * The kind is FSIMG_HOST_FILE for a regular file and FSIMG_HOST_LINK for a
 * symbolic link, whose length is its target length.
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
 * Reads within the declared length are exact or fail; a request crossing it
 * is clipped there and reports the confirmed bytes. Past-EOF offsets fail.
 * Close releases the current source without validating it and is a no-op
 * when none is open; release also closes every source root.
 */
void fsimg_source_close(fsimg_writer_t *);
void fsimg_sources_release(fsimg_writer_t *);
/* Internal startup consumes an already-reserved root FD, a directory or a
 * regular file; scanning occurs here.
 */
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
/* fd is ignored (pass -1) when identity is a symbolic link, which has no
 * descriptor and is checked by identity alone.
 */
fsimg_status_t fsimg_source_check_object(fsimg_writer_t *, int,
    const fsimg_host_file_t *, const fsimg_source_root_t *, fsimg_error_t *);
/* Read the link parent/name into target, which holds
 * FSIMG_SYMLINK_TARGET_MAX + 1 bytes, without following it. identity
 * describes the link, with length replaced by the bytes read. A target that
 * is empty or longer than FSIMG_SYMLINK_TARGET_MAX fails as unsupported; a
 * link that changes while it is read fails as a changed source.
 */
fsimg_status_t fsimg_source_read_link(fsimg_writer_t *, int, const char *,
    fsimg_host_file_t *, unsigned char *, fsimg_error_t *);

#endif
