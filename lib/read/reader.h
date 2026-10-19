/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_READER_PRIVATE_H
#define FSIMG_READER_PRIVATE_H

#include "directory.h"
#include "io.h"
#include "meta.h"

typedef struct fsimg_format_variant fsimg_format_variant_t;

/* A selected driver owns only its format state, the private driver pointer.
 * Common code owns the input, session, public snapshot, traversal and
 * lifecycle, including terminal failure cleanup. open receives the forced
 * public variant, or NULL after detection; the probe has already claimed the
 * image, but open still validates everything it uses. close accepts a
 * partially opened object and repeated calls after cleanup.
 *
 * Directory callbacks (specs/design-directories.md):
 * prepare	decode the root directory at level 0, otherwise the directory
 *		the current entry locates; claim its storage and admit every
 *		child with fsimg_directory_admit().
 * replay	re-validate a replayed record's name and payload and make it
 *		the current entry: fill reader->entry except its path and set
 *		the driver's current locator and content state.
 * keys		derive the namespace keys of a record's name and payload.
 * key		derive the key of one spelling, used for selection queries;
 *		FSIMG_EINVAL when the format cannot represent it.
 *		Key buffers hold FSIMG_DIRECTORY_KEY_BYTES bytes.
 * ambiguous	the corruption message for two entries sharing a key.
 */
typedef struct fsimg_reader_ops {
	fsimg_status_t (*open)(fsimg_reader_t *, const fsimg_format_variant_t *,
	    fsimg_error_t *);
	void (*close)(fsimg_reader_t *);
	fsimg_status_t (*prepare)(fsimg_reader_t *, fsimg_directory_build_t *,
	    fsimg_error_t *);
	fsimg_status_t (*replay)(fsimg_reader_t *, const char *,
	    const unsigned char *, size_t, fsimg_error_t *);
	fsimg_status_t (*keys)(fsimg_reader_t *, const char *,
	    const unsigned char *, size_t, fsimg_directory_keys_t *,
	    fsimg_error_t *);
	fsimg_status_t (*key)(fsimg_reader_t *, const char *, unsigned char *,
	    size_t *, fsimg_error_t *);
	const char	*ambiguous;
	fsimg_status_t (*read)(fsimg_reader_t *, uint64_t, void *, size_t,
	    size_t *, fsimg_error_t *);
	fsimg_status_t (*range)(fsimg_reader_t *, uint64_t, fsimg_range_t *,
	    fsimg_error_t *);
	fsimg_status_t (*verify)(fsimg_reader_t *, unsigned,
	    fsimg_verify_report_t *, fsimg_error_t *);
} fsimg_reader_ops_t;

typedef enum fsimg_reader_state {
	FSIMG_READER_NEW,
	FSIMG_READER_ITERATING,
	FSIMG_READER_DONE,
	FSIMG_READER_FAILED
} fsimg_reader_state_t;

struct fsimg_reader {
	fsimg_ctx_t	*ctx;
	fsimg_session_t	 session;
	fsimg_io_t	*input;
	const fsimg_reader_ops_t *ops;
	void		*driver;
	fsimg_host_file_t identity;
	fsimg_volume_info_t info;
	fsimg_entry_t	 entry;
	fsimg_reader_state_t state;
	fsimg_walk_t	*walk;
	char		*path;
	bool		 host_identity;
};

fsimg_status_t fsimg_reader_bytes(fsimg_reader_t *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_reader_stable(fsimg_reader_t *, fsimg_error_t *);
fsimg_status_t fsimg_reader_abort(fsimg_reader_t *, fsimg_status_t);
fsimg_status_t fsimg_reader_end_scope(fsimg_reader_t *);
fsimg_status_t fsimg_reader_match_name(fsimg_reader_t *, const char *, bool *,
    fsimg_error_t *);
fsimg_status_t fsimg_reader_skip_directory(fsimg_reader_t *, fsimg_error_t *);

/* The common walker. next advances to the following entry in depth-first
 * order, or returns FSIMG_DONE; it is the only traversal, used by iteration,
 * selection and structural verification. free releases every prepared
 * directory and accepts a reader that never started.
 */
fsimg_status_t fsimg_walk_next(fsimg_reader_t *, fsimg_error_t *);
fsimg_status_t fsimg_walk_match(fsimg_reader_t *, const char *, bool *,
    fsimg_error_t *);
fsimg_status_t fsimg_walk_skip(fsimg_reader_t *, fsimg_error_t *);
void fsimg_walk_free(fsimg_reader_t *);
/* Test hook: the scratch store and frame offset of the record the next
 * advance replays. Both are zero before traversal starts.
 */
void fsimg_test_walk_cursor(const fsimg_reader_t *, fsimg_store_t **,
    uint64_t *);

#endif
