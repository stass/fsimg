/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "format.h"

/* The registry is the intentional list of supported drivers, in reporting
 * order. Nothing else in common code names a format.
 */
static const fsimg_format_t *const formats[] = {
	&fsimg_fat_format,
	&fsimg_exfat_format
};

#define FORMAT_COUNT	(sizeof(formats) / sizeof(formats[0]))

/* Capabilities follow from which operations a descriptor provides, so a
 * format cannot advertise an operation it lacks.
 */
static unsigned
format_capabilities(const fsimg_format_t *format)
{
	unsigned capabilities;

	capabilities = 0;
	if (format->reader != NULL) {
		capabilities |= FSIMG_CAP_READ;
		if (format->reader->verify != NULL)
			capabilities |= FSIMG_CAP_VERIFY;
	}
	if (format->writer != NULL)
		capabilities |= FSIMG_CAP_WRITE;
	return (capabilities);
}

const fsimg_format_t *
fsimg_format_find(const char *name, const fsimg_format_variant_t **variant)
{
	for (size_t i = 0; i < FORMAT_COUNT; i++) {
		for (size_t j = 0; j < formats[i]->variant_count; j++) {
			if (strcmp(name, formats[i]->variants[j].info.name) == 0) {
				*variant = &formats[i]->variants[j];
				return (formats[i]);
			}
		}
	}
	*variant = NULL;
	return (NULL);
}

/* Run one probe over its declared share of the prefix. The probe's reason is
 * cleared first, so a claim never carries stale bytes.
 */
static fsimg_status_t
format_probe(const fsimg_format_t *format, const unsigned char *prefix,
    size_t length, uint64_t volume_bytes, fsimg_error_t *error)
{
	fsimg_error_clear(error);
	if (length > format->probe_bytes)
		length = format->probe_bytes;
	return (format->probe(prefix, length, volume_bytes, error));
}

/* Decide which descriptor may open an image from its prefix. A forced
 * descriptor is the only one probed, so its absent signature is final; no
 * other decoder is tried. Otherwise every probe runs and exactly one claim
 * decides: a match selects its descriptor and an unsupported or corrupt claim
 * is the result. No claim or several claims cannot be resolved here. A
 * successful claim always names a descriptor with a reader.
 */
fsimg_status_t
fsimg_format_claim(const fsimg_format_t *const *registry, size_t count,
    const fsimg_format_t *forced, const unsigned char *prefix, size_t length,
    uint64_t volume_bytes, const fsimg_format_t **claimed, fsimg_error_t *error)
{
	const fsimg_format_t *claimant;
	fsimg_error_t detail, claim_error;
	fsimg_status_t status, claim_status;

	*claimed = NULL;
	if (forced != NULL) {
		claimant = forced;
		claim_status = format_probe(forced, prefix, length, volume_bytes,
		    &claim_error);
	} else {
		/* Keep the first claimant's status and reason; they decide only
		 * when no other probe claims the image.
		 */
		size_t claim_count;

		claimant = NULL;
		claim_status = FSIMG_EFORMAT;
		fsimg_error_clear(&claim_error);
		claim_count = 0;
		for (size_t i = 0; i < count; i++) {
			status = format_probe(registry[i], prefix, length,
			    volume_bytes, &detail);
			if (status == FSIMG_EFORMAT)
				continue;
			claim_count++;
			if (claim_count == 1) {
				claimant = registry[i];
				claim_status = status;
				claim_error = detail;
			}
		}
		if (claim_count == 0)
			return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
			    FSIMG_OP_READER, "unrecognized image format"));
		if (claim_count > 1)
			return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
			    FSIMG_OP_READER, "image matches several formats; force one"));
	}
	if (claim_status != FSIMG_OK) {
		if (error != NULL)
			*error = claim_error;
		return (claim_status);
	}
	if (claimant->reader == NULL)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "recognized format has no reader"));
	*claimed = claimant;
	fsimg_error_clear(error);
	return (FSIMG_OK);
}

/* Read the image prefix through the reader's session into a context-charged
 * buffer. An empty prefix needs no buffer and leaves *prefix NULL.
 */
static fsimg_status_t
format_read_prefix(fsimg_reader_t *reader, size_t length,
    unsigned char **prefix, fsimg_error_t *error)
{
	fsimg_error_t detail;
	size_t transferred;
	fsimg_status_t status;

	*prefix = NULL;
	if (length == 0)
		return (FSIMG_OK);
	*prefix = fsimg_alloc(reader->ctx, length, &detail);
	if (*prefix == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = fsimg_reader_bytes(reader, 0, *prefix, length, &transferred,
	    error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(*prefix);
		*prefix = NULL;
	}
	return (status);
}

/* Select a driver and open the volume with it, in three stages: size the
 * prefix (the forced descriptor's, or the largest any descriptor declares, cut
 * to the volume), read it once and claim, then open the claimed driver.
 * Probes never perform I/O or allocate driver state themselves.
 */
fsimg_status_t
fsimg_format_open(fsimg_reader_t *reader, const char *forced_name,
    fsimg_error_t *error)
{
	const fsimg_format_t *forced, *claimed;
	const fsimg_format_variant_t *variant;
	unsigned char *prefix;
	uint64_t volume_bytes;
	size_t length;
	fsimg_status_t status;

	forced = NULL;
	variant = NULL;
	length = 0;
	if (forced_name != NULL) {
		forced = fsimg_format_find(forced_name, &variant);
		if (forced == NULL || forced->reader == NULL)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_READER, "unsupported reader format"));
		length = forced->probe_bytes;
	} else {
		for (size_t i = 0; i < FORMAT_COUNT; i++) {
			if (formats[i]->probe_bytes > length)
				length = formats[i]->probe_bytes;
		}
	}
	status = fsimg_io_get_size(reader->input, &volume_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	if (length > volume_bytes)
		length = (size_t)volume_bytes;

	status = format_read_prefix(reader, length, &prefix, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_format_claim(formats, FORMAT_COUNT, forced, prefix,
	    length, volume_bytes, &claimed, error);
	fsimg_dealloc(prefix);
	if (status != FSIMG_OK)
		return (status);

	reader->ops = claimed->reader;
	status = reader->ops->open(reader, variant, error);
	if (status != FSIMG_OK)
		return (status);
	/* An opened volume offers its driver's reader operations. */
	reader->info.capabilities = format_capabilities(claimed) &
	    (FSIMG_CAP_READ | FSIMG_CAP_VERIFY);
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_format_next(size_t *cursor, fsimg_format_info_t *out, fsimg_error_t *error)
{
	const fsimg_format_t *format;
	size_t index;

	fsimg_error_clear(error);
	if (cursor == NULL || out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "missing format cursor or output"));
	bzero(out, sizeof(*out));
	index = *cursor;
	for (size_t i = 0; i < FORMAT_COUNT; i++) {
		format = formats[i];
		if (index >= format->variant_count) {
			index -= format->variant_count;
			continue;
		}
		*out = format->variants[index].info;
		out->capabilities = format_capabilities(format);
		(*cursor)++;
		return (FSIMG_OK);
	}
	return (FSIMG_DONE);
}
