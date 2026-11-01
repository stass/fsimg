/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_CODEC_MEMORY_H
#define FSIMG_CODEC_MEMORY_H

#include <stddef.h>

#include <zlib.h>

#include "internal.h"

/*
 * A codec library's allocations for one stream, charged to a borrowed
 * context like every other allocation.
 * The adapter records the first allocation failure and keeps it for the
 * stream's whole life: no later allocation, free, reset or end clears it,
 * and every allocation after it fails at once. An allocation failure is
 * terminal for its stream; fresh use takes a fresh stream and adapter.
 *
 * The state is library neutral. A binding is the pair of callbacks one
 * library calls, shims over fsimg_codec_memory_alloc and
 * fsimg_codec_memory_free, installed together with the adapter as their
 * opaque pointer: zlib's below, liblzma's and libzstd's in the layer file
 * that includes that library's header.
 *
 * The stream's owner keeps the adapter at a stable address beside the
 * stream, for as long as the library may call it: from bind through the
 * stream's end. The context outlives both. Neither is copied, moved or
 * rebound while the stream is live; separate streams use separate adapters.
 */
typedef struct fsimg_codec_memory {
	fsimg_ctx_t	*ctx;
	fsimg_error_t	 error;	/* The first allocation failure, or none */
} fsimg_codec_memory_t;

/* Start a fresh adapter over ctx with no error recorded. */
void fsimg_codec_memory_init(fsimg_codec_memory_t *, fsimg_ctx_t *);

/*
 * items times size bytes from the context, or NULL with the failure
 * recorded if it is the first: a product that overflows is FSIMG_ERANGE,
 * and after any failure every request fails at once. A zero-byte request
 * is an ordinary charged allocation returning a distinct pointer.
 */
void *fsimg_codec_memory_alloc(fsimg_codec_memory_t *, size_t, size_t);

/* Release an allocation the adapter made; NULL is a no-op. */
void fsimg_codec_memory_free(void *);

/*
 * The cause of a library's out-of-memory result: the recorded first
 * failure with its complete diagnostic, or FSIMG_ENOMEM when the library
 * reported one without an allocation failing through the adapter. Only
 * that result is interpreted this way; the recorded error is left in
 * place.
 */
fsimg_status_t fsimg_codec_memory_error(const fsimg_codec_memory_t *,
    fsimg_error_t *);

/*
 * Bind a fresh, zero-initialized z_stream with no zlib state to a fresh
 * adapter: set zalloc, zfree and opaque together, and clear the adapter's
 * error. The stream's other fields are the owner's; nothing is allocated.
 */
void fsimg_zlib_memory_bind(fsimg_codec_memory_t *, fsimg_ctx_t *, z_stream *);

/* The running zlib's name and version, as "zlib 1.3.1". */
void fsimg_zlib_version(char *, size_t);

#endif /* FSIMG_CODEC_MEMORY_H */
