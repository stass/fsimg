/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "fsimg.h"

int
main(void)
{
	fsimg_options_t options;
	fsimg_ctx_t *ctx;

	fsimg_options_init(&options);
	if (fsimg_ctx_new(&options, &ctx, NULL) != FSIMG_OK)
		return (1);
	fsimg_ctx_free(ctx);
	return (0);
}
