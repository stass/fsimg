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
 *		regular file set reader->initialized; for a symbolic link
 *		whose target the format encodes, declare the decoded target
 *		with fsimg_reader_set_link_target() instead.
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
 * prepare and replay report a decoder's rejection of the image as FSIMG_ECORRUPT
 * or FSIMG_EUNSUPPORTED under FSIMG_OP_READER, and no storage, I/O or limit failure
 * that way: salvage extraction steps over exactly those, so more replays and
 * prepares can follow one, with the claims already made still in effect.
 *
 * Content callback (specs/design-content.md). Common delivery owns reads and
 * range queries of the current file; the driver only resolves storage:
 * resolve	return one validated extent of the current file containing
 *		logical offset o, where o < reader->initialized. Delivery
 *		checks that it contains o and reads only the part of it
 *		inside reader->content_domain, or copies it from reader->held
 *		when resolve set that. It is never called for a decoded link
 *		target.
 *
 * open reads its decoding options, such as a view, from reader->options.
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
 * extraction without the skip option, fails at a counted root. The
 * operation that starts the walk sets descend, refuse and the root's scope
 * before its first advance; the walker in directory.c is the only writer
 * of count. uncounted, set by fsimg_walk_restart for every walk of full
 * verification after the first, takes the whole walk out of scope and
 * makes fsimg_verify_account count nothing.
 */
typedef struct fsimg_omission {
	uint64_t	 count;
	bool		 root_excluded;
	bool		 descend;
	bool		 refuse;
	bool		 uncounted;
} fsimg_omission_t;

/* Salvage's side of the walk, set by extraction before its first advance.
 * While enabled, the walker steps over a listing or entry the driver's decoder
 * rejects (specs/design-salvage.md); losses counts what was noticed. bound is
 * the logical offset at which the current file's allocation broke, for a
 * driver that finds that at replay; the walker sets it to UINT64_MAX before
 * each replay. It reaches past the initialized length, where no lookup looks.
 */
typedef struct fsimg_salvage {
	uint64_t	 losses;
	uint64_t	 bound;
	bool		 enabled;
} fsimg_salvage_t;

/* A linked cluster allocation for fsimg_reader_salvage_index(). link reports the
 * cluster after cluster, 0 at the chain's end, and rejects a damaged link like
 * replay rejects an entry. clusters is the volume's count, and wanted how many
 * the file's size calls for. Cluster 2 starts at heap_offset, and first is
 * already known valid.
 */
typedef struct fsimg_salvage_chain {
	fsimg_status_t (*link)(fsimg_reader_t *, uint32_t, uint32_t *,
	    fsimg_error_t *);
	uint64_t	 heap_offset;
	uint64_t	 wanted;
	uint32_t	 first;
	uint32_t	 clusters;
	uint32_t	 cluster_bytes;
} fsimg_salvage_chain_t;

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
	uint64_t	 slice_bytes;	/* the input's length, fixed at open */
	const fsimg_reader_ops_t *ops;
	void		*driver;
	fsimg_host_file_t identity;
	/* The caller's decoding options, copied at open for the driver's
	 * open to read, such as the view a format with several presents.
	 * Common code has checked each setting's form and that a non-default
	 * one applies to the format. Every borrowed pointer is cleared after
	 * the copy, format among them: the name is borrowed for detection
	 * only.
	 */
	fsimg_read_options_t options;
	/* The first identity check a forced open waived although it failed,
	 * a static phrase such as "FAT[0] does not repeat the media byte", or
	 * NULL. Only fsimg_reader_identity() sets it, so fsimg_verify(),
	 * which refuses the volume while it is set, sees every waived check.
	 */
	const char	*assumed_identity;
	fsimg_volume_info_t info;
	fsimg_entry_t	 entry;
	/* The current file's initialized length: bytes below it come from
	 * storage, the rest of entry.size reads as zeros. The walker clears it
	 * before each replay, so a driver that does not set it exposes zeros,
	 * never backing storage, and bounds it by entry.size after.
	 */
	uint64_t	 initialized;
	/* A symbolic link's target the driver decoded at replay, declared
	 * with fsimg_reader_set_link_target(): link_length bytes in
	 * link_target, one buffer of FSIMG_SYMLINK_TARGET_MAX bytes allocated
	 * on the first declaration and kept until the reader is freed or
	 * fails. The walker clears link_length before each replay, as it
	 * clears initialized; while it is nonzero, delivery serves the
	 * current entry from the buffer instead of resolving extents.
	 */
	unsigned char	*link_target;
	size_t		 link_length;
	/* Set by resolve when the extent it returns is held rather than mapped:
	 * its length bytes are here, in a charged buffer the driver owns, valid
	 * until the next resolve or the walker's advance, and its image_offset
	 * means nothing. fsimg_reader_resolve() takes it and clears it as resolve
	 * returns, so it is NULL outside that call.
	 */
	const unsigned char *held;
	/* The image range file storage can occupy, such as the FAT data
	 * region or the exFAT cluster heap, set by the driver's open. Content
	 * is never read outside it; until a driver sets it, it is empty. It
	 * also bounds every extent index the driver builds.
	 */
	fsimg_extent_domain_t content_domain;
	fsimg_reader_state_t state;
	fsimg_omission_t omission;
	fsimg_salvage_t	 salvage;
	fsimg_walk_t	*walk;
	char		*path;
	bool		 host_identity;
};

/* Count one noticed salvage loss of kind, a FSIMG_LOSS_* value, for the reader
 * and its context, and report the first FSIMG_LOSS_EXAMPLES to the loss
 * callback. path and omitted are as fsimg_path_render() takes them.
 */
void fsimg_reader_salvage_loss(fsimg_reader_t *, const char *, const bool *,
    uint32_t);
/* Whether salvage steps over this failure of a driver callback: the driver's
 * decoder rejecting the image (specs/design-salvage.md#failure-boundary-and-limits).
 * Without salvage failure may be NULL, and is not read.
 */
bool fsimg_reader_decode_rejected(const fsimg_reader_t *, const fsimg_error_t *,
    fsimg_status_t);
/* Plan the current regular file under salvage, before anything is created for
 * it. Resolving its extents from the first byte finds the first one that is
 * unavailable: a lookup the driver rejects, the end of the content domain or
 * the end of the slice, whichever comes first, byte for byte. Damage shortens
 * the current entry to that prefix and sets *damaged, so the charge, the output
 * size and every read that follow see only the prefix. Any other failure is
 * terminal and aborts the reader. A format supplies only resolve, whose lookup
 * past a stopped extent index already rejects, and a replay that does not reject
 * damage inside the allocation.
 */
fsimg_status_t fsimg_reader_salvage_plan(fsimg_reader_t *, bool *,
    fsimg_error_t *);
/* Finish a walk of the current regular file's allocation: a walk whose decoder
 * rejected the image keeps the index it built before the damage, stopped so that
 * a lookup past it rejects too, and reports FSIMG_OK. Without salvage, for any
 * other entry, for a walk with no index yet and for any other failure, the
 * status comes back unchanged and the caller frees the index. A directory's walk
 * has a directory or nothing current, so a bad directory stays rejected whole.
 * An index that already covers the file's size leaves nothing to shorten: damage
 * past the size is not damage to the file.
 */
fsimg_status_t fsimg_reader_salvage_keep(fsimg_reader_t *,
    fsimg_extent_index_t *, fsimg_status_t, const fsimg_error_t *);
/* Index a linked allocation for fsimg_reader_salvage_plan() at replay. The index
 * covers the clusters before the first damage, each once: the chain's end, a
 * link the driver rejects, or the first repeat of a cycle. A chain cut short sets
 * salvage.bound, and one that reaches past the slice stops the index. A chain
 * longer than wanted is not damage to this file. Wanted zero leaves *out NULL.
 * Every failure returned is terminal.
 */
fsimg_status_t fsimg_reader_salvage_index(fsimg_reader_t *,
    const fsimg_salvage_chain_t *, fsimg_extent_index_t **, fsimg_error_t *);
fsimg_status_t fsimg_reader_bytes(fsimg_reader_t *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
/* The one call of the driver's resolve: the extent containing offset, checked to
 * contain it. *held is what reader->held was set to, NULL for a mapped extent,
 * and is taken from the reader even when resolve fails, so no later resolve can
 * see it.
 */
fsimg_status_t fsimg_reader_resolve(fsimg_reader_t *, uint64_t, fsimg_extent_t *,
    const unsigned char **, fsimg_error_t *);
/* Declare the current entry's content a symbolic-link target the driver
 * decoded at replay from an encoding, such as a sequence of path
 * components, rather than stored verbatim (specs/design-content.md
 * #decoded-link-targets). replay calls it after setting entry.kind to
 * FSIMG_ENTRY_SYMLINK, with a length from 1 through
 * FSIMG_SYMLINK_TARGET_MAX, otherwise FSIMG_ESTATE. It copies the bytes
 * into the reader's target buffer and sets entry.size and initialized to
 * length; delivery then serves them under the same bounds, stability
 * check and work charging as stored content, and never calls resolve for
 * the entry. A driver that stores targets verbatim never calls it.
 */
fsimg_status_t fsimg_reader_set_link_target(fsimg_reader_t *,
    const void *, size_t, fsimg_error_t *);
fsimg_status_t fsimg_reader_stable(fsimg_reader_t *, fsimg_error_t *);
/* Charge the current file's size to the output budget before it is copied: its
 * declared size, or under salvage the prefix planning left it. A failure can
 * leave the reader aborted, so the copy must not start.
 */
fsimg_status_t fsimg_reader_charge_file(fsimg_reader_t *, fsimg_error_t *);
/* The one way a driver's open treats an identity check, the evidence that
 * the bytes are its format: when holds is false it fails with
 * FSIMG_ECORRUPT and the refusal message, unless the caller assumes the
 * identity (FSIMG_READ_ASSUME_IDENTITY), when it records the phrase in
 * assumed_identity for full verification to refuse and returns FSIMG_OK.
 * A validation check never goes through it.
 */
fsimg_status_t fsimg_reader_identity(fsimg_reader_t *, bool, const char *,
    const char *, fsimg_error_t *);
/* Account the walker's current entry in a verification report, the one
 * place a verifier counts entries: a delivered directory, or a delivered
 * file or other entry with its logical bytes. An entry that is not
 * delivered, or any entry of an uncounted walk, is walked and checked but
 * not counted here.
 */
fsimg_status_t fsimg_verify_account(fsimg_reader_t *,
    fsimg_verify_report_t *, fsimg_error_t *);
/* Common content delivery for the current file, without the public entry
 * checks or terminal failure: payload verification reads through it. It
 * reports the bytes transferred and, among them, the bytes stored: read
 * from the image rather than synthesized or decoded.
 */
fsimg_status_t fsimg_reader_content(fsimg_reader_t *, uint64_t, void *, size_t,
    size_t *, size_t *, fsimg_error_t *);
/* Payload verification of the current object: read its initialized bytes
 * [0, reader->initialized) in order through common content delivery, in
 * batches of at most capacity bytes into the borrowed buffer, adding every
 * confirmed stored byte to report->payload_bytes_read, before a failure
 * too; a decoded link target's bytes are checked but not stored. The
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
/* Several walks (specs/design-directories.md#several-walks): once an
 * advance of full verification has returned FSIMG_DONE, release the
 * finished walker so the next advance walks another tree of the volume.
 * The driver selects that tree's root in its own state first, which its
 * prepare decodes at level 0; every directory is then prepared, checked
 * and validated again under the same limits and the same session budget.
 * The first walk is the counted one, whose entries the report counts;
 * every restarted walk is uncounted: it still descends into and checks
 * omitted subtrees, but counts none of them and accounts no entry.
 * FSIMG_ESTATE outside full verification or before the walk has finished.
 */
fsimg_status_t fsimg_walk_restart(fsimg_reader_t *, fsimg_error_t *);
void fsimg_walk_free(fsimg_reader_t *);
/* Test hook: the walker's record stream, or NULL without one. */
fsimg_stream_t *fsimg_test_walk_records(const fsimg_reader_t *);
/* Test hook: the scratch store and frame offset of the record the next
 * advance replays. Both are zero before traversal starts.
 */
void fsimg_test_walk_cursor(const fsimg_reader_t *, fsimg_store_t **,
    uint64_t *);

#endif
