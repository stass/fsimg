/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_IO_PRIVATE_H
#define FSIMG_IO_PRIVATE_H

#include <time.h>

#include "host.h"
#include "internal.h"

fsimg_ctx_t *fsimg_io_context(const fsimg_io_t *);
/*
 * A read-only object of a fixed length whose bytes an internal owner
 * computes, such as a container's disk. resolve fills exactly the length
 * requested at an offset the object's bounds already checked, never more
 * than the object's maximum transfer; it returns FSIMG_OK only when every
 * byte is filled and otherwise sets the confirmed count. next_zero answers
 * fsimg_io_next_zero_range for an offset below the length, in the
 * object's own coordinates. All three operations are required, and all
 * run as library code, not as a caller's
 * callback. The object carries backing's identity, so alias and stability
 * checks see the file beneath it; backing must outlive it. It adopts
 * argument on every outcome and calls release on it when it is freed.
 */
typedef struct fsimg_io_resolver_ops {
	fsimg_status_t	(*resolve)(void *, uint64_t, void *, size_t, size_t *,
			    fsimg_error_t *);
	fsimg_status_t	(*next_zero)(void *, uint64_t, uint64_t *, uint64_t *,
			    fsimg_error_t *);
	void		(*release)(void *);
} fsimg_io_resolver_ops_t;
fsimg_status_t fsimg_io_resolver(fsimg_ctx_t *, fsimg_io_t *, uint64_t,
    const fsimg_io_resolver_ops_t *, void *, fsimg_io_t **, fsimg_error_t *);
/* A slice that refuses writes whatever the parent's access, for handing
 * one partition of a disk to a reader.
 */
fsimg_status_t fsimg_io_read_slice(fsimg_io_t *, uint64_t, uint64_t,
    fsimg_io_t **, fsimg_error_t *);
/* A per-call session never attaches borrowed input to a reader's lifetime.
 * read_session transfers exactly the length or fails; read_at_session, as
 * fsimg_io_read_at, may transfer less, and transfers zero only at the end.
 * Both charge every transfer to the session, whatever session the object
 * has bound.
 */
fsimg_status_t fsimg_io_read_session(fsimg_io_t *, fsimg_session_t *,
    uint64_t, void *, size_t, size_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_read_at_session(fsimg_io_t *, fsimg_session_t *,
    uint64_t, void *, size_t, size_t *, fsimg_error_t *);
/* Callback storage has no host identity; its stability is the caller's duty. */
fsimg_status_t fsimg_io_identity(fsimg_io_t *, fsimg_host_file_t *, bool *,
    fsimg_error_t *);
/* Whether a host file's identity from fsimg_io_identity still has the
 * length the object's root was made at, so that the root's bounds cover
 * the whole file.
 */
bool fsimg_io_length_current(const fsimg_io_t *, const fsimg_host_file_t *);
/* Capability/length validation only: no resize, zeroing, or write occurs. */
fsimg_status_t fsimg_io_check_output(fsimg_io_t *, uint64_t, bool, bool *,
    fsimg_error_t *);
fsimg_status_t fsimg_io_write_session(fsimg_io_t *, fsimg_session_t *,
    uint64_t, const void *, size_t, size_t *, fsimg_error_t *);
/* Consumes an owned whole FD adapter and reports its final close outcome;
 * a borrowing object is freed and refused, its FD left open.
 */
fsimg_status_t fsimg_io_close_output(fsimg_io_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_zero_session(fsimg_io_t *, fsimg_session_t *,
    uint64_t, uint64_t, fsimg_error_t *);
/* Takes exclusive ownership of a just-created empty regular file, a fresh
 * file whose written-range record proves every range not written through it
 * zero. Never use for a descriptor with another writer or an existing pathname
 * outside private staging.
 */
fsimg_status_t fsimg_io_adopt_empty_output(fsimg_ctx_t *, int, fsimg_io_t **,
    fsimg_error_t *);
/* Adopt a handed-over file, the spool or the raw intermediate, on every
 * outcome: only an empty, unlinked, read/write regular file without
 * O_APPEND is accepted, as a fresh file; anything else is FSIMG_EINVAL
 * naming role in error.path.
 */
fsimg_status_t fsimg_io_adopt_handed_over(fsimg_ctx_t *, int, const char *,
    fsimg_io_t **, fsimg_error_t *);
/*
 * The next range at or after offset (at most the object's length) that the
 * object knows reads as zero, as [*start, *end) with
 * offset <= *start <= *end <= length. An empty answer says no zero range
 * the object knows of begins before *start, where the caller resumes; each
 * answer makes progress, and an object that knows nothing answers
 * *start == *end == length. A fresh
 * file answers from its record, a slice through its root, a resolver and
 * a callback object through their own operations, and a read-only file
 * fsimg did not write from the host's report after one sync, never
 * covering its last byte; a writable one and scratch know nothing.
 */
fsimg_status_t fsimg_io_next_zero_range(fsimg_io_t *, uint64_t, uint64_t *,
    uint64_t *, fsimg_error_t *);
/* The bytes the host allocated to a descriptor object's file, which the
 * spool reports; fsimg's own descriptors never leave this module.
 */
fsimg_status_t fsimg_io_allocated_bytes(fsimg_io_t *, uint64_t *,
    fsimg_error_t *);
/* Private test observation: a fresh root's granule shift and record bytes. */
void fsimg_test_io_record(const fsimg_io_t *, unsigned *, size_t *);
fsimg_status_t fsimg_io_set_times(fsimg_io_t *, const struct timespec *,
    fsimg_error_t *);

#endif
