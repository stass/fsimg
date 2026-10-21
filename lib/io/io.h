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
 * byte is filled and otherwise sets the confirmed count. It runs as
 * library code, not as a caller's callback. The object carries backing's
 * identity, so alias and stability checks see the file beneath it; backing
 * must outlive it. It adopts argument on every outcome and calls release
 * on it when it is freed.
 */
typedef fsimg_status_t (*fsimg_io_resolve_t)(void *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_resolver(fsimg_ctx_t *, fsimg_io_t *, uint64_t,
    fsimg_io_resolve_t, void (*)(void *), void *, fsimg_io_t **,
    fsimg_error_t *);
/* A slice that refuses writes whatever the parent's access, for handing
 * one partition of a disk to a reader.
 */
fsimg_status_t fsimg_io_read_slice(fsimg_io_t *, uint64_t, uint64_t,
    fsimg_io_t **, fsimg_error_t *);
/* A per-call session never attaches borrowed input to a reader's lifetime. */
fsimg_status_t fsimg_io_read_session(fsimg_io_t *, fsimg_session_t *,
    uint64_t, void *, size_t, size_t *, fsimg_error_t *);
/* Callback storage has no host identity; its stability is the caller's duty. */
fsimg_status_t fsimg_io_identity(fsimg_io_t *, fsimg_host_file_t *, bool *,
    fsimg_error_t *);
/* Capability/length validation only: no resize, zeroing, or write occurs. */
fsimg_status_t fsimg_io_check_output(fsimg_io_t *, uint64_t, bool, bool *,
    fsimg_error_t *);
fsimg_status_t fsimg_io_write_session(fsimg_io_t *, fsimg_session_t *,
    uint64_t, const void *, size_t, size_t *, fsimg_error_t *);
/* Consumes an owned whole FD adapter and reports its final close outcome. */
fsimg_status_t fsimg_io_close_output(fsimg_io_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_zero_session(fsimg_io_t *, fsimg_session_t *,
    uint64_t, uint64_t, fsimg_error_t *);
/* Takes exclusive ownership of a just-created empty regular file. Its initial
 * bytes after resize are proven zero until the first write. Never use for a
 * descriptor with another writer or an existing pathname outside private staging.
 */
fsimg_status_t fsimg_io_adopt_empty_output(fsimg_ctx_t *, int, fsimg_io_t **,
    fsimg_error_t *);
/* Whether every byte of the object's backing is proven zero: such a fresh
 * file before any write has been attempted on it. A sequential writer that
 * asks once, before its first write, may leave a zero range unwritten,
 * since it never writes that range afterwards.
 */
bool fsimg_io_proven_zero(const fsimg_io_t *);
/* Adopt a handed-over file, the spool or the raw intermediate, on every
 * outcome: only an empty, unlinked, read/write regular file without
 * O_APPEND is accepted, as a proven-zero object; anything else is
 * FSIMG_EINVAL naming role in error.path.
 */
fsimg_status_t fsimg_io_adopt_handed_over(fsimg_ctx_t *, int, const char *,
    fsimg_io_t **, fsimg_error_t *);
/* The first hole the host reports at or after offset of a file-backed
 * object, as [hole_start, hole_end) within the object; an object or host
 * that reports none gives hole_start == hole_end == its length.
 */
void fsimg_io_next_hole(fsimg_io_t *, uint64_t, uint64_t *, uint64_t *);
fsimg_status_t fsimg_io_set_times(fsimg_io_t *, const struct timespec *,
    fsimg_error_t *);

#endif
