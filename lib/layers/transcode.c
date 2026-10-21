/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <zlib.h>

#include "io.h"
#include "layer.h"
#include "publish.h"

/*
 * The transcode (specs/design-layers.md#transcode): one sequential pass
 * over a disk object, a raw intermediate or an assembled disk, into a
 * sequential sink. Each chunk of the disk is read, or recognised as lying
 * in a range the disk reports zero, and handed to an encoder as data or as
 * zeros; the container's encoder, or the copy encoder without one, decides
 * what bytes that becomes and appends them to the sink. With a compression, that sink
 * is a filter: the compression's encoder takes what the container appends
 * and writes its own encoding to the output's sink. The output's sink is
 * the only writer of the output, so no encoder seeks, and each sink holds
 * what is appended to it to the bound the plan charged.
 *
 * Zero decisions depend only on the disk's bytes. A zero range the disk
 * object reports (fsimg_io_next_zero_range) may
 * only spare the read of a chunk lying wholly inside it, so the output is
 * the same with hole knowledge consulted, ignored
 * (fsimg_test_hole_discovery) and for any buffer or budget size.
 */
#define SINK_BUFFER_BYTES	65536u

/*
 * The sequential sink. A filter sink hands everything appended to it to
 * its compression encoder (filter, filter_state), which writes the next
 * sink, and offset counts what it accepted. Any other sink is the
 * output's: bytes collect in buffer and are written at offset, the output
 * length so far excluding them, to exactly one destination: the
 * positional object io, or when io is NULL the stream fd. A zero run
 * appended to io goes through the zeroing operation, which writes nothing
 * on a fresh file's unwritten ranges and zeros everywhere else; a stream
 * receives every byte. The sink keeps no knowledge of its own about io.
 */
struct fsimg_seq_sink {
	fsimg_session_t	*session;
	uint64_t	 offset;	/* Accepted, or output written so far */
	uint64_t	 bound;		/* The most it may accept or write */
	/* A filter: its encoder, NULL for an output's sink, and state. */
	const fsimg_compressor_ops_t *filter;
	void		*filter_state;
	/* An output's sink only; unused in a filter, whose fd is -1. */
	fsimg_io_t	*io;		/* The positional object, or NULL */
	int		 fd;		/* The stream when io is NULL */
	unsigned char	*buffer;	/* SINK_BUFFER_BYTES */
	size_t		 used;		/* Buffered bytes not yet written */
	bool		 resized;	/* io was sized to bound; trim at close */
	bool		 touched;	/* The output may differ from before */
};

/* What fsimg_disk_output hands the staged file's fill. */
typedef struct disk_output {
	fsimg_session_t	*session;
	fsimg_io_t	*disk;
	const fsimg_disk_plan_t *plan;
	bool		 durable;
	uint64_t	 written;
} disk_output_t;

static fsimg_status_t sink_open(fsimg_seq_sink_t *, fsimg_session_t *,
    fsimg_io_t *, int, uint64_t, bool, bool, fsimg_error_t *);
static fsimg_status_t sink_flush(fsimg_seq_sink_t *, fsimg_error_t *);
static fsimg_status_t sink_close(fsimg_seq_sink_t *, bool, fsimg_error_t *);
static fsimg_status_t stream_write(fsimg_seq_sink_t *, const unsigned char *,
    size_t, fsimg_error_t *);
static fsimg_status_t sink_room(const fsimg_seq_sink_t *, uint64_t,
    fsimg_error_t *);
static fsimg_status_t filter_open(fsimg_seq_sink_t *, fsimg_session_t *,
    const fsimg_disk_plan_t *, fsimg_seq_sink_t *, fsimg_error_t *);
static void filter_close(fsimg_seq_sink_t *);
static bool all_zero(const unsigned char *, size_t);
static fsimg_status_t transcode_pass(const disk_output_t *,
    const fsimg_encoder_ops_t *, void *, unsigned, unsigned char *, size_t,
    fsimg_error_t *);
static fsimg_status_t transcode(const disk_output_t *, fsimg_seq_sink_t *,
    fsimg_error_t *);
static fsimg_status_t output_to(disk_output_t *, fsimg_io_t *, int,
    fsimg_error_t *);
static fsimg_status_t fill_staged(void *, fsimg_io_t *, uint64_t *,
    fsimg_error_t *);
static fsimg_status_t copy_open(const fsimg_encode_t *, void **,
    fsimg_error_t *);
static fsimg_status_t copy_chunk(void *, unsigned, const unsigned char *,
    size_t, fsimg_error_t *);
static fsimg_status_t copy_finish(void *, unsigned, fsimg_error_t *);
static void copy_close(void *);

/* Without a container the disk is copied unchanged, zeros as zeros. */
static const fsimg_encoder_ops_t copy_encoder = {
	.passes = 1,
	.open = copy_open,
	.chunk = copy_chunk,
	.finish = copy_finish,
	.close = copy_close
};

/*
 * Open a sink over a positional object io, or the stream fd when io is
 * NULL, for an output of at most bound bytes; exact says the output is
 * exactly bound bytes long, as a copy is. A positional object must be
 * writable, durable when asked, and either resizable or, for an exact
 * output, already bound bytes long. A resizable one is sized to bound
 * first, since an object accepts no write past its length, and to the
 * output's length when the sink closes. Nothing is written here but the
 * resize.
 */
static fsimg_status_t
sink_open(fsimg_seq_sink_t *sink, fsimg_session_t *session, fsimg_io_t *io,
    int fd, uint64_t bound, bool exact, bool durable, fsimg_error_t *error)
{
	fsimg_status_t status;
	bool resize;

	*sink = (fsimg_seq_sink_t){ .session = session, .io = io, .fd = fd,
	    .bound = bound };
	sink->buffer = fsimg_alloc(session->ctx, SINK_BUFFER_BYTES, error);
	if (sink->buffer == NULL)
		return (error->status);
	if (io == NULL)
		return (FSIMG_OK);
	if (fsimg_io_context(io) != session->ctx)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "inputs and output require the same context"));
	status = fsimg_io_check_output(io, bound, durable, &resize, error);
	if (status != FSIMG_OK)
		return (status);
	if (!resize && !exact)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_IO,
		    "an encoded output needs a resizable sink"));
	if (!resize)
		return (FSIMG_OK);
	sink->touched = sink->resized = true;
	return (fsimg_io_resize(io, bound, error));
}

/*
 * Open a filter sink over the output's sink out: the plan's compression
 * encodes what the container, or the copy, appends, at most the
 * container's bound. The encoder may write its header to out at once.
 */
static fsimg_status_t
filter_open(fsimg_seq_sink_t *sink, fsimg_session_t *session,
    const fsimg_disk_plan_t *plan, fsimg_seq_sink_t *out, fsimg_error_t *error)
{
	*sink = (fsimg_seq_sink_t){ .session = session, .fd = -1,
	    .bound = plan->container_bytes,
	    .filter = plan->compression->compression->encoder };
	return (sink->filter->open(session->ctx, &plan->compression_settings,
	    out, &sink->filter_state, error));
}

static void
filter_close(fsimg_seq_sink_t *sink)
{
	if (sink->filter_state != NULL)
		sink->filter->close(sink->filter_state);
	sink->filter_state = NULL;
}

/* Write the buffered bytes and empty the buffer. */
static fsimg_status_t
sink_flush(fsimg_seq_sink_t *sink, fsimg_error_t *error)
{
	size_t transferred;
	fsimg_status_t status;

	if (sink->used == 0)
		return (FSIMG_OK);
	sink->touched = true;
	if (sink->io != NULL)
		status = fsimg_io_write_session(sink->io, sink->session,
		    sink->offset, sink->buffer, sink->used, &transferred, error);
	else
		status = stream_write(sink, sink->buffer, sink->used, error);
	if (status != FSIMG_OK)
		return (status);
	sink->offset += sink->used;
	sink->used = 0;
	return (FSIMG_OK);
}

/*
 * Finish the output: write what is buffered, set a resized object's length
 * to the output's, and flush it, durably when asked. An exact output on an
 * object that could not resize already has its length. A stream needs only
 * its bytes.
 */
static fsimg_status_t
sink_close(fsimg_seq_sink_t *sink, bool durable, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = sink_flush(sink, error);
	if (status != FSIMG_OK || sink->io == NULL)
		return (status);
	if (sink->resized)
		status = fsimg_io_resize(sink->io, sink->offset, error);
	else if (sink->offset != sink->bound)
		status = fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "a copied disk did not fill its sink");
	if (status == FSIMG_OK)
		status = fsimg_io_flush(sink->io, durable, error);
	return (status);
}

/*
 * Write all of bytes to the stream, resuming after a short write or an
 * interrupted call, which also polls cancellation; any other failure is an
 * I/O error. A stream whose reader has gone raises SIGPIPE as write(2)
 * does; the library leaves the host's signal handling alone.
 */
static fsimg_status_t
stream_write(fsimg_seq_sink_t *sink, const unsigned char *bytes,
    size_t length, fsimg_error_t *error)
{
	fsimg_ctx_t *ctx = sink->session->ctx;
	fsimg_status_t status;
	ssize_t done;

	while (length > 0) {
		done = write(sink->fd, bytes, length > SSIZE_MAX ? SSIZE_MAX :
		    length);
		fsimg_stat_add(&ctx->stats.io_calls, 1);
		if (done == -1 && errno == EINTR) {
			status = fsimg_poll(ctx, NULL, error);
			if (status != FSIMG_OK)
				return (status);
			continue;
		}
		if (done <= 0)
			return (fsimg_error_set(error, FSIMG_EIO,
			    done == -1 ? errno : 0, FSIMG_OP_IO,
			    "stream output write failed"));
		fsimg_stat_add(&ctx->stats.written_bytes, (uint64_t)done);
		bytes += done;
		length -= (size_t)done;
	}
	return (FSIMG_OK);
}

/* Whether length more bytes stay within the bound the plan charged. Every
 * write and zero run asks first, so written never exceeds bound. An
 * encoder that crosses it misjudged its bound, an internal error.
 */
static fsimg_status_t
sink_room(const fsimg_seq_sink_t *sink, uint64_t length, fsimg_error_t *error)
{
	uint64_t written = sink->offset + sink->used;

	if (length > sink->bound - written)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "the output exceeds its planned bound"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_seq_write(fsimg_seq_sink_t *sink, const void *bytes, size_t length,
    fsimg_error_t *error)
{
	const unsigned char *next = bytes;
	size_t step;
	fsimg_status_t status;

	status = sink_room(sink, length, error);
	if (status == FSIMG_OK && sink->filter != NULL) {
		status = sink->filter->write(sink->filter_state, bytes, length,
		    error);
		if (status == FSIMG_OK)
			sink->offset += length;
		return (status);
	}
	while (status == FSIMG_OK && length > 0) {
		if (sink->used == SINK_BUFFER_BYTES) {
			status = sink_flush(sink, error);
			continue;
		}
		step = SINK_BUFFER_BYTES - sink->used;
		if (step > length)
			step = length;
		bcopy(next, sink->buffer + sink->used, step);
		sink->used += step;
		next += step;
		length -= step;
	}
	return (status);
}

/*
 * Append length zeros, in the sink's way: a filter encodes them; an
 * object has the range zeroed, which on a fresh file's never-written
 * range writes nothing, so it stays a hole; a stream is sent zero-filled
 * bytes from the emptied buffer.
 */
fsimg_status_t
fsimg_seq_zero(fsimg_seq_sink_t *sink, uint64_t length, fsimg_error_t *error)
{
	size_t step;
	fsimg_status_t status;

	status = sink_room(sink, length, error);
	if (status == FSIMG_OK && sink->filter != NULL) {
		status = sink->filter->write(sink->filter_state, NULL, length,
		    error);
		if (status == FSIMG_OK)
			sink->offset += length;
		return (status);
	}
	if (status == FSIMG_OK)
		status = sink_flush(sink, error);
	if (status != FSIMG_OK || length == 0)
		return (status);
	sink->touched = true;
	if (sink->io != NULL)
		status = fsimg_io_zero_session(sink->io, sink->session,
		    sink->offset, length, error);
	else {
		bzero(sink->buffer, SINK_BUFFER_BYTES);
		for (uint64_t done = 0; status == FSIMG_OK && done < length;
		    done += step) {
			step = length - done > SINK_BUFFER_BYTES ?
			    SINK_BUFFER_BYTES : (size_t)(length - done);
			status = stream_write(sink, sink->buffer, step, error);
		}
	}
	if (status == FSIMG_OK)
		sink->offset += length;
	return (status);
}

uint64_t
fsimg_seq_written(const fsimg_seq_sink_t *sink)
{
	return (sink->offset + sink->used);
}

static bool
all_zero(const unsigned char *bytes, size_t length)
{
	return (length == 0 ||
	    (bytes[0] == 0 && bcmp(bytes, bytes + 1, length - 1) == 0));
}

/*
 * One pass over the disk in chunks of the buffer's length, the last one
 * shorter. Every chunk charges a unit of work, which polls cancellation.
 * A chunk wholly inside the zero range [zero_start, zero_end) the disk
 * reports is zero without a read. The disk is asked again only once the
 * pass reaches the end of its last answer, which for an empty answer is
 * where the next range may begin, so the answers number about the zero
 * runs, not the chunks.
 */
static fsimg_status_t
transcode_pass(const disk_output_t *output, const fsimg_encoder_ops_t *ops,
    void *state, unsigned pass, unsigned char *buffer, size_t chunk,
    fsimg_error_t *error)
{
	const fsimg_hole_test_t *test = &output->session->ctx->hole_test;
	uint64_t disk_bytes = output->plan->disk_bytes;
	uint64_t zero_start, zero_end;
	size_t length, transferred;
	fsimg_status_t status;
	bool consult, zero;

	consult = !output->session->ctx->no_hole_discovery;
	zero_start = zero_end = 0;
	status = FSIMG_OK;
	for (uint64_t offset = 0; status == FSIMG_OK && offset < disk_bytes;
	    offset += length) {
		length = disk_bytes - offset > chunk ? chunk :
		    (size_t)(disk_bytes - offset);
		if (consult && offset >= zero_end) {
			status = fsimg_io_next_zero_range(output->disk, offset,
			    &zero_start, &zero_end, error);
			if (status != FSIMG_OK)
				break;
		}
		zero = consult && zero_start <= offset && offset < zero_end &&
		    (length <= zero_end - offset || test->partial_chunks);
		if (!zero || !test->unpolled_zeros)
			status = fsimg_session_charge(output->session, 0, 0, 1,
			    error);
		if (status != FSIMG_OK)
			break;
		if (!zero) {
			status = fsimg_io_read_session(output->disk,
			    output->session, offset, buffer, length, &transferred,
			    error);
			if (status != FSIMG_OK)
				break;
			zero = all_zero(buffer, length);
		}
		status = ops->chunk(state, pass, zero ? NULL : buffer, length,
		    error);
	}
	return (status);
}

/* Run the plan's encoder, or the copy encoder, over the disk into sink. */
static fsimg_status_t
transcode(const disk_output_t *output, fsimg_seq_sink_t *sink,
    fsimg_error_t *error)
{
	const fsimg_disk_plan_t *plan = output->plan;
	const fsimg_encoder_ops_t *ops;
	fsimg_encode_t encode;
	unsigned char *buffer;
	void *state;
	size_t chunk;
	fsimg_status_t status;

	ops = plan->container == NULL ? &copy_encoder :
	    plan->container->container->encoder;
	encode = (fsimg_encode_t){ .ctx = output->session->ctx,
	    .session = output->session, .sink = sink,
	    .settings = &plan->container_settings,
	    .disk_bytes = plan->disk_bytes };
	chunk = plan->container_settings.chunk_bytes;
	buffer = fsimg_alloc(encode.ctx, chunk, error);
	if (buffer == NULL)
		return (error->status);
	state = NULL;
	status = ops->open(&encode, &state, error);
	for (unsigned pass = 0; status == FSIMG_OK && pass < ops->passes;
	    pass++) {
		status = transcode_pass(output, ops, state, pass, buffer, chunk,
		    error);
		if (status == FSIMG_OK)
			status = ops->finish(state, pass, error);
	}
	if (state != NULL)
		ops->close(state);
	fsimg_dealloc(buffer);
	return (status);
}

/*
 * Transcode into a positional object or a stream: the encoders append to
 * the output's sink, or with a compression to a filter over it, which is
 * finished before the output's sink is closed. A failure once the output
 * may have changed leaves it incomplete; output->written is the output's
 * length when the transcode succeeds.
 */
static fsimg_status_t
output_to(disk_output_t *output, fsimg_io_t *io, int fd, fsimg_error_t *error)
{
	const fsimg_disk_plan_t *plan = output->plan;
	fsimg_seq_sink_t sink, filter;
	fsimg_seq_sink_t *head;
	fsimg_status_t status;

	filter = (fsimg_seq_sink_t){ .filter = NULL };
	head = &sink;
	status = sink_open(&sink, output->session, io, fd, plan->output_bytes,
	    !fsimg_disk_plan_encoded(plan), output->durable, error);
	if (status == FSIMG_OK && plan->compression != NULL) {
		head = &filter;
		status = filter_open(&filter, output->session, plan, &sink,
		    error);
	}
	if (status == FSIMG_OK)
		status = transcode(output, head, error);
	if (status == FSIMG_OK && head == &filter)
		status = filter.filter->finish(filter.filter_state, error);
	if (status == FSIMG_OK)
		status = sink_close(&sink, output->durable, error);
	if (status == FSIMG_OK)
		output->written = sink.offset;
	else if (sink.touched && error != NULL)
		error->output_state = FSIMG_OUTPUT_INCOMPLETE;
	filter_close(&filter);
	fsimg_dealloc(sink.buffer);
	return (status);
}

static fsimg_status_t
fill_staged(void *argument, fsimg_io_t *staged, uint64_t *length,
    fsimg_error_t *error)
{
	disk_output_t *output = argument;
	fsimg_status_t status;

	status = output_to(output, staged, -1, error);
	*length = output->written;
	return (status);
}

fsimg_status_t
fsimg_disk_output(fsimg_session_t *session, fsimg_io_t *disk,
    const fsimg_disk_plan_t *plan, const fsimg_sink_t *sink, bool durable,
    uint64_t *written, fsimg_error_t *error)
{
	disk_output_t output;
	fsimg_status_t status;

	output = (disk_output_t){ .session = session, .disk = disk,
	    .plan = plan, .durable = durable };
	switch (sink->kind) {
	case FSIMG_SINK_FILE:
		status = fsimg_output_stage(session->ctx, sink->parent_fd,
		    sink->name, fill_staged, &output, NULL, error);
		break;
	case FSIMG_SINK_IO:
		status = output_to(&output, sink->io, -1, error);
		break;
	case FSIMG_SINK_STREAM:
		status = output_to(&output, NULL, sink->stream_fd, error);
		break;
	default:
		status = fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_PUBLISH,
		    "invalid output sink");
		break;
	}
	*written = status == FSIMG_OK ? output.written : 0;
	return (status);
}

static fsimg_status_t
copy_open(const fsimg_encode_t *encode, void **state,
    fsimg_error_t *error FSIMG_UNUSED)
{
	*state = encode->sink;
	return (FSIMG_OK);
}

static fsimg_status_t
copy_chunk(void *state, unsigned pass FSIMG_UNUSED, const unsigned char *bytes,
    size_t length, fsimg_error_t *error)
{
	if (bytes == NULL)
		return (fsimg_seq_zero(state, length, error));
	return (fsimg_seq_write(state, bytes, length, error));
}

static fsimg_status_t
copy_finish(void *state FSIMG_UNUSED, unsigned pass FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

static void
copy_close(void *state FSIMG_UNUSED)
{
}

/* The codec a plan's output depends on: zlib for the DMG chunks it
 * deflates and for a zlib compression.
 */
void
fsimg_disk_plan_codec(const fsimg_disk_plan_t *plan, char *out,
    size_t capacity)
{
	if ((plan->container != NULL && plan->container_settings.level != 0) ||
	    (plan->compression != NULL && plan->compression->compression->zlib))
		snprintf(out, capacity, "zlib %s", zlibVersion());
	else if (capacity != 0)
		out[0] = '\0';
}

/*
 * The CRC32 of crc's bytes followed by length zeros, by binary
 * decomposition: block is the CRC32 of 2^k zeros, doubled with
 * crc32_combine at each step and folded into crc for each set bit of
 * length. Every block is the same zeros, so the folding order does not
 * matter. Lengths stay within z_off_t, which is 32 bits on some hosts, by
 * taking length in pieces of at most 2^30 there.
 */
unsigned long
fsimg_crc32_zeros(unsigned long crc, uint64_t length)
{
	static const unsigned char zero[1];
	const uint64_t piece_max = sizeof(z_off_t) < 8 ? UINT64_C(1) << 30 :
	    UINT64_C(1) << 62;
	unsigned long block;
	uint64_t piece;
	z_off_t size;

	while (length > 0) {
		piece = length > piece_max ? piece_max : length;
		length -= piece;
		/* size doubles only while bits remain, so it never exceeds
		 * piece_max and the signed z_off_t never overflows. */
		block = crc32(0, zero, 1);
		for (size = 1;; size *= 2) {
			if ((piece & 1) != 0)
				crc = crc32_combine(crc, block, size);
			piece >>= 1;
			if (piece == 0)
				break;
			block = crc32_combine(block, block, size);
		}
	}
	return (crc);
}
