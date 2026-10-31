/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
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
 *		the current entry: fill reader->entry except its path, set
 *		the driver's current locator and content state, and for a
 *		regular file set reader->initialized.
 * keys		derive the namespace keys of a record's name and payload;
 *		the name is the stored component, raw bytes for an omitted
 *		child.
 * key		derive the key of one spelling, used for selection queries;
 *		FSIMG_EINVAL when the format cannot represent it, which
 *		under a name profile includes every spelling outside it.
 *		Key buffers hold FSIMG_DIRECTORY_KEY_BYTES bytes.
 * ambiguous	the corruption message for two entries sharing a key.
 * name_profile	how preparation classes children by name.
 *
 * Content callback (specs/design-content.md). Common delivery owns reads and
 * range queries of the current file; the driver only resolves storage:
 * resolve	return one validated extent of the current file containing
 *		logical offset o, where o < reader->initialized. Delivery
 *		checks that it contains o and reads only the part of it
 *		inside reader->content_domain.
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
	fsimg_name_profile_t name_profile;
	fsimg_status_t (*resolve)(fsimg_reader_t *, uint64_t, fsimg_extent_t *,
	    fsimg_error_t *);
	fsimg_status_t (*verify)(fsimg_reader_t *, unsigned,
	    fsimg_verify_report_t *, fsimg_error_t *);
} fsimg_reader_ops_t;

/* What the walker does with entries omitted for their names. An omitted
 * subtree root, an omitted entry whose ancestors are all delivered, is
 * counted in count when the directory listing it is in scope. Without a
 * selection every directory is in scope, the whole volume. A selection
 * narrows the scope with fsimg_walk_scope_children: root_excluded takes the
 * root out of scope, and a descended directory inherits its parent's scope
 * unless the selection sets it. descend, set by full verification, walks
 * omitted directories, which delivery never does; refuse, set by
 * extraction without the skip option, fails at a counted root. The operation that starts the walk sets
 * descend, refuse and the root's scope before its first advance; the walker
 * in directory.c is the only writer of count.
 */
typedef struct fsimg_omission {
	uint64_t	 count;
	bool		 root_excluded;
	bool		 descend;
	bool		 refuse;
} fsimg_omission_t;

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
	/* The current file's initialized length: bytes below it come from
	 * storage, the rest of entry.size reads as zeros. The walker clears it
	 * before each replay, so a driver that does not set it exposes zeros,
	 * never backing storage, and bounds it by entry.size after.
	 */
	uint64_t	 initialized;
	/* The image range file storage can occupy, such as the FAT data
	 * region or the exFAT cluster heap, set by the driver's open. Content
	 * is never read outside it; until a driver sets it, it is empty. It
	 * also bounds every extent index the driver builds.
	 */
	fsimg_extent_domain_t content_domain;
	fsimg_reader_state_t state;
	fsimg_omission_t omission;
	fsimg_walk_t	*walk;
	char		*path;
	bool		 host_identity;
};

fsimg_status_t fsimg_reader_bytes(fsimg_reader_t *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_reader_stable(fsimg_reader_t *, fsimg_error_t *);
/* Account the walker's current entry in a verification report, the one
 * place a verifier counts entries: a delivered directory, or a delivered
 * file or other entry with its logical bytes. An entry that is not
 * delivered is walked and checked but not counted here.
 */
fsimg_status_t fsimg_verify_account(fsimg_reader_t *,
    fsimg_verify_report_t *, fsimg_error_t *);
/* Common content delivery for the current file, without the public entry
 * checks or terminal failure: payload verification reads through it.
 */
fsimg_status_t fsimg_reader_content(fsimg_reader_t *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
/* Payload verification of the current object: read its initialized bytes
 * [0, reader->initialized) in order through common content delivery, in
 * batches of at most capacity bytes into the borrowed buffer, adding every
 * confirmed byte to report->payload_bytes_read, before a failure too. The
 * zero tail and allocation slack are never read. The driver decides which
 * objects to read; the walker does not move. capacity must be nonzero.
 */
fsimg_status_t fsimg_verify_payload(fsimg_reader_t *, unsigned char *, size_t,
    fsimg_verify_report_t *, fsimg_error_t *);
fsimg_status_t fsimg_reader_abort(fsimg_reader_t *, fsimg_status_t);
fsimg_status_t fsimg_reader_end_scope(fsimg_reader_t *);
fsimg_status_t fsimg_reader_match_name(fsimg_reader_t *, const char *, bool *,
    fsimg_error_t *);
fsimg_status_t fsimg_reader_skip_directory(fsimg_reader_t *, fsimg_error_t *);

/* The common walker. next advances to the following entry in depth-first
 * order, or returns FSIMG_DONE; it is the only traversal, used by iteration,
 * selection and structural verification. Every admitted child becomes the
 * current entry, omitted ones included; delivered tells whether the current
 * entry and all its ancestors are delivered, which iteration filters on.
 * scope_children sets whether the omitted children of the next directory to
 * list are in scope: the current directory, which descends on the next
 * advance, or the root before the walk starts. free releases every prepared
 * directory and accepts a reader that never started.
 */
fsimg_status_t fsimg_walk_next(fsimg_reader_t *, fsimg_error_t *);
bool fsimg_walk_delivered(const fsimg_reader_t *);
void fsimg_walk_scope_children(fsimg_reader_t *, bool);
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
