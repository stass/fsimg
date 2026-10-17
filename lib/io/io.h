/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_IO_PRIVATE_H
#define FSIMG_IO_PRIVATE_H

#include "host.h"
#include "internal.h"

fsimg_ctx_t *fsimg_io_context(const fsimg_io_t *);
/* A per-call session never attaches borrowed input to a reader's lifetime. */
fsimg_status_t fsimg_io_read_session(fsimg_io_t *, fsimg_session_t *,
    uint64_t, void *, size_t, size_t *, fsimg_error_t *);
/* Callback storage has no host identity; its stability is the caller's duty. */
fsimg_status_t fsimg_io_identity(fsimg_io_t *, fsimg_host_file_t *, bool *,
    fsimg_error_t *);

#endif
