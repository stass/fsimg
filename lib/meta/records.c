/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "fsimg-endian.h"
#include "meta.h"

struct fsimg_array {
	fsimg_store_t	*store;
	uint64_t	 count;
	uint32_t	 record_size;
};

struct fsimg_bitset {
	fsimg_store_t	*store;
	uint64_t	 bits;
};

static fsimg_status_t record_write(fsimg_store_t *, uint64_t, const void *,
    uint32_t, fsimg_error_t *);
static fsimg_status_t array_offset(fsimg_array_t *, uint64_t, uint64_t *,
    fsimg_error_t *);
fsimg_status_t
fsimg_record_length(fsimg_store_t *store, uint64_t offset, uint64_t end,
    uint32_t maximum, uint32_t *out, fsimg_error_t *error)
{
	unsigned char header[8];
	uint64_t frame;
	fsimg_status_t status;

	*out = 0;
	if (end > fsimg_store_size(store) ||
	    !fsimg_range_fits(offset, sizeof(header), end))
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_METADATA, "truncated metadata frame"));
	status = fsimg_store_read(store, offset, header, sizeof(header), error);
	if (status != FSIMG_OK)
		return (status);
	frame = fsimg_load_le64(header);
	if ((uint32_t)frame != 1 || (frame >> 32) > maximum ||
	    !fsimg_range_fits(offset + sizeof(header), frame >> 32, end))
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_METADATA, "invalid metadata frame version or length"));
	*out = (uint32_t)(frame >> 32);
	return (FSIMG_OK);
}

/* Write one frame at offset, growing the store when the frame ends past it. */
static fsimg_status_t
record_write(fsimg_store_t *store, uint64_t offset, const void *data,
    uint32_t length, fsimg_error_t *error)
{
	unsigned char header[8];
	uint64_t end;
	fsimg_status_t status;

	if (!fsimg_add_u64(offset, sizeof(header) + (uint64_t)length, &end))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "metadata stream length overflow"));
	fsimg_store_le64(header, ((uint64_t)length << 32) | 1);
	if (end > fsimg_store_size(store)) {
		status = fsimg_store_grow(store, end, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = fsimg_store_write(store, offset, header, sizeof(header), error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_store_write(store, offset + sizeof(header), data,
	    length, error));
}

fsimg_status_t
fsimg_record_append(fsimg_store_t *store, const void *data, uint32_t length,
    fsimg_error_t *error)
{
	return (record_write(store, fsimg_store_size(store), data, length,
	    error));
}

fsimg_status_t
fsimg_stream_new(fsimg_session_t *session, uint32_t maximum, fsimg_stream_t **out,
    fsimg_error_t *error)
{
	fsimg_stream_t *stream;
	fsimg_ctx_t *ctx;
	fsimg_error_t detail;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	ctx = session == NULL ? NULL : session->ctx;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "missing stream output"));
	stream = fsimg_alloc(ctx, sizeof(*stream), &detail);
	if (stream == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = fsimg_store_new(session, &stream->store, error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(stream);
		return (status);
	}
	stream->max_record = maximum;
	*out = stream;
	return (FSIMG_OK);
}

void
fsimg_stream_free(fsimg_stream_t *stream)
{
	if (stream == NULL || fsimg_store_context(stream->store)->in_callback)
		return;
	fsimg_store_free(stream->store);
	fsimg_dealloc(stream);
}

fsimg_status_t
fsimg_stream_append(fsimg_stream_t *stream, const void *data, size_t length,
    uint64_t *reference, fsimg_error_t *error)
{
	uint64_t offset;
	fsimg_status_t status;

	if (reference != NULL)
		*reference = 0;
	if (stream == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "missing stream"));
	status = fsimg_ctx_ready(fsimg_store_context(stream->store), error);
	if (status != FSIMG_OK)
		return (status);
	if (stream->sealed)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_METADATA, "sealed stream cannot append"));
	if (length > stream->max_record || (data == NULL && length != 0))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "invalid metadata record length or buffer"));
	if (stream->count == UINT64_MAX)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "metadata record count overflow"));
	offset = stream->end;
	status = record_write(stream->store, offset, data, (uint32_t)length,
	    error);
	if (status != FSIMG_OK)
		return (status);
	stream->end = offset + 8 + (uint64_t)length;
	stream->count++;
	if (reference != NULL)
		*reference = offset;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_stream_seal(fsimg_stream_t *stream, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (stream == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "missing stream"));
	status = fsimg_store_read(stream->store, 0, NULL, 0, error);
	if (status != FSIMG_OK)
		return (status);
	stream->sealed = true;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_stream_next(fsimg_stream_t *stream, uint64_t *cursor, void *buffer,
    size_t capacity, size_t *length, fsimg_error_t *error)
{
	uint64_t end;
	uint32_t record_length;
	fsimg_status_t status;

	if (length != NULL)
		*length = 0;
	if (stream == NULL || cursor == NULL || length == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "missing stream iteration argument"));
	status = fsimg_store_read(stream->store, 0, NULL, 0, error);
	if (status != FSIMG_OK)
		return (status);
	end = stream->end;
	if (*cursor == end)
		return (FSIMG_DONE);
	status = fsimg_record_length(stream->store, *cursor, end,
	    stream->max_record, &record_length, error);
	if (status != FSIMG_OK)
		return (status);
	if (record_length > capacity || (record_length != 0 && buffer == NULL))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "record exceeds iteration buffer"));
	status = fsimg_store_read(stream->store, *cursor + 8, buffer,
	    record_length, error);
	if (status != FSIMG_OK)
		return (status);
	*cursor += 8 + (uint64_t)record_length;
	*length = record_length;
	return (FSIMG_OK);
}

fsimg_stream_mark_t
fsimg_stream_mark(const fsimg_stream_t *stream)
{
	return ((fsimg_stream_mark_t){ .offset = stream->end,
	    .count = stream->count });
}

fsimg_status_t
fsimg_stream_rewind(fsimg_stream_t *stream, fsimg_stream_mark_t mark,
    fsimg_error_t *error)
{
	if (stream == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "missing stream"));
	if (stream->sealed)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_METADATA, "sealed stream cannot rewind"));
	if (mark.offset > stream->end || mark.count > stream->count)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "stream mark is past the stream end"));
	stream->end = mark.offset;
	stream->count = mark.count;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_array_new(fsimg_session_t *session, uint32_t record_size, uint64_t count,
    fsimg_array_t **out, fsimg_error_t *error)
{
	fsimg_array_t *array;
	fsimg_ctx_t *ctx;
	fsimg_error_t detail;
	uint64_t bytes;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	ctx = session == NULL ? NULL : session->ctx;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL || record_size == 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "invalid array output or record size"));
	if (!fsimg_mul_u64(count, record_size, &bytes))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "array length overflow"));
	array = fsimg_alloc(ctx, sizeof(*array), &detail);
	if (array == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = fsimg_store_new(session, &array->store, error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(array);
		return (status);
	}
	array->count = count;
	array->record_size = record_size;
	status = fsimg_store_grow(array->store, bytes, error);
	if (status != FSIMG_OK) {
		fsimg_array_free(array);
		return (status);
	}
	*out = array;
	return (FSIMG_OK);
}

void
fsimg_array_free(fsimg_array_t *array)
{
	if (array == NULL || fsimg_store_context(array->store)->in_callback)
		return;
	fsimg_store_free(array->store);
	fsimg_dealloc(array);
}

static fsimg_status_t
array_offset(fsimg_array_t *array, uint64_t index, uint64_t *offset,
    fsimg_error_t *error)
{
	*offset = 0;
	if (array == NULL || index >= array->count)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "array index outside object"));
	*offset = index * array->record_size;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_array_get(fsimg_array_t *array, uint64_t index, void *buffer,
    fsimg_error_t *error)
{
	uint64_t offset;
	fsimg_status_t status;

	status = array_offset(array, index, &offset, error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_store_read(array->store, offset, buffer,
	    array->record_size, error));
}

fsimg_status_t
fsimg_array_set(fsimg_array_t *array, uint64_t index, const void *buffer,
    fsimg_error_t *error)
{
	uint64_t offset;
	fsimg_status_t status;

	status = array_offset(array, index, &offset, error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_store_write(array->store, offset, buffer,
	    array->record_size, error));
}

fsimg_status_t
fsimg_bitset_new(fsimg_session_t *session, uint64_t bits, fsimg_bitset_t **out,
    fsimg_error_t *error)
{
	fsimg_bitset_t *bitset;
	fsimg_ctx_t *ctx;
	fsimg_error_t detail;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	ctx = session == NULL ? NULL : session->ctx;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "missing bitset output"));
	bitset = fsimg_alloc(ctx, sizeof(*bitset), &detail);
	if (bitset == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = fsimg_store_new(session, &bitset->store, error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(bitset);
		return (status);
	}
	bitset->bits = bits;
	status = fsimg_store_grow(bitset->store, bits / 8 + (bits % 8 != 0),
	    error);
	if (status != FSIMG_OK) {
		fsimg_bitset_free(bitset);
		return (status);
	}
	*out = bitset;
	return (FSIMG_OK);
}

void
fsimg_bitset_free(fsimg_bitset_t *bitset)
{
	if (bitset == NULL || fsimg_store_context(bitset->store)->in_callback)
		return;
	fsimg_store_free(bitset->store);
	fsimg_dealloc(bitset);
}

fsimg_status_t
fsimg_bitset_test(fsimg_bitset_t *bitset, uint64_t bit, bool *set,
    fsimg_error_t *error)
{
	fsimg_status_t status;
	unsigned char byte;

	if (set != NULL)
		*set = false;
	if (bitset == NULL || bit >= bitset->bits || set == NULL)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "invalid bitset index or output"));
	status = fsimg_store_read(bitset->store, bit / 8, &byte, 1, error);
	if (status == FSIMG_OK)
		*set = (byte & (1u << (bit % 8))) != 0;
	return (status);
}

fsimg_status_t
fsimg_bitset_test_and_set(fsimg_bitset_t *bitset, uint64_t bit, bool *was_set,
    fsimg_error_t *error)
{
	fsimg_status_t status;
	unsigned char byte, mask;

	if (was_set != NULL)
		*was_set = false;
	if (bitset == NULL || bit >= bitset->bits || was_set == NULL)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "invalid bitset index or output"));
	status = fsimg_store_read(bitset->store, bit / 8, &byte, 1, error);
	if (status != FSIMG_OK)
		return (status);
	mask = (unsigned char)(1u << (bit % 8));
	*was_set = (byte & mask) != 0;
	byte |= mask;
	return (fsimg_store_write(bitset->store, bit / 8, &byte, 1, error));
}

/* A run of bits is updated a chunk of bytes at a time: each byte's mask
 * covers the bits of the range inside it, so a run of n bits costs about
 * n / 4096 store accesses instead of one per bit.
 */
fsimg_status_t
fsimg_bitset_test_and_set_range(fsimg_bitset_t *bitset, uint64_t first,
    uint64_t count, bool *was_set, fsimg_error_t *error)
{
	unsigned char chunk[512];
	uint64_t end, byte, last_byte, low, high;
	size_t length;
	unsigned mask;
	fsimg_status_t status;

	if (was_set != NULL)
		*was_set = false;
	if (bitset == NULL || was_set == NULL || first > bitset->bits ||
	    count > bitset->bits - first)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "invalid bitset range or output"));
	if (count == 0)
		return (FSIMG_OK);
	end = first + count;
	last_byte = (end - 1) / 8;
	for (byte = first / 8; byte <= last_byte; byte += length) {
		length = last_byte - byte + 1 < sizeof(chunk) ?
		    (size_t)(last_byte - byte + 1) : sizeof(chunk);
		status = fsimg_store_read(bitset->store, byte, chunk, length,
		    error);
		if (status != FSIMG_OK)
			return (status);
		for (size_t i = 0; i < length; i++) {
			/* The range's bits within byte + i: [low, high). */
			low = (byte + i) * 8 < first ? first % 8 : 0;
			high = (byte + i) * 8 + 8 > end ? end % 8 : 8;
			mask = (0xffu >> (8 - (high - low))) << low;
			if ((chunk[i] & mask) != 0)
				*was_set = true;
			chunk[i] |= (unsigned char)mask;
		}
		status = fsimg_store_write(bitset->store, byte, chunk, length,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_bitset_read(fsimg_bitset_t *bitset, uint64_t first_byte, void *buffer,
    size_t length, fsimg_error_t *error)
{
	uint64_t bytes;

	if (bitset == NULL || (buffer == NULL && length != 0))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "invalid bitset range or output"));
	bytes = bitset->bits / 8 + (bitset->bits % 8 != 0);
	if (first_byte > bytes || length > bytes - first_byte)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "invalid bitset range or output"));
	if (length == 0)
		return (FSIMG_OK);
	return (fsimg_store_read(bitset->store, first_byte, buffer, length,
	    error));
}
