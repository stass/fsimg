/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_ZLIB_MEMORY_H
#define FSIMG_ZLIB_MEMORY_H

#include <zlib.h>

#include "internal.h"

/*
 * The context-backed zlib allocator (specs/design-binary-primitives.md):
 * one adapter per z_stream charges every zlib allocation to a borrowed
 * context and keeps the first allocation failure for the stream's whole
 * life. The stream's owner embeds both at stable addresses and keeps the
 * stream's lifecycle, framing and result interpretation.
 */
typedef struct fsimg_zlib_memory {
	fsimg_ctx_t	*ctx;
	fsimg_error_t	 error;
} fsimg_zlib_memory_t;

/* Bind a fresh, zeroed stream with no zlib state to the adapter: install
 * both callbacks and the adapter as opaque. Nothing is allocated.
 */
void fsimg_zlib_memory_bind(fsimg_zlib_memory_t *, fsimg_ctx_t *, z_stream *);
/* The cause of a Z_MEM_ERROR: the first recorded allocation failure, or
 * FSIMG_ENOMEM when zlib reported one without the callback failing.
 */
fsimg_status_t fsimg_zlib_memory_error(const fsimg_zlib_memory_t *,
    fsimg_error_t *);

#endif
