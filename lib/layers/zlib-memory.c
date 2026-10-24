/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <limits.h>
#include <stdint.h>
#include <strings.h>

#include "host.h"
#include "zlib-memory.h"

/* zlib's two uInt factors must fit size_t for the product check below. */
_Static_assert(UINT_MAX <= SIZE_MAX, "uInt factors must fit size_t");

static voidpf zlib_memory_alloc(voidpf, uInt, uInt);
static void zlib_memory_free(voidpf, voidpf);

void
fsimg_zlib_memory_bind(fsimg_zlib_memory_t *memory, fsimg_ctx_t *ctx,
    z_stream *stream)
{
	memory->ctx = ctx;
	fsimg_error_clear(&memory->error);
	stream->zalloc = zlib_memory_alloc;
	stream->zfree = zlib_memory_free;
	stream->opaque = memory;
}

/*
 * Charge one zlib allocation to the context. The first failure is kept and
 * every later attempt fails at once without replacing it, so a later
 * successful allocation within the same zlib call cannot erase the cause.
 */
static voidpf
zlib_memory_alloc(voidpf opaque, uInt items, uInt size)
{
	fsimg_zlib_memory_t *memory = opaque;
	fsimg_error_t detail;
	void *allocation;

	if (memory->error.status != FSIMG_OK)
		return (Z_NULL);
	if (size != 0 && items > SIZE_MAX / size) {
		fsimg_error_set(&memory->error, FSIMG_ERANGE, 0,
		    FSIMG_OP_ALLOCATE, "zlib allocation size overflow");
		return (Z_NULL);
	}
	allocation = fsimg_alloc(memory->ctx, (size_t)items * size, &detail);
	if (allocation == NULL) {
		memory->error = detail;
		if (detail.status == FSIMG_OK)
			fsimg_error_set(&memory->error, FSIMG_ENOMEM, 0,
			    FSIMG_OP_ALLOCATE, "zlib allocation failed");
	}
	return (allocation);
}

static void
zlib_memory_free(voidpf opaque FSIMG_UNUSED, voidpf address)
{
	fsimg_dealloc(address);
}

fsimg_status_t
fsimg_zlib_memory_error(const fsimg_zlib_memory_t *memory,
    fsimg_error_t *error)
{
	if (memory->error.status == FSIMG_OK)
		return (fsimg_error_set(error, FSIMG_ENOMEM, 0,
		    FSIMG_OP_ALLOCATE, "zlib allocation failed"));
	*error = memory->error;
	return (error->status);
}
