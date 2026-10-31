/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_MTREE_WRITER_H
#define FSIMG_MTREE_WRITER_H

#include "internal.h"
#include "sha256.h"

/* The mtree(5) manifest the extractor writes beside the tree it extracts.
 * It starts with the "#mtree v2.0" signature and the image root's
 * ". type=dir" line, then holds one full-form line per entry, in the order
 * the extractor visits them:
 *
 *	./path type=KIND [mode=0NNN] [uid=N gid=N] [time=S.NNNNNNNNN]
 *	    [size=N sha256digest=HEX] [link=TARGET]
 *
 * A path and a link target are written with \ooo for every byte outside
 * 0x21 through 0x7E and for '\' and '#', so each line is plain ASCII and
 * the manifest parser decodes exactly the bytes the image holds. Only
 * standard keywords appear, so other mtree readers read it too.
 *
 * The writer holds one fixed buffer and the byte count already written; it
 * keeps nothing per entry. Its lines reach the manifest file through the
 * caller's descriptor, borrowed and never duplicated, since a confined
 * worker cannot duplicate one; every write is charged as work to the
 * session given at open, which also polls cancellation.
 */

typedef struct fsimg_mtree_writer fsimg_mtree_writer_t;

/* One manifest line. entry supplies the path, kind, owner and mode;
 * modified_valid says whether the line carries a time, and
 * modified_seconds and modified_nanoseconds hold its UTC instant. digest
 * is a file's SHA-256 and NULL for any other kind or a file whose bytes
 * were not copied; a line with a digest also carries the entry's size.
 * link is a symbolic link's target of link_length bytes, NULL otherwise.
 */
typedef struct fsimg_mtree_record {
	const fsimg_entry_t *entry;
	const unsigned char *digest;
	const unsigned char *link;
	size_t		 link_length;
	int64_t		 modified_seconds;
	uint32_t	 modified_nanoseconds;
	bool		 modified_valid;
} fsimg_mtree_record_t;

/* Check that fd is a manifest output: a regular file opened for writing,
 * without O_APPEND, and empty. It changes nothing.
 */
fsimg_status_t fsimg_mtree_writer_check(int, fsimg_error_t *);

/* Start a manifest on fd, which stays the caller's and open until the
 * writer is finished or freed. The signature and root lines are buffered,
 * not yet written.
 */
fsimg_status_t fsimg_mtree_writer_open(fsimg_ctx_t *, fsimg_session_t *, int,
    fsimg_mtree_writer_t **, fsimg_error_t *);

/* Start a manifest on output, which the writer owns from this call on,
 * whatever it returns. fsimg_mtree_writer_open() starts one on its
 * descriptor's borrowed I/O object; tests start one on an I/O object that
 * fails on demand.
 */
fsimg_status_t fsimg_mtree_writer_start(fsimg_ctx_t *, fsimg_session_t *,
    fsimg_io_t *, fsimg_mtree_writer_t **, fsimg_error_t *);

/* Append the line for one entry. A failure leaves the manifest incomplete;
 * the caller frees the writer and publishes nothing.
 */
fsimg_status_t fsimg_mtree_writer_add(fsimg_mtree_writer_t *,
    const fsimg_mtree_record_t *, fsimg_error_t *);

/* Write what is buffered and flush the file durably. The writer is
 * consumed either way; closing the descriptor stays the caller's.
 */
fsimg_status_t fsimg_mtree_writer_finish(fsimg_mtree_writer_t *,
    fsimg_error_t *);

/* Release an unfinished writer; NULL is ignored. */
void fsimg_mtree_writer_free(fsimg_mtree_writer_t *);

#endif
