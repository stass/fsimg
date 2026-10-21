/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_IO_PRIVATE_H
#define FSIMG_IO_PRIVATE_H

#include <time.h>

#include "host.h"
#include "internal.h"

fsimg_ctx_t *fsimg_io_context(const fsimg_io_t *);
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
fsimg_status_t fsimg_io_set_times(fsimg_io_t *, const struct timespec *,
    fsimg_error_t *);

#endif
