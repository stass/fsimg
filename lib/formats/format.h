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
 * it, or FSIMG_EUNSUPPORTED or FSIMG_ECORRUPT with a reason in error. With
 * FSIMG_OK it also names the variant it identified, so a claim reported
 * without opening the volume names a public format.
 *
 * reader and writer are independently optional.
 */
struct fsimg_format {
	const fsimg_format_variant_t *variants;
	size_t		 variant_count;
	size_t		 probe_bytes;
	fsimg_status_t	(*probe)(const unsigned char *, size_t, uint64_t,
			    const fsimg_format_variant_t **, fsimg_error_t *);
	const fsimg_reader_ops_t *reader;
	const fsimg_writer_ops_t *writer;
};

/* The registered descriptors; the registry in format.c lists them. */
extern const fsimg_format_t fsimg_fat_format;
extern const fsimg_format_t fsimg_exfat_format;
extern const fsimg_format_t fsimg_qnx6_format;
extern const fsimg_format_t fsimg_qnx4_format;
extern const fsimg_format_t fsimg_ifs_format;

/*
 * What a set of probes says about one input, under the one claim rule for
 * filesystems and image layers alike: a probe failing with FSIMG_EFORMAT
 * does not claim the input, and any other verdict, a corrupt or
 * unsupported one included, is a claim. claims counts them; the first
 * claim's verdict and reason are kept with its claimant, a filesystem
 * format (with its variant on a match) or an image layer. A survey that
 * ran a forced probe alone keeps that probe's verdict even when it does
 * not claim, since its absent signature is the answer.
 */
struct fsimg_layer;

typedef struct fsimg_survey {
	const fsimg_format_t *format;
	const fsimg_format_variant_t *variant;
	const struct fsimg_layer *layer;
	unsigned	 claims;
	fsimg_status_t	 status;
	fsimg_error_t	 reason;
} fsimg_survey_t;

/* An empty survey: no claim, status FSIMG_EFORMAT. */
void fsimg_survey_init(fsimg_survey_t *);
/* Count one probe's verdict under the claim rule; true when it is the
 * first claim, whose claimant the caller records.
 */
bool fsimg_survey_count(fsimg_survey_t *, fsimg_status_t,
    const fsimg_error_t *);
/* What the count decides: no claim, several (AMBIGUOUS), or the one
 * claim as a match, an unsupported or a corrupt input.
 */
fsimg_claim_t fsimg_survey_claim(const fsimg_survey_t *);

const fsimg_format_t *fsimg_format_find(const char *,
    const fsimg_format_variant_t **);
void fsimg_format_survey(const fsimg_format_t *const *, size_t,
    const fsimg_format_t *, const unsigned char *, size_t, uint64_t,
    fsimg_survey_t *);
/* The registry's survey for the image layers: every reader format, or the
 * one named by forced_name, which must name a format with a reader. The
 * prefix needs at most fsimg_format_probe_bytes(forced_name) bytes.
 */
fsimg_status_t fsimg_format_identify(const char *, const unsigned char *,
    size_t, uint64_t, fsimg_survey_t *, fsimg_error_t *);
size_t fsimg_format_probe_bytes(const char *);
fsimg_status_t fsimg_format_claim(const fsimg_format_t *const *, size_t,
    const fsimg_format_t *, const unsigned char *, size_t, uint64_t,
    const fsimg_format_t **, fsimg_error_t *);
fsimg_status_t fsimg_format_open(fsimg_reader_t *, const char *, fsimg_error_t *);

#endif
