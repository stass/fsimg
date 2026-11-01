/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdint.h>
#include <stdio.h>

#include "codec-memory.h"
#include "host.h"

/* Two uInt factors are converted to size_t before they are multiplied. */
_Static_assert((uInt)-1 <= SIZE_MAX, "a uInt must fit a size_t");

static voidpf zlib_alloc(voidpf, uInt, uInt);
static void zlib_free(voidpf, voidpf);

void
fsimg_codec_memory_init(fsimg_codec_memory_t *memory, fsimg_ctx_t *ctx)
{
	memory->ctx = ctx;
	fsimg_error_clear(&memory->error);
}

/*
 * A library may make several allocations before it tests any of them,
 * and a successful context allocation clears the error record it is
 * given, so each attempt reports into a local record and only the first
 * failure is kept.
 */
void *
fsimg_codec_memory_alloc(fsimg_codec_memory_t *memory, size_t items,
    size_t size)
{
	fsimg_error_t detail;
	void *allocation;

	if (memory->error.status != FSIMG_OK)
		return (NULL);
	if (size != 0 && items > SIZE_MAX / size) {
		fsimg_error_set(&memory->error, FSIMG_ERANGE, 0,
		    FSIMG_OP_ALLOCATE, "codec allocation size overflow");
		return (NULL);
	}
	allocation = fsimg_alloc(memory->ctx, items * size, &detail);
	if (allocation == NULL)
		memory->error = detail;
	return (allocation);
}

void
fsimg_codec_memory_free(void *address)
{
	fsimg_dealloc(address);
}

fsimg_status_t
fsimg_codec_memory_error(const fsimg_codec_memory_t *memory,
    fsimg_error_t *error)
{
	if (memory->error.status == FSIMG_OK)
		return (fsimg_error_set(error, FSIMG_ENOMEM, 0,
		    FSIMG_OP_ALLOCATE,
		    "the codec library could not allocate its state"));
	*error = memory->error;
	return (error->status);
}

/* zlib's binding: its callbacks with zlib's exact signatures. */
static voidpf
zlib_alloc(voidpf opaque, uInt items, uInt size)
{
	return (fsimg_codec_memory_alloc(opaque, (size_t)items, (size_t)size));
}

static void
zlib_free(voidpf opaque FSIMG_UNUSED, voidpf address)
{
	fsimg_codec_memory_free(address);
}

void
fsimg_zlib_memory_bind(fsimg_codec_memory_t *memory, fsimg_ctx_t *ctx,
    z_stream *stream)
{
	fsimg_codec_memory_init(memory, ctx);
	stream->zalloc = zlib_alloc;
	stream->zfree = zlib_free;
	stream->opaque = memory;
}

void
fsimg_zlib_version(char *out, size_t capacity)
{
	snprintf(out, capacity, "zlib %s", zlibVersion());
}
