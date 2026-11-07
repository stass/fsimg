/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_TEST_EROFS_CONTRACT_H
#define FSIMG_TEST_EROFS_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * An EROFS image of a source tree for the driver contracts, built with the
 * independent generator in erofs-gen.c. It has its own header because the
 * contracts also include the HFS Plus generator, whose type names collide
 * with the EROFS generator's.
 */

/* One source entry: an absolute path and, for a file, its size. */
typedef struct erofs_source {
	const char	*path;
	bool		 directory;
	uint64_t	 size;
} erofs_source_t;

/* Byte `offset` of the content of source entry `index`. */
typedef unsigned char (*erofs_byte_fn_t)(void *, size_t, uint64_t);

/*
 * The 4096-byte-block image holding the sources, a parent listed before its
 * children and the root implicit. Directories are flat, files flat-inline.
 * The result is allocated with malloc(); its size is stored in *length.
 */
unsigned char	*erofs_contract_image(const erofs_source_t *, size_t,
		    erofs_byte_fn_t, void *, size_t *);

#endif /* !FSIMG_TEST_EROFS_CONTRACT_H */
