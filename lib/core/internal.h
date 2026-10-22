/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_INTERNAL_H
#define FSIMG_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fsimg.h"

/* The closed sets of public metadata fields and entry kinds. */
#define FSIMG_META_KNOWN	(FSIMG_META_CREATED | FSIMG_META_MODIFIED |	\
	FSIMG_META_ACCESSED | FSIMG_META_ATTRIBUTES | FSIMG_META_CHANGED |	\
	FSIMG_META_OWNER | FSIMG_META_MODE)
#define FSIMG_ENTRY_KNOWN	(FSIMG_ENTRY_FILE | FSIMG_ENTRY_DIRECTORY |	\
	FSIMG_ENTRY_SYMLINK | FSIMG_ENTRY_FIFO)
/* The entry model's bound on a symbolic-link target, chosen as the longest
 * any supported format stores. It sizes every target buffer and bounds the
 * host scan; each format's capability record states its own limit in
 * max_link_target_bytes, never above this one, which planning enforces.
 */
#define FSIMG_SYMLINK_TARGET_MAX 1024u
#define FSIMG_MODE_BITS		07777u

typedef struct fsimg_meta_cache fsimg_meta_cache_t;

/*
 * Private test controls and observations of hole knowledge. All zero is the
 * library's own behaviour; each switch disables one protection, so a test
 * can show that protection is what catches its case, or injects one host
 * failure.
 */
typedef struct fsimg_hole_test {
	uint64_t	 discovery_syncs;	/* Syncs made before host queries */
	int		 sync_errno;	/* Nonzero: discovery syncs fail so */
	int		 flush_errno;	/* Nonzero: descriptor flushes fail so */
	int		 resize_errno;	/* Nonzero: descriptor resizes fail so */
	bool		 no_length_check;	/* Answers ignore a shorter file */
	bool		 no_tail_clip;	/* Answers may cover the last byte */
	bool		 partial_chunks;	/* A partly covered chunk is zero */
	bool		 unpolled_zeros;	/* Answered chunks are not charged */
	/* Replaces the host's query, as fsimg_host_next_hole, when set. */
	int		(*host_query)(void *, int, uint64_t, uint64_t *,
			    uint64_t *, uint64_t *);
	/* Runs between the host query's two seeks, when set. */
	void		(*between_seeks)(void *);
	void		*argument;
} fsimg_hole_test_t;

struct fsimg_ctx {
	fsimg_options_t	 options;
	fsimg_stats_t	 stats;
	/* With a loss callback, the buffer its path is rendered into:
	 * loss_path_bytes, 4 * max_path_bytes + 1, holds the worst-case
	 * rendering of any path within the path limit.
	 */
	char		*loss_path;
	size_t		 loss_path_bytes;
	uint64_t	 allocation_attempts;
	uint64_t	 fail_allocation;
	fsimg_meta_cache_t *meta_cache;
	/* The owned scratch directory's identity, fixed while its descriptor
	 * options.scratch_directory_fd is held; valid when that is not -1.
	 * take_scratch() in context.c sets all three together.
	 */
	uint64_t	 scratch_device;
	uint64_t	 scratch_inode;
	uint64_t	 scratch_sequence;
	uint64_t	 meta_fault_after;
	uint32_t	 meta_cache_pages;
	unsigned	 meta_fault_point;
	int		 meta_fault_errno;
	/* Test control: XOR 0x01 into the disk byte at this offset minus one
	 * as a transcode reads it; zero disables.
	 */
	uint64_t	 transcode_flip;
	bool		 in_callback;
	bool		 no_hole_discovery;	/* Test control: read every hole */
	fsimg_hole_test_t hole_test;
	bool		 poison_allocations;	/* Test control: fill, not zero */
	bool		 unproven_writes;	/* Test control: see below */
	bool		 misplace_boot_header;	/* Test control: see below */
};

/* One session is embedded in each reader/writer; never reset per API call. */
typedef struct fsimg_session {
	fsimg_ctx_t	*ctx;
	fsimg_progress_t progress;
	uint64_t	 output_bytes;
	uint64_t	 output_limit;
} fsimg_session_t;

bool fsimg_add_u64(uint64_t, uint64_t, uint64_t *);
bool fsimg_mul_u64(uint64_t, uint64_t, uint64_t *);
bool fsimg_to_size(uint64_t, size_t *);
bool fsimg_range_fits(uint64_t, uint64_t, uint64_t);
void fsimg_error_clear(fsimg_error_t *);
fsimg_status_t fsimg_error_set(fsimg_error_t *, fsimg_status_t, int,
    fsimg_operation_t, const char *);
fsimg_status_t fsimg_limit_error(fsimg_error_t *, fsimg_resource_t,
    uint64_t, uint64_t);
fsimg_status_t fsimg_ctx_ready(fsimg_ctx_t *, fsimg_error_t *);
/* Consumes an already-reserved private scratch directory FD, without dup. */
fsimg_status_t fsimg_ctx_adopt_scratch(fsimg_ctx_t *, int, fsimg_error_t *);
fsimg_status_t fsimg_poll(fsimg_ctx_t *, const fsimg_progress_t *,
    fsimg_error_t *);
void *fsimg_alloc(fsimg_ctx_t *, size_t, fsimg_error_t *);
void fsimg_dealloc(void *);
fsimg_status_t fsimg_fd_reserve(fsimg_ctx_t *, fsimg_error_t *);
void fsimg_fd_release(fsimg_ctx_t *, int);
/*
 * Create name beneath parent as a fresh private directory, opened in *out
 * and counted against the descriptor limit; compat/private.h states the
 * rule. A taken name is FSIMG_EEXISTS, a directory the rule refuses is
 * FSIMG_EINVAL naming what the parent does to new directories, and a failed
 * host call is FSIMG_EIO. *created says whether a directory the library made
 * remains under name for the caller to remove or report.
 */
fsimg_status_t fsimg_private_mkdir(fsimg_ctx_t *, int, const char *,
    fsimg_operation_t, int *, bool *, fsimg_error_t *);
/* Internal files reserve a slot before open. Adoption consumes both the FD
 * and that reservation on every outcome, avoiding duplication in a worker.
 */
fsimg_status_t fsimg_io_adopt_fd(fsimg_ctx_t *, int, unsigned,
    fsimg_io_t **, fsimg_error_t *);
/* An object over a regular file's FD that stays with its owner, with
 * access checked as fsimg_io_from_fd checks it: nothing is duplicated,
 * adopted or reserved, and freeing the object leaves the FD open. The owner
 * keeps the FD open until the object and every slice of it are freed. A
 * worker reads or writes a descriptor it already holds this way, as its
 * confinement refuses duplication.
 */
fsimg_status_t fsimg_io_borrow_fd(fsimg_ctx_t *, int, unsigned, fsimg_io_t **,
    fsimg_error_t *);
/* Adopt a private scratch file as an unsized object: it accepts transfers
 * anywhere in the host range and grows as writes reach it, but an exact
 * read past its end fails. Its owner tracks the written extent and reads
 * only within it; size, slice and resize refuse the object.
 */
fsimg_status_t fsimg_io_adopt_scratch(fsimg_ctx_t *, int, fsimg_io_t **,
    fsimg_error_t *);
/* Bind a freshly constructed internal adapter to its operation's work quota. */
void fsimg_io_bind_session(fsimg_io_t *, fsimg_session_t *);
void fsimg_session_init(fsimg_session_t *, fsimg_ctx_t *, const char *,
    uint64_t);
fsimg_status_t fsimg_session_charge(fsimg_session_t *, uint64_t, uint64_t,
    uint64_t, fsimg_error_t *);
void fsimg_stat_add(uint64_t *, uint64_t);
/* fields is a validated FSIMG_META_* mask; path is a plain path, which the
 * callback receives rendered and whole. The context counts one loss per
 * field; the result is that count.
 */
uint64_t fsimg_report_loss(fsimg_ctx_t *, const char *, uint32_t);
/* Render a library path for emission into out, which holds size bytes and
 * its NUL, and return whether the rendering was truncated. It is the one
 * rendering of every path the library emits other than fsimg_entry_t.path.
 * omitted gives the class of each component of a walker path, the first
 * after the root at index 0; NULL renders a plain path, every component
 * delivered. Every backslash is written "\\" and, in an omitted component,
 * every byte outside 0x20 to 0x7e "\xNN" in lowercase hex; nothing else
 * changes, so the rendering is injective. A rendering too long for out
 * keeps its tail, which names the entry, and drops whole units from the
 * front: a byte, an escape or a UTF-8 sequence, never part of one.
 */
bool fsimg_path_render(const char *, const bool *, char *, size_t);
/* Where the tail of a rendered path of length bytes, at most limit bytes
 * long, starts: on a whole rendered unit, "\\\\", "\\xNN", a UTF-8 lead
 * byte with its continuation bytes, or one byte, which parse apart left to
 * right. Zero when the whole rendering fits.
 */
size_t fsimg_rendered_tail(const char *, size_t, size_t);
/* Name the object an error concerns: path, a plain path, rendered into the
 * error's path field, with path_truncated set when its tail was kept.
 */
void fsimg_error_path(fsimg_error_t *, const char *);
/* Private fault injection: zero disables; n fails the nth subsequent attempt. */
void fsimg_test_fail_alloc_after(fsimg_ctx_t *, uint64_t);
/* Private test control: whether a transcode consults the zero ranges its
 * disk reports (the default) or reads every chunk, which
 * must produce the same output.
 */
void fsimg_test_hole_discovery(fsimg_ctx_t *, bool);
/* The context's hole-knowledge test controls, which a test sets directly. */
fsimg_hole_test_t *fsimg_test_holes(fsimg_ctx_t *);
/* Private test control: fill each new allocation with 0xa5 instead of
 * zero, so that a byte its owner never writes shows.
 */
void fsimg_test_poison_allocations(fsimg_ctx_t *, bool);
/* Private test control: a transcode flips bit 0 of the disk byte at the
 * offset as it reads it, as if the disk changed or was staged corrupt;
 * UINT64_MAX disables.
 */
void fsimg_test_transcode_flip(fsimg_ctx_t *, uint64_t);
/* Private test control: a writer whose profile gates its modes on native
 * evidence, ifsboot's machine table, accepts every mode its tables can
 * describe, so a mode's plan and emission, which are the same code for
 * every machine, are tested on machines no evidence yet qualifies.
 */
void fsimg_test_unproven_writes(fsimg_ctx_t *, bool);
/* Private test control: the ifsboot encoder plants a header that ties at
 * offset 0 in front of the one it decodes before emission, the defect its
 * emission check exists to catch.
 */
void fsimg_test_misplace_boot_header(fsimg_ctx_t *, bool);

#endif
