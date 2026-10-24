/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdint.h>

#include "host.h"
#include "zlib-memory.h"

/* Two uInt factors are converted to size_t before they are multiplied. */
_Static_assert((uInt)-1 <= SIZE_MAX, "a uInt must fit a size_t");

static voidpf memory_alloc(voidpf, uInt, uInt);
static void memory_free(voidpf, voidpf);

/*
 * zlib may make several allocations before it tests any of them, and a
 * successful context allocation clears the error record it is given, so
 * each attempt reports into a local record and only the first failure is
 * kept. A zero-byte request is an ordinary charged allocation: it returns
 * a distinct pointer that zlib can free, and it can fail its budget.
 */
static voidpf
memory_alloc(voidpf opaque, uInt items, uInt size)
{
	fsimg_zlib_memory_t *memory = opaque;
	fsimg_error_t detail;
	void *allocation;

	if (memory->error.status != FSIMG_OK)
		return (Z_NULL);
	if (size != 0 && (size_t)items > SIZE_MAX / size) {
		fsimg_error_set(&memory->error, FSIMG_ERANGE, 0,
		    FSIMG_OP_ALLOCATE, "zlib allocation size overflow");
		return (Z_NULL);
	}
	allocation = fsimg_alloc(memory->ctx, (size_t)items * size, &detail);
	if (allocation == NULL)
		memory->error = detail;
	return (allocation);
}

static void
memory_free(voidpf opaque FSIMG_UNUSED, voidpf address)
{
	fsimg_dealloc(address);
}

void
fsimg_zlib_memory_bind(fsimg_zlib_memory_t *memory, fsimg_ctx_t *ctx,
    z_stream *stream)
{
	memory->ctx = ctx;
	fsimg_error_clear(&memory->error);
	stream->zalloc = memory_alloc;
	stream->zfree = memory_free;
	stream->opaque = memory;
}

fsimg_status_t
fsimg_zlib_memory_error(const fsimg_zlib_memory_t *memory,
    fsimg_error_t *error)
{
	if (memory->error.status == FSIMG_OK)
		return (fsimg_error_set(error, FSIMG_ENOMEM, 0,
		    FSIMG_OP_ALLOCATE, "zlib could not allocate its state"));
	*error = memory->error;
	return (error->status);
}
