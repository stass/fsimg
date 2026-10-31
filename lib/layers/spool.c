/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "io.h"
#include "layer.h"

/*
 * The spool holds an input that cannot be read in place: a stream, which
 * cannot seek to a container's trailer, or a compressed file, which offers
 * no random access (specs/design-layers.md#stream-input-and-the-spool).
 * It is a handed-over file: the caller created it exclusively and unlinked
 * it, so no name reaches it, fsimg is its only writer, and its unwritten
 * ranges are proven zero by its written-range record.
 *
 * Bytes only ever arrive at the end. Each append first reserves its length
 * against the context's spool limit, which every live spool shares, then
 * extends the file to the new length and writes only the blocks that hold
 * a nonzero byte: an extended range reads as zero, so a zero block is
 * skipped and stays a hole, and all-zero input costs no storage while its
 * logical length is charged in full. Because the reservation precedes the
 * extension, the file never grows past the limit; the reservation is kept
 * until the spool is freed, even when the host fails partway.
 */
#define SPOOL_BLOCK_BYTES	4096u
#define SPOOL_COPY_BYTES	(256u * 1024)

struct fsimg_spool {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*file;
	uint64_t	 length;	/* Bytes appended, all reserved */
	uint64_t	 physical;	/* Allocated bytes sampled at seal */
};

static bool block_zero(const unsigned char *, size_t);
static size_t block_run(const unsigned char *, size_t, bool);
static fsimg_status_t spool_reserve(fsimg_spool_t *, size_t, fsimg_error_t *);
static fsimg_status_t stream_read(fsimg_spool_input_t *, unsigned char *,
    size_t, size_t *, fsimg_error_t *);

fsimg_status_t
fsimg_spool_adopt(fsimg_ctx_t *ctx, int fd, fsimg_spool_t **out,
    fsimg_error_t *error)
{
	fsimg_spool_t *spool;
	fsimg_status_t status;

	*out = NULL;
	spool = fsimg_alloc(ctx, sizeof(*spool), error);
	if (spool == NULL) {
		close(fd);
		return (error->status);
	}
	bzero(spool, sizeof(*spool));
	spool->ctx = ctx;
	status = fsimg_io_adopt_handed_over(ctx, fd, "spool", &spool->file,
	    error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(spool);
		return (status);
	}
	*out = spool;
	return (FSIMG_OK);
}

void
fsimg_spool_free(fsimg_spool_t *spool)
{
	if (spool == NULL)
		return;
	spool->ctx->stats.spool_bytes -= spool->length;
	spool->ctx->stats.spool_physical_bytes -= spool->physical;
	fsimg_io_free(spool->file);
	fsimg_dealloc(spool);
}

/* Charge length more logical bytes to the context's spool limit. */
static fsimg_status_t
spool_reserve(fsimg_spool_t *spool, size_t length, fsimg_error_t *error)
{
	fsimg_ctx_t *ctx = spool->ctx;
	uint64_t observed;

	if (!fsimg_add_u64(ctx->stats.spool_bytes, length, &observed))
		return (fsimg_limit_error(error, FSIMG_RESOURCE_SPOOL,
		    ctx->options.spool_bytes, UINT64_MAX));
	if (observed > ctx->options.spool_bytes)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_SPOOL,
		    ctx->options.spool_bytes, observed));
	ctx->stats.spool_bytes = observed;
	spool->length += length;
	if (observed > ctx->stats.spool_peak)
		ctx->stats.spool_peak = observed;
	return (FSIMG_OK);
}

static bool
block_zero(const unsigned char *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		if (bytes[i] != 0)
			return (false);
	}
	return (true);
}

/*
 * The length of the leading run of blocks at data that are all zero, or,
 * when zero is false, that each hold a nonzero byte. Blocks start at data,
 * and the last one may be short.
 */
static size_t
block_run(const unsigned char *data, size_t length, bool zero)
{
	size_t block, run;

	for (run = 0; run < length; run += block) {
		block = length - run < SPOOL_BLOCK_BYTES ? length - run :
		    SPOOL_BLOCK_BYTES;
		if (block_zero(data + run, block) != zero)
			break;
	}
	return (run);
}

fsimg_status_t
fsimg_spool_append(fsimg_spool_t *spool, const unsigned char *data,
    size_t length, fsimg_error_t *error)
{
	fsimg_progress_t progress = { .phase = "spool" };
	uint64_t base;
	size_t offset, run, transferred;
	fsimg_status_t status;

	if (length == 0)
		return (FSIMG_OK);
	progress.bytes = spool->length;
	status = fsimg_poll(spool->ctx, &progress, error);
	if (status != FSIMG_OK)
		return (status);
	base = spool->length;
	status = spool_reserve(spool, length, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_io_resize(spool->file, spool->length, error);
	if (status != FSIMG_OK)
		return (status);

	/* Alternate: skip a run of zero blocks, which stay holes, then write
	 * the run of nonzero blocks after it as one transfer. Each pass
	 * advances, since the next block is one kind or the other.
	 */
	offset = 0;
	while (offset < length) {
		offset += block_run(data + offset, length - offset, true);
		run = block_run(data + offset, length - offset, false);
		if (run == 0)
			continue;
		status = fsimg_io_write_exact(spool->file, base + offset,
		    data + offset, run, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		offset += run;
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_spool_seal(fsimg_spool_t *spool, fsimg_io_t **view, fsimg_error_t *error)
{
	fsimg_ctx_t *ctx = spool->ctx;
	uint64_t bytes;
	fsimg_status_t status;

	*view = NULL;
	status = fsimg_io_allocated_bytes(spool->file, &bytes, error);
	if (status != FSIMG_OK)
		return (status);
	if (!fsimg_add_u64(ctx->stats.spool_physical_bytes, bytes,
	    &ctx->stats.spool_physical_bytes))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_IO,
		    "spool allocation size overflow"));
	spool->physical = bytes;
	return (fsimg_io_read_slice(spool->file, 0, spool->length, view, error));
}

/* One read(2) of the stream, retrying interruptions, polling cancellation
 * before each attempt.
 */
static fsimg_status_t
stream_read(fsimg_spool_input_t *source, unsigned char *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	ssize_t count;
	fsimg_status_t status;

	for (unsigned interruptions = 0;; interruptions++) {
		status = fsimg_poll(source->ctx, NULL, error);
		if (status != FSIMG_OK)
			return (status);
		count = read(source->fd, buffer, length);
		if (count >= 0) {
			*transferred = (size_t)count;
			fsimg_stat_add(&source->ctx->stats.read_bytes,
			    (uint64_t)count);
			return (FSIMG_OK);
		}
		if (errno != EINTR || interruptions == 1024)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_IO, "image stream read failed"));
	}
}

fsimg_status_t
fsimg_spool_input_read(fsimg_spool_input_t *source, unsigned char *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	uint64_t left;
	fsimg_status_t status;

	*transferred = 0;
	if (source->prefix_length != 0) {
		*transferred = source->prefix_length < length ?
		    source->prefix_length : length;
		bcopy(source->prefix, buffer, *transferred);
		source->prefix += *transferred;
		source->prefix_length -= *transferred;
		return (FSIMG_OK);
	}
	if (source->file == NULL)
		return (stream_read(source, buffer, length, transferred, error));
	left = source->file_bytes - source->offset;
	if (left < length)
		length = (size_t)left;
	if (length == 0)
		return (FSIMG_OK);
	status = fsimg_io_read_session(source->file, source->session,
	    source->offset, buffer, length, transferred, error);
	if (status == FSIMG_OK)
		source->offset += length;
	return (status);
}

fsimg_status_t
fsimg_spool_fill(fsimg_spool_t *spool, fsimg_spool_input_t *source,
    fsimg_error_t *error)
{
	unsigned char *buffer;
	size_t count;
	fsimg_status_t status;

	buffer = fsimg_alloc(spool->ctx, SPOOL_COPY_BYTES, error);
	if (buffer == NULL)
		return (error->status);
	do {
		status = fsimg_spool_input_read(source, buffer, SPOOL_COPY_BYTES,
		    &count, error);
		if (status == FSIMG_OK)
			status = fsimg_spool_append(spool, buffer, count, error);
	} while (status == FSIMG_OK && count != 0);
	fsimg_dealloc(buffer);
	return (status);
}
