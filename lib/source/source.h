/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_SOURCE_H
#define FSIMG_SOURCE_H

#include "host.h"
#include "sha256.h"
#include "writer.h"

#define FSIMG_SOURCE_HEADER 120u
#define FSIMG_SOURCE_DIGEST 88u

/* Host records contain eleven little-endian u64 fields: root ID, device,
 * inode, length, mtime seconds/nanoseconds, ctime seconds/nanoseconds, kind,
 * version and digest flag; then, at FSIMG_SOURCE_DIGEST, the SHA-256 the
 * file's bytes must have when the flag is one, else zero bytes; then a
 * NUL-terminated relative path, empty exactly when the root is the regular
 * file itself. Signed seconds use modulo-2^64 encoding. These offsets are
 * unrelated to the entry's stable catalog ID. The kind is FSIMG_HOST_FILE
 * for a regular file and FSIMG_HOST_LINK for a symbolic link, whose length
 * is its target length; only a file a manifest describes carries a digest.
 */
_Static_assert(FSIMG_SOURCE_DIGEST + FSIMG_SHA256_DIGEST_LENGTH ==
    FSIMG_SOURCE_HEADER, "the digest ends the host record header");

/* A root holds an opened directory or regular file, or no descriptor at
 * all (fd -1): a manifest root whose entries need no host bytes, which
 * consumes no descriptor reservation and which no host record names.
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
/* fsimg_writer_add_manifest() over descriptors it consumes: an already
 * reserved manifest file and an already reserved content directory or -1,
 * each consumed with its reservation on every outcome, so a confined
 * worker never duplicates a descriptor.
 */
fsimg_status_t fsimg_writer_adopt_manifest(fsimg_writer_t *, int, int,
    fsimg_error_t *);
fsimg_status_t fsimg_source_open(fsimg_writer_t *, uint64_t, fsimg_error_t *);
fsimg_status_t fsimg_source_read(fsimg_writer_t *, uint64_t, void *,
    size_t, size_t *, fsimg_error_t *);
fsimg_status_t fsimg_source_range(fsimg_writer_t *, uint64_t,
    fsimg_range_t *, fsimg_error_t *);
fsimg_status_t fsimg_source_finish(fsimg_writer_t *, fsimg_error_t *);
fsimg_status_t fsimg_source_scan(fsimg_writer_t *, fsimg_source_root_t *,
    const char *, fsimg_error_t *);
/* Append a host record for identity at the relative path beneath root,
 * carrying digest when it is not NULL, and store its stream offset in
 * *reference. The record is built in writer->source_record; the path may
 * already lie there. A path beyond the context's path limit fails.
 */
fsimg_status_t fsimg_source_append_host(fsimg_writer_t *,
    const fsimg_source_root_t *, const fsimg_host_file_t *,
    const unsigned char *, const char *, uint64_t *, fsimg_error_t *);
/* Check every sha256digest a manifest claimed for generated content
 * (manifest.c), generating each file's bytes once, one unit of work per
 * piece, and release the claims. Planning calls it after the image has
 * accepted every size. A mismatch is FSIMG_EINVAL; it, and any other
 * failure while a claim is checked, names the claim's line and image path
 * as the parser names a refused line.
 */
fsimg_status_t fsimg_manifest_check_digests(fsimg_writer_t *,
    fsimg_error_t *);
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

/* Why a relative path is walked beneath a root, which decides how a failed
 * open is reported. REOPEN follows a recorded path, so an object that is
 * missing or has become a link is a changed source. IMPORT follows a path
 * a manifest names, so a link is unsupported, and a missing object or a
 * name too long for its file system is invalid input.
 */
typedef enum fsimg_source_walk {
	FSIMG_WALK_REOPEN,
	FSIMG_WALK_IMPORT
} fsimg_source_walk_t;

/* Open every directory component of path beneath the directory root, one
 * at a time, each as fsimg_source_open_child opens a directory. On success
 * *parent is the last directory, root->fd itself when path has a single
 * component, and *name points at the final component within path; the
 * caller releases *parent unless it is root->fd. path is split in place
 * while a component is opened and is unchanged on every return; an empty,
 * "." or ".." component fails.
 */
fsimg_status_t fsimg_source_walk(fsimg_writer_t *, const fsimg_source_root_t *,
    char *, fsimg_source_walk_t, int *, char **, fsimg_error_t *);
/* Open parent/name without following a link, with O_DIRECTORY when
 * directory is true, inspect it, and check it for the root's device and
 * mount and against the output and scratch directories. An import walk
 * opens a final name only if it is a regular file when inspected. On
 * success *fd is a reserved descriptor the caller releases and *identity
 * describes it.
 */
fsimg_status_t fsimg_source_open_child(fsimg_writer_t *,
    const fsimg_source_root_t *, int, const char *, bool, fsimg_source_walk_t,
    int *, fsimg_host_file_t *, fsimg_error_t *);

#endif
