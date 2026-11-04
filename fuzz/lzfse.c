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
	uint8_t *in, *out;
	void *scratch;
	size_t capacity, code;
	fsimg_codec_step_t step;
	fsimg_error_t error;

	if (size < 1 || size - 1 > FSIMG_LZFSE_INPUT_MAX)
		return (0);
	code = data[0] >> 1;
	capacity = code == 127 ? FSIMG_LZFSE_OUTPUT_MAX : code * 512;
	in = malloc(size == 1 ? 1 : size - 1);
	out = malloc(capacity + 32);
	scratch = malloc(fsimg_lzfse_scratch_bytes());
	if (in == NULL || out == NULL || scratch == NULL)
		abort();
	bcopy(data + 1, in, size - 1);
	memset(out, 0xa5, capacity + 32);
	if ((data[0] & 1) != 0)
		(void)fsimg_lzvn_decode(in, size - 1, out, capacity, &step, &error);
	else
		(void)fsimg_lzfse_decode(in, size - 1, out, capacity, scratch,
		    &step, &error);
	for (size_t i = capacity; i < capacity + 32; i++)
		if (out[i] != 0xa5)
			abort();
	free(scratch);
	free(out);
	free(in);
	return (0);
}
