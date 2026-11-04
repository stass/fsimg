/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "codec.h"
#include "fsimg.h"

/*
 * The bzip2 decoder on arbitrary streams. The first two bytes are a
 * little-endian capacity in units of 16 bytes (0 to 1048560 bytes); the
 * rest is one stream. test-codec --seed-bzip2 writes this seed format.
 *
 * A successful call ends with all input consumed and output within the
 * chosen capacity. Bytes beyond the reported output retain their poison.
 * Exact allocations let sanitizer builds check both buffer boundaries.
 * A context is created and freed for each call, including corrupt inputs.
 */
int LLVMFuzzerTestOneInput(const uint8_t *, size_t);

int
LLVMFuzzerTestOneInput(const uint8_t *in, size_t length)
{
	fsimg_options_t options;
	fsimg_codec_step_t step;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	fsimg_status_t status;
	uint8_t *out;
	size_t capacity;

	if (length < 2)
		return (0);
	capacity = ((size_t)in[0] | (size_t)in[1] << 8) * 16;
	out = malloc(capacity == 0 ? 1 : capacity);
	if (out == NULL)
		abort();
	memset(out, 0xa5, capacity);
	fsimg_options_init(&options);
	if (fsimg_ctx_new(&options, &ctx, &error) != FSIMG_OK)
		abort();
	status = fsimg_bzip2_decode(ctx, in + 2, length - 2, out, capacity,
	    &step, &error);
	if (step.consumed > length - 2 || step.produced > capacity)
		abort();
	if (status == FSIMG_OK &&
	    (!step.done || step.consumed != length - 2))
		abort();
	for (size_t i = step.produced; i < capacity; i++)
		if (out[i] != 0xa5)
			abort();
	fsimg_ctx_free(ctx);
	free(out);
	return (0);
}
