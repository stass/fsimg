/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_ZLIB_MEMORY_H
#define FSIMG_ZLIB_MEMORY_H

#include <zlib.h>

#include "internal.h"

/*
 * zlib's allocations for one stream, charged to a borrowed context like
 * every other allocation. The adapter records the first allocation failure
 * and keeps it for the stream's whole life: no later allocation, free,
 * reset or end clears it, and every allocation after it fails at once.
 * An allocation failure is terminal for its stream; fresh use takes a
 * fresh stream and adapter.
 *
 * The stream's owner keeps the adapter at a stable address beside the
 * stream, for as long as zlib may call it: from bind through the stream's
 * End. The context outlives both. Neither is copied, moved or rebound
 * while the stream is live; separate streams use separate adapters.
 */
typedef struct fsimg_zlib_memory {
	fsimg_ctx_t	*ctx;
	fsimg_error_t	 error;	/* The first allocation failure, or none */
} fsimg_zlib_memory_t;

/*
 * Bind a fresh, zero-initialized stream with no zlib state to the adapter:
 * set zalloc, zfree and opaque together, and clear the adapter's error.
 * The stream's other fields are the owner's; nothing is allocated.
 */
void fsimg_zlib_memory_bind(fsimg_zlib_memory_t *, fsimg_ctx_t *, z_stream *);

/*
 * The cause of a Z_MEM_ERROR: the recorded first failure with its complete
 * diagnostic, or FSIMG_ENOMEM when zlib reported one without an allocation
 * failing through the adapter. Only Z_MEM_ERROR is interpreted this way;
 * the recorded error is left in place.
 */
fsimg_status_t fsimg_zlib_memory_error(const fsimg_zlib_memory_t *,
    fsimg_error_t *);

#endif /* FSIMG_ZLIB_MEMORY_H */
