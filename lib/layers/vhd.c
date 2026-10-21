/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "io.h"
#include "layer.h"

/*
 * Fixed VHD: the raw disk followed by a 512-byte big-endian footer, so the
 * disk is simply the file before the footer, a slice with no chunk index.
 * The footer's identity is its "conectix" cookie, its disk type and, for a
 * fixed disk, a current size equal to the bytes before the footer. A
 * dynamic or differencing disk has no such tie; its identity is a copy of
 * the footer at offset 0 and a data offset inside the file, and it is
 * reported as unsupported rather than read as a raw disk.
 */
#define VHD_FOOTER_BYTES	512u
#define VHD_VERSION		UINT32_C(0x00010000)
#define VHD_FIXED		2u
#define VHD_DYNAMIC		3u
#define VHD_DIFFERENCING	4u

/* Footer fields. */
enum {
	FOOTER_COOKIE = 0,
	FOOTER_VERSION = 12,
	FOOTER_DATA_OFFSET = 16,
	FOOTER_CURRENT_SIZE = 48,
	FOOTER_DISK_TYPE = 60,
	FOOTER_CHECKSUM = 64,
	FOOTER_UNIQUE_ID = 68,
	FOOTER_SAVED_STATE = 84
};

static fsimg_status_t vhd_corrupt(fsimg_error_t *, const char *);
static fsimg_status_t vhd_probe(const unsigned char *, size_t,
    const unsigned char *, size_t, uint64_t, fsimg_error_t *);
static fsimg_status_t footer_check(const unsigned char *, uint64_t,
    fsimg_error_t *);
static fsimg_status_t read_footer(const fsimg_container_input_t *,
    unsigned char *, fsimg_error_t *);
static fsimg_status_t vhd_open(const fsimg_container_input_t *,
    fsimg_container_t *, fsimg_error_t *);
static fsimg_status_t vhd_verify(const fsimg_container_input_t *,
    const fsimg_container_t *, bool, fsimg_error_t *);

const fsimg_container_ops_t fsimg_vhd_container = {
	.probe_bytes = VHD_FOOTER_BYTES,
	.probe = vhd_probe,
	.open = vhd_open,
	.verify = vhd_verify,
	.codecs = "raw",
	.payload_checks = false,
	.zlib = false
};

static fsimg_status_t
vhd_corrupt(fsimg_error_t *error, const char *check)
{
	char message[sizeof(error->message)];

	snprintf(message, sizeof(message), "VHD footer %s", check);
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

static fsimg_status_t
vhd_probe(const unsigned char *prefix, size_t prefix_length,
    const unsigned char *tail, size_t tail_length, uint64_t file_bytes,
    fsimg_error_t *error)
{
	uint64_t data_offset;
	uint32_t type;

	if (tail_length < VHD_FOOTER_BYTES ||
	    bcmp(tail + FOOTER_COOKIE, "conectix", 8) != 0)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no VHD footer"));
	type = fsimg_layer_be32(tail + FOOTER_DISK_TYPE);
	if (type == VHD_FIXED &&
	    fsimg_layer_be64(tail + FOOTER_CURRENT_SIZE) ==
	    file_bytes - VHD_FOOTER_BYTES)
		return (FSIMG_OK);
	if (type == VHD_DYNAMIC || type == VHD_DIFFERENCING) {
		data_offset = fsimg_layer_be64(tail + FOOTER_DATA_OFFSET);
		if (prefix_length >= VHD_FOOTER_BYTES &&
		    bcmp(prefix, tail, VHD_FOOTER_BYTES) == 0 &&
		    data_offset < file_bytes)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_IMAGE, type == VHD_DYNAMIC ?
			    "dynamic VHD images are not supported" :
			    "differencing VHD images are not supported"));
	}
	return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
	    "no VHD footer"));
}

/*
 * Validate a claimed fixed footer: its checksum, the one's complement of
 * the byte sum with the checksum field taken as zero, and the constant
 * fields a fixed disk carries.
 */
static fsimg_status_t
footer_check(const unsigned char *footer, uint64_t file_bytes,
    fsimg_error_t *error)
{
	uint32_t sum;
	fsimg_status_t status;

	status = vhd_probe(NULL, 0, footer, VHD_FOOTER_BYTES, file_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	sum = 0;
	for (unsigned i = 0; i < VHD_FOOTER_BYTES; i++) {
		if (i < FOOTER_CHECKSUM || i >= FOOTER_CHECKSUM + 4)
			sum += footer[i];
	}
	if (~sum != fsimg_layer_be32(footer + FOOTER_CHECKSUM))
		return (vhd_corrupt(error, "checksum does not match"));
	if (fsimg_layer_be32(footer + FOOTER_VERSION) != VHD_VERSION)
		return (vhd_corrupt(error, "format version is not 1.0"));
	if (fsimg_layer_be64(footer + FOOTER_DATA_OFFSET) != UINT64_MAX)
		return (vhd_corrupt(error, "data offset is set on a fixed disk"));
	if (footer[FOOTER_SAVED_STATE] != 0)
		return (vhd_corrupt(error, "records a saved state"));
	if ((file_bytes - VHD_FOOTER_BYTES) % 512 != 0)
		return (vhd_corrupt(error, "disk size is not a whole number of sectors"));
	return (FSIMG_OK);
}

static fsimg_status_t
read_footer(const fsimg_container_input_t *input, unsigned char *footer,
    fsimg_error_t *error)
{
	size_t transferred;
	fsimg_status_t status;

	if (input->file_bytes < VHD_FOOTER_BYTES)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no VHD footer"));
	status = fsimg_io_read_session(input->file, input->session,
	    input->file_bytes - VHD_FOOTER_BYTES, footer, VHD_FOOTER_BYTES,
	    &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	return (footer_check(footer, input->file_bytes, error));
}

/* The disk is the file before the footer: one raw chunk. */
static fsimg_status_t
vhd_open(const fsimg_container_input_t *input, fsimg_container_t *out,
    fsimg_error_t *error)
{
	unsigned char footer[VHD_FOOTER_BYTES];
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	status = read_footer(input, footer, error);
	if (status != FSIMG_OK)
		return (status);
	out->disk_bytes = input->file_bytes - VHD_FOOTER_BYTES;
	status = fsimg_io_read_slice(input->file, 0, out->disk_bytes, &out->disk,
	    error);
	if (status != FSIMG_OK)
		return (status);
	bcopy(footer + FOOTER_UNIQUE_ID, out->guid, sizeof(out->guid));
	out->chunks = 1;
	out->kinds = 1u << FSIMG_CHUNK_RAW;
	return (FSIMG_OK);
}

/* A fixed VHD has no bytes beyond its footer to check; verification reads
 * the footer again and repeats its checks.
 */
static fsimg_status_t
vhd_verify(const fsimg_container_input_t *input,
    const fsimg_container_t *container FSIMG_UNUSED, bool payload FSIMG_UNUSED,
    fsimg_error_t *error)
{
	unsigned char footer[VHD_FOOTER_BYTES];

	return (read_footer(input, footer, error));
}
