/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "fsimg-endian.h"
#include "io.h"
#include "layer.h"

/*
 * Fixed VHD: the raw disk followed by a 512-byte big-endian footer, so the
 * disk is simply the file before the footer, a slice with no chunk index.
 * The footer's identity is its "conectix" cookie, its disk type and, for a
 * fixed disk, a current size equal to the bytes before the footer. A
 * dynamic or differencing disk has no such tie; its identity is a copy of
 * the footer at offset 0 and a data offset inside the file, and it is
 * reported as unsupported rather than read as a raw disk. Writing copies
 * the disk and appends the footer.
 */
#define VHD_FOOTER_BYTES	512u
#define VHD_VERSION		UINT32_C(0x00010000)
#define VHD_FEATURES_RESERVED	UINT32_C(0x00000002)
#define VHD_FIXED		2u
#define VHD_DYNAMIC		3u
#define VHD_DIFFERENCING	4u
#define VHD_MAX_ALIGN		(UINT32_C(1) << 30)
#define VHD_EPOCH		INT64_C(946684800)	/* 2000-01-01 UTC */

/* Footer fields. */
enum {
	FOOTER_COOKIE = 0,
	FOOTER_FEATURES = 8,
	FOOTER_VERSION = 12,
	FOOTER_DATA_OFFSET = 16,
	FOOTER_TIMESTAMP = 24,
	FOOTER_CREATOR = 28,
	FOOTER_CREATOR_VERSION = 32,
	FOOTER_CREATOR_HOST = 36,
	FOOTER_ORIGINAL_SIZE = 40,
	FOOTER_CURRENT_SIZE = 48,
	FOOTER_GEOMETRY = 56,
	FOOTER_DISK_TYPE = 60,
	FOOTER_CHECKSUM = 64,
	FOOTER_UNIQUE_ID = 68,
	FOOTER_SAVED_STATE = 84
};

/* The private plan: the footer's creation time in Unix seconds, the epoch
 * under FSIMG_WRITE_REPRODUCIBLE and the host clock otherwise.
 */
typedef struct vhd_plan {
	fsimg_container_plan_t base;
	int64_t		 timestamp;
} vhd_plan_t;

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
static fsimg_status_t vhd_plan(const fsimg_container_plan_args_t *,
    fsimg_container_plan_t **, fsimg_error_t *);
static fsimg_status_t vhd_bound(const fsimg_container_plan_t *, uint64_t,
    uint64_t *, fsimg_error_t *);
static void vhd_geometry(uint64_t, unsigned char *);
static void vhd_footer(const vhd_plan_t *, uint64_t,
    unsigned char [static VHD_FOOTER_BYTES]);
static fsimg_status_t vhd_encode_open(const fsimg_encode_t *, void **,
    fsimg_error_t *);
static fsimg_status_t vhd_encode_chunk(void *, unsigned,
    const unsigned char *, size_t, fsimg_error_t *);
static fsimg_status_t vhd_encode_finish(void *, unsigned, fsimg_error_t *);
static void vhd_encode_close(void *);

static const fsimg_encoder_ops_t vhd_encoder = {
	.open = vhd_encode_open,
	.chunk = vhd_encode_chunk,
	.finish = vhd_encode_finish,
	.close = vhd_encode_close
};

static const fsimg_container_writer_t vhd_writer = {
	.plan = vhd_plan,
	.plan_free = fsimg_container_plan_release,
	.bound = vhd_bound,
	.encoder = &vhd_encoder
};

const fsimg_container_ops_t fsimg_vhd_container = {
	.probe_bytes = VHD_FOOTER_BYTES,
	.probe = vhd_probe,
	.open = vhd_open,
	.verify = vhd_verify,
	.writer = &vhd_writer,
	.codecs = "raw",
	.trailer_bytes = VHD_FOOTER_BYTES,
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
	type = fsimg_load_be32(tail + FOOTER_DISK_TYPE);
	if (type == VHD_FIXED &&
	    fsimg_load_be64(tail + FOOTER_CURRENT_SIZE) ==
	    file_bytes - VHD_FOOTER_BYTES)
		return (FSIMG_OK);
	if (type == VHD_DYNAMIC || type == VHD_DIFFERENCING) {
		data_offset = fsimg_load_be64(tail + FOOTER_DATA_OFFSET);
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
	if (~sum != fsimg_load_be32(footer + FOOTER_CHECKSUM))
		return (vhd_corrupt(error, "checksum does not match"));
	if (fsimg_load_be32(footer + FOOTER_VERSION) != VHD_VERSION)
		return (vhd_corrupt(error, "format version is not 1.0"));
	if (fsimg_load_be64(footer + FOOTER_DATA_OFFSET) != UINT64_MAX)
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

/*
 * The write side: the disk unchanged, then the footer. The plan resolves
 * vhd:align, the disk-length granularity, the unique identifier and the
 * creation time; the footer's timestamp counts seconds since 2000-01-01
 * UTC, clamped to its 32 bits, so the default 1980 epoch encodes as zero.
 */
static fsimg_status_t
vhd_plan(const fsimg_container_plan_args_t *args, fsimg_container_plan_t **out,
    fsimg_error_t *error)
{
	const fsimg_layer_options_t *layers = args->layers;
	uint32_t flags = args->flags;
	int64_t epoch = args->epoch;
	vhd_plan_t *plan;
	uint32_t align;

	align = layers->vhd_align_bytes == 0 ? 512 : layers->vhd_align_bytes;
	if ((align & (align - 1)) != 0 || align < 512 || align > VHD_MAX_ALIGN)
		return (fsimg_setting_error(error, "vhd:align",
		    "the alignment must be a power of two from 512 bytes to 1 GiB"));
	plan = fsimg_container_plan_alloc(args->ctx, sizeof(*plan), out, error);
	if (plan == NULL)
		return (error->status);
	plan->base.granularity = align;
	plan->timestamp = (flags & FSIMG_WRITE_REPRODUCIBLE) != 0 ? epoch :
	    (int64_t)time(NULL);
	return (fsimg_container_identifier(layers->vhd_uuid, "vhd:uuid",
	    FSIMG_ROLE_VHD, flags, epoch, plan->base.identifier, error));
}

static fsimg_status_t
vhd_bound(const fsimg_container_plan_t *plan FSIMG_UNUSED,
    uint64_t disk_bytes, uint64_t *out, fsimg_error_t *error)
{
	if (!fsimg_add_u64(disk_bytes, VHD_FOOTER_BYTES, out))
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, "the disk is too long for a VHD"));
	return (FSIMG_OK);
}

/* The specification's CHS geometry for a disk of sectors. */
static void
vhd_geometry(uint64_t sectors, unsigned char *out)
{
	uint64_t per_cylinder;
	uint32_t heads, per_track, cylinders;

	if (sectors > UINT64_C(65535) * 16 * 255)
		sectors = UINT64_C(65535) * 16 * 255;
	if (sectors >= UINT64_C(65535) * 16 * 63) {
		per_track = 255;
		heads = 16;
		per_cylinder = sectors / per_track;
	} else {
		per_track = 17;
		per_cylinder = sectors / per_track;
		heads = (uint32_t)((per_cylinder + 1023) / 1024);
		if (heads < 4)
			heads = 4;
		if (per_cylinder >= heads * 1024 || heads > 16) {
			per_track = 31;
			heads = 16;
			per_cylinder = sectors / per_track;
		}
		if (per_cylinder >= heads * 1024) {
			per_track = 63;
			heads = 16;
			per_cylinder = sectors / per_track;
		}
	}
	cylinders = (uint32_t)(per_cylinder / heads);
	out[0] = (unsigned char)(cylinders >> 8);
	out[1] = (unsigned char)cylinders;
	out[2] = (unsigned char)heads;
	out[3] = (unsigned char)per_track;
}

/*
 * The footer of a fixed disk of disk_bytes: the creator "fsim" at fsimg's
 * major and minor release, on the host "Wi2k" every reader accepts, the
 * original size equal to the current one, and the checksum over the rest.
 */
static void
vhd_footer(const vhd_plan_t *plan, uint64_t disk_bytes,
    unsigned char footer[static VHD_FOOTER_BYTES])
{
	unsigned major, minor;
	int64_t seconds;
	uint32_t sum;

	major = minor = 0;
	sscanf(PACKAGE_VERSION, "%u.%u", &major, &minor);
	seconds = plan->timestamp < VHD_EPOCH ? 0 :
	    plan->timestamp - VHD_EPOCH;
	bzero(footer, VHD_FOOTER_BYTES);
	bcopy("conectix", footer + FOOTER_COOKIE, 8);
	fsimg_store_be32(footer + FOOTER_FEATURES, VHD_FEATURES_RESERVED);
	fsimg_store_be32(footer + FOOTER_VERSION, VHD_VERSION);
	fsimg_store_be64(footer + FOOTER_DATA_OFFSET, UINT64_MAX);
	fsimg_store_be32(footer + FOOTER_TIMESTAMP,
	    seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds);
	bcopy("fsim", footer + FOOTER_CREATOR, 4);
	fsimg_store_be32(footer + FOOTER_CREATOR_VERSION,
	    (uint32_t)(major & 0xffff) << 16 | (minor & 0xffff));
	bcopy("Wi2k", footer + FOOTER_CREATOR_HOST, 4);
	fsimg_store_be64(footer + FOOTER_ORIGINAL_SIZE, disk_bytes);
	fsimg_store_be64(footer + FOOTER_CURRENT_SIZE, disk_bytes);
	vhd_geometry(disk_bytes / 512, footer + FOOTER_GEOMETRY);
	fsimg_store_be32(footer + FOOTER_DISK_TYPE, VHD_FIXED);
	bcopy(plan->base.identifier, footer + FOOTER_UNIQUE_ID, 16);
	sum = 0;
	for (unsigned i = 0; i < VHD_FOOTER_BYTES; i++)
		sum += footer[i];
	fsimg_store_be32(footer + FOOTER_CHECKSUM, ~sum);
}

/* The encoder's state is a copy of what it encodes. */
static fsimg_status_t
vhd_encode_open(const fsimg_encode_t *encode, void **state,
    fsimg_error_t *error)
{
	fsimg_encode_t *copy;

	copy = fsimg_alloc(encode->ctx, sizeof(*copy), error);
	if (copy == NULL)
		return (error->status);
	*copy = *encode;
	*state = copy;
	return (FSIMG_OK);
}

/* The disk's bytes pass through unchanged, zeros as zeros. */
static fsimg_status_t
vhd_encode_chunk(void *state, unsigned pass FSIMG_UNUSED,
    const unsigned char *bytes, size_t length, fsimg_error_t *error)
{
	const fsimg_encode_t *encode = state;

	if (bytes == NULL)
		return (fsimg_seq_zero(encode->sink, length, error));
	return (fsimg_seq_write(encode->sink, bytes, length, error));
}

static fsimg_status_t
vhd_encode_finish(void *state, unsigned pass FSIMG_UNUSED,
    fsimg_error_t *error)
{
	const fsimg_encode_t *encode = state;
	unsigned char footer[VHD_FOOTER_BYTES];

	vhd_footer((const vhd_plan_t *)encode->plan, encode->disk_bytes,
	    footer);
	return (fsimg_seq_write(encode->sink, footer, sizeof(footer), error));
}

static void
vhd_encode_close(void *state)
{
	fsimg_dealloc(state);
}
