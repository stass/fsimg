/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_FORMAT_H
#define FSIMG_FORMAT_H

#include "reader.h"
#include "writer.h"

/* One public format name served by a descriptor. Common code reports info
 * after deriving its capability bits from the descriptor's operations. id is
 * private to the driver: common code passes it through and never interprets it.
 * A reader's open receives the forced variant record, or NULL after detection;
 * a writer's validate receives the selected variant's id.
 */
struct fsimg_format_variant {
	fsimg_format_info_t info;
	unsigned	 id;
};

/* A descriptor is the only holder of a format's identity.
 *
 * probe is required. It inspects at most probe_bytes of the image prefix, or
 * the whole volume when it is shorter (prefix is NULL when the volume is
 * empty), and is a pure function of those bytes and the volume length. It
 * returns FSIMG_EFORMAT when the format's identity signature is absent (no
 * claim). Once the signature matches it claims the image: FSIMG_OK to open
 * it, or FSIMG_EUNSUPPORTED or FSIMG_ECORRUPT with a reason in error.
 *
 * reader and writer are independently optional.
 */
struct fsimg_format {
	const fsimg_format_variant_t *variants;
	size_t		 variant_count;
	size_t		 probe_bytes;
	fsimg_status_t	(*probe)(const unsigned char *, size_t, uint64_t,
			    fsimg_error_t *);
	const fsimg_reader_ops_t *reader;
	const fsimg_writer_ops_t *writer;
};

/* The registered descriptors; the registry in format.c lists them. */
extern const fsimg_format_t fsimg_fat_format;
extern const fsimg_format_t fsimg_exfat_format;
extern const fsimg_format_t fsimg_qnx6_format;

const fsimg_format_t *fsimg_format_find(const char *,
    const fsimg_format_variant_t **);
fsimg_status_t fsimg_format_claim(const fsimg_format_t *const *, size_t,
    const fsimg_format_t *, const unsigned char *, size_t, uint64_t,
    const fsimg_format_t **, fsimg_error_t *);
fsimg_status_t fsimg_format_open(fsimg_reader_t *, const char *, fsimg_error_t *);

#endif
