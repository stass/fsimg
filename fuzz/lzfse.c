/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "codec.h"

/* Byte 0: bit 0 selects raw LZVN; bits 1..7 choose the capacity in
 * 512-byte steps, with code 127 selecting 65536. The rest is exact-size input.
 * test-lzfse --seed writes this framing. Wide output copies must respect the
 * chosen capacity; a tail guard makes that invariant observable without ASan.
 */
int LLVMFuzzerTestOneInput(const uint8_t *, size_t);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint8_t *in, *out, *reference;
	void *scratch;
	size_t capacity, code;
	fsimg_codec_step_t step, first = { 0 };
	fsimg_error_t error;
	fsimg_status_t status;

	if (size < 1 || size - 1 > FSIMG_LZFSE_INPUT_MAX)
		return (0);
	code = data[0] >> 1;
	capacity = code == 127 ? FSIMG_LZFSE_OUTPUT_MAX : code * 512;
	in = malloc(size == 1 ? 1 : size - 1);
	out = malloc(capacity + 32);
	reference = malloc(capacity + 1);
	scratch = malloc(fsimg_lzfse_scratch_bytes());
	if (in == NULL || out == NULL || reference == NULL || scratch == NULL)
		abort();
	bcopy(data + 1, in, size - 1);
	/* Successful bytes must not depend on the caller's output or scratch
	 * contents. Initialized output alone hides unwritten bytes from MSan.
	 */
	for (unsigned pass = 0; pass < 2; pass++) {
		uint8_t fill = pass == 0 ? 0xa5 : 0x5a;

		memset(out, fill, capacity + 32);
		memset(scratch, fill, fsimg_lzfse_scratch_bytes());
		if ((data[0] & 1) != 0)
			status = fsimg_lzvn_decode(in, size - 1, out, capacity,
			    &step, &error);
		else
			status = fsimg_lzfse_decode(in, size - 1, out, capacity,
			    scratch, &step, &error);
		for (size_t i = capacity; i < capacity + 32; i++)
			if (out[i] != fill)
				abort();
		if (pass == 0) {
			if (status != FSIMG_OK)
				break;
			if (!step.done || step.produced > capacity)
				abort();
			first = step;
			bcopy(out, reference, step.produced);
		} else if (status != FSIMG_OK || !step.done ||
		    step.produced != first.produced ||
		    step.consumed != first.consumed ||
		    bcmp(out, reference, first.produced) != 0) {
			abort();
		}
	}
	free(scratch);
	free(reference);
	free(out);
	free(in);
	return (0);
}
