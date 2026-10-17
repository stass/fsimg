/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "internal.h"
#include "protocol.h"

static bool time_field_valid(const unsigned char *);
static bool entry_metadata_valid(const unsigned char *);
static fsimg_status_t protocol_error(fsimg_error_t *, const char *);
static fsimg_status_t transfer_frame_bytes(int, void *, size_t, bool,
    fsimg_error_t *);
static fsimg_status_t decode_header(fsimg_frame_decoder_t *, fsimg_error_t *);

void
fsimg_wire_put64(unsigned char *bytes, uint64_t value)
{
	for (unsigned i = 0; i < 8; i++)
		bytes[i] = (unsigned char)(value >> (i * 8));
}

uint64_t
fsimg_wire_get64(const unsigned char *bytes)
{
	uint64_t value;

	value = 0;
	for (unsigned i = 0; i < 8; i++)
		value |= (uint64_t)bytes[i] << (i * 8);
	return (value);
}

int64_t
fsimg_wire_get_signed64(const unsigned char *bytes)
{
	uint64_t value;

	value = fsimg_wire_get64(bytes);
	if (value <= INT64_MAX)
		return ((int64_t)value);
	return (-1 - (int64_t)(UINT64_MAX - value));
}

static fsimg_status_t
protocol_error(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_EIO, 0, FSIMG_OP_CONTEXT, message));
}

fsimg_status_t
fsimg_frame_header(unsigned char *header, fsimg_frame_type_t type,
    uint32_t length, fsimg_error_t *error)
{
	if (length > FSIMG_FRAME_BYTES || type < FSIMG_FRAME_JOB ||
	    type > FSIMG_FRAME_DONE)
		return (protocol_error(error, "invalid worker frame type or length"));
	bzero(header, FSIMG_FRAME_HEADER);
	bcopy("FSIP", header, 4);
	header[4] = 1;
	header[5] = (unsigned char)type;
	fsimg_wire_put64(header + 8, length);
	return (FSIMG_OK);
}

static fsimg_status_t
decode_header(fsimg_frame_decoder_t *decoder, fsimg_error_t *error)
{
	uint64_t length;

	length = fsimg_wire_get64(decoder->header + 8);
	if (bcmp(decoder->header, "FSIP", 4) != 0 || decoder->header[4] != 1 ||
	    decoder->header[6] != 0 || decoder->header[7] != 0 ||
	    decoder->header[5] < FSIMG_FRAME_JOB || decoder->header[5] > FSIMG_FRAME_DONE ||
	    length > FSIMG_FRAME_BYTES)
		return (protocol_error(error, "malformed worker frame header"));
	decoder->frame.type = (fsimg_frame_type_t)decoder->header[5];
	decoder->frame.length = (uint32_t)length;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_frame_feed(fsimg_frame_decoder_t *decoder, const void *input,
    size_t length, size_t *consumed, fsimg_error_t *error)
{
	const unsigned char *bytes = input;
	size_t batch;
	fsimg_status_t status;

	*consumed = 0;
	if (decoder->complete)
		return (protocol_error(error, "worker frame was not consumed"));
	while (*consumed < length) {
		if (decoder->header_used < FSIMG_FRAME_HEADER) {
			batch = FSIMG_FRAME_HEADER - decoder->header_used;
			if (batch > length - *consumed)
				batch = length - *consumed;
			bcopy(bytes + *consumed, decoder->header + decoder->header_used, batch);
			decoder->header_used += batch;
			*consumed += batch;
			if (decoder->header_used != FSIMG_FRAME_HEADER)
				continue;
			status = decode_header(decoder, error);
			if (status != FSIMG_OK)
				return (status);
		}
		batch = decoder->frame.length - decoder->payload_used;
		if (batch > length - *consumed)
			batch = length - *consumed;
		bcopy(bytes + *consumed, decoder->frame.payload + decoder->payload_used, batch);
		decoder->payload_used += batch;
		*consumed += batch;
		if (decoder->payload_used == decoder->frame.length) {
			decoder->complete = true;
			return (FSIMG_OK);
		}
	}
	return (FSIMG_OK);
}

void
fsimg_frame_advance(fsimg_frame_decoder_t *decoder)
{
	bzero(decoder, sizeof(*decoder));
}

fsimg_status_t
fsimg_frame_finish(const fsimg_frame_decoder_t *decoder, fsimg_error_t *error)
{
	if (decoder->header_used != 0 || decoder->payload_used != 0 || decoder->complete)
		return (protocol_error(error, "truncated or unconsumed worker frame"));
	return (FSIMG_OK);
}

static fsimg_status_t
transfer_frame_bytes(int fd, void *buffer, size_t length, bool writing,
    fsimg_error_t *error)
{
	unsigned char *bytes = buffer;
	size_t done;
	ssize_t count;
	unsigned interruptions;

	done = 0;
	interruptions = 0;
	while (done < length) {
		if (writing)
			count = write(fd, bytes + done, length - done);
		else
			count = read(fd, bytes + done, length - done);
		if (count == -1 && errno == EINTR && interruptions++ < 1024)
			continue;
		if (count == -1)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_CONTEXT, "worker channel transfer failed"));
		if (count == 0)
			return (protocol_error(error, "worker channel ended prematurely"));
		done += (size_t)count;
		interruptions = 0;
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_frame_write(int fd, fsimg_frame_type_t type, const void *payload,
    uint32_t length, fsimg_error_t *error)
{
	unsigned char header[FSIMG_FRAME_HEADER];
	fsimg_status_t status;

	status = fsimg_frame_header(header, type, length, error);
	if (status != FSIMG_OK)
		return (status);
	status = transfer_frame_bytes(fd, header, sizeof(header), true, error);
	if (status != FSIMG_OK)
		return (status);
	return (transfer_frame_bytes(fd, (void *)payload, length, true, error));
}

fsimg_status_t
fsimg_frame_read(int fd, fsimg_frame_t *frame, fsimg_error_t *error)
{
	fsimg_frame_decoder_t decoder;
	fsimg_status_t status;

	bzero(&decoder, sizeof(decoder));
	status = transfer_frame_bytes(fd, decoder.header, FSIMG_FRAME_HEADER, false, error);
	if (status != FSIMG_OK)
		return (status);
	status = decode_header(&decoder, error);
	if (status != FSIMG_OK)
		return (status);
	frame->type = decoder.frame.type;
	frame->length = decoder.frame.length;
	return (transfer_frame_bytes(fd, frame->payload, frame->length, false, error));
}

void
fsimg_wire_error_encode(unsigned char *raw, const fsimg_error_t *error)
{
	size_t path_length, message_length;
	uint64_t flags;

	path_length = strnlen(error->path, sizeof(error->path));
	message_length = strnlen(error->message, sizeof(error->message));
	flags = (error->offset_valid ? 1u : 0u) |
	    (error->path_truncated ? 2u : 0u) | (error->message_truncated ? 4u : 0u);
	if (path_length == sizeof(error->path)) {
		path_length--;
		flags |= 2;
	}
	if (message_length == sizeof(error->message)) {
		message_length--;
		flags |= 4;
	}
	bzero(raw, FSIMG_WIRE_ERROR_BYTES);
	fsimg_wire_put64(raw + FSIMG_WIRE_ERROR_STATUS,
	    (uint64_t)(int64_t)error->status);
	fsimg_wire_put64(raw + FSIMG_WIRE_ERROR_OS_ERROR, (unsigned)error->os_error);
	fsimg_wire_put64(raw + FSIMG_WIRE_ERROR_OPERATION, error->operation);
	fsimg_wire_put64(raw + FSIMG_WIRE_ERROR_RESOURCE, error->resource);
	fsimg_wire_put64(raw + FSIMG_WIRE_ERROR_OUTPUT, error->output_state);
	fsimg_wire_put64(raw + FSIMG_WIRE_ERROR_OFFSET, error->offset);
	fsimg_wire_put64(raw + FSIMG_WIRE_ERROR_LIMIT, error->limit);
	fsimg_wire_put64(raw + FSIMG_WIRE_ERROR_OBSERVED, error->observed);
	fsimg_wire_put64(raw + FSIMG_WIRE_ERROR_FLAGS, flags);
	fsimg_wire_put64(raw + FSIMG_WIRE_ERROR_CLEANUP, (unsigned)error->cleanup_error);
	/* Only string contents cross the boundary; unused diagnostic storage may
	 * contain bytes from an earlier error even when its current string is short.
	 */
	bcopy(error->path, raw + FSIMG_WIRE_ERROR_PATH, path_length);
	bcopy(error->message, raw + FSIMG_WIRE_ERROR_MESSAGE, message_length);
}

fsimg_status_t
fsimg_wire_error_decode(const unsigned char *raw, uint32_t length,
    fsimg_error_t *error)
{
	fsimg_error_t decoded;
	uint64_t code, flags;

	if (length != FSIMG_WIRE_ERROR_BYTES)
		return (protocol_error(error, "invalid worker error frame length"));
	code = fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_STATUS);
	flags = fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_FLAGS);
	if (code < (uint64_t)(int64_t)FSIMG_EDURABILITY ||
	    fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_OS_ERROR) > INT32_MAX ||
	    fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_OPERATION) > FSIMG_OP_EXTRACT ||
	    fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_RESOURCE) > FSIMG_RESOURCE_PATH ||
	    fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_OUTPUT) > FSIMG_OUTPUT_UNCERTAIN ||
	    flags > 7 ||
	    fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_CLEANUP) > INT32_MAX ||
	    memchr(raw + FSIMG_WIRE_ERROR_PATH, '\0', 256) == NULL ||
	    memchr(raw + FSIMG_WIRE_ERROR_MESSAGE, '\0', 256) == NULL)
		return (protocol_error(error, "invalid worker error fields"));
	bzero(&decoded, sizeof(decoded));
	decoded.status = (fsimg_status_t)(-1 - (int64_t)(UINT64_MAX - code));
	decoded.os_error = (int)fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_OS_ERROR);
	decoded.operation = (fsimg_operation_t)
	    fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_OPERATION);
	decoded.resource = (fsimg_resource_t)
	    fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_RESOURCE);
	decoded.output_state = (fsimg_output_state_t)
	    fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_OUTPUT);
	decoded.offset = fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_OFFSET);
	decoded.limit = fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_LIMIT);
	decoded.observed = fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_OBSERVED);
	decoded.offset_valid = (flags & 1) != 0;
	decoded.path_truncated = (flags & 2) != 0;
	decoded.message_truncated = (flags & 4) != 0;
	decoded.cleanup_error = (int)fsimg_wire_get64(raw + FSIMG_WIRE_ERROR_CLEANUP);
	bcopy(raw + FSIMG_WIRE_ERROR_PATH, decoded.path, 256);
	bcopy(raw + FSIMG_WIRE_ERROR_MESSAGE, decoded.message, 256);
	if (error != NULL)
		*error = decoded;
	return (decoded.status);
}

static bool
time_field_valid(const unsigned char *raw)
{
	uint64_t basis, precision, nanoseconds;
	int64_t offset;

	basis = fsimg_wire_get64(raw + FSIMG_WIRE_TIME_BASIS);
	precision = fsimg_wire_get64(raw + FSIMG_WIRE_TIME_PRECISION);
	nanoseconds = fsimg_wire_get64(raw + FSIMG_WIRE_TIME_NANOSECONDS);
	offset = fsimg_wire_get_signed64(raw + FSIMG_WIRE_TIME_OFFSET);
	if (basis < FSIMG_TIME_UTC || basis > FSIMG_TIME_WALL)
		return (false);
	if (precision == 0 || nanoseconds >= 1000000000)
		return (false);
	if (offset <= -1440 || offset >= 1440)
		return (false);
	return (basis == FSIMG_TIME_OFFSET || offset == 0);
}

static bool
entry_metadata_valid(const unsigned char *raw)
{
	uint64_t valid, required, attributes;
	const unsigned char *time;

	valid = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_VALID);
	required = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_REQUIRED);
	attributes = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_ATTRIBUTES);
	if (valid > 15 || (required & ~valid) != 0 || attributes > 15)
		return (false);
	for (unsigned i = 0; i < 3; i++) {
		if ((valid & (1u << i)) == 0)
			continue;
		time = raw + FSIMG_WIRE_ENTRY_CREATED + i * FSIMG_WIRE_TIME_BYTES;
		if (!time_field_valid(time))
			return (false);
	}
	return (true);
}

bool
fsimg_wire_info_frame_valid(const fsimg_frame_t *frame, uint64_t volume_limit)
{
	const unsigned char *raw = frame->payload;
	uint64_t valid, capabilities, basis, scope;

	if (frame->length != FSIMG_WIRE_INFO_BYTES)
		return (false);
	valid = fsimg_wire_get64(raw + FSIMG_WIRE_INFO_VALID);
	capabilities = fsimg_wire_get64(raw + FSIMG_WIRE_INFO_CAPABILITIES);
	basis = fsimg_wire_get64(raw + FSIMG_WIRE_INFO_TIME_BASIS);
	scope = fsimg_wire_get64(raw + FSIMG_WIRE_INFO_SCOPE);
	if (valid > 7 || capabilities > 7 || basis < FSIMG_TIME_UTC ||
	    basis > FSIMG_TIME_PER_TIMESTAMP)
		return (false);
	if (scope < FSIMG_VALIDATION_HEADER || scope > FSIMG_VALIDATION_PAYLOAD)
		return (false);

	if (fsimg_wire_get64(raw + FSIMG_WIRE_INFO_VOLUME) > volume_limit)
		return (false);
	if (fsimg_wire_get64(raw + FSIMG_WIRE_INFO_SECTOR) > UINT32_MAX ||
	    fsimg_wire_get64(raw + FSIMG_WIRE_INFO_CLUSTER) > UINT32_MAX)
		return (false);
	if (fsimg_wire_get64(raw + FSIMG_WIRE_INFO_CLEAN) > 1 ||
	    fsimg_wire_get64(raw + FSIMG_WIRE_INFO_IO_ERROR) > 1)
		return (false);
	return (memchr(raw + FSIMG_WIRE_INFO_FORMAT, '\0', 16) != NULL &&
	    memchr(raw + FSIMG_WIRE_INFO_PROFILE, '\0', 40) != NULL &&
	    memchr(raw + FSIMG_WIRE_INFO_LABEL, '\0', 48) != NULL);
}

bool
fsimg_wire_plan_frame_valid(const fsimg_frame_t *frame)
{
	const unsigned char *raw = frame->payload;

	if (frame->length != FSIMG_WIRE_PLAN_BYTES)
		return (false);
	if (fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_VOLUME) == 0)
		return (false);
	if (fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_SECTOR) > UINT32_MAX ||
	    fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_CLUSTER) > UINT32_MAX)
		return (false);
	if (fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_IO_FLAGS) > 7)
		return (false);
	return (memchr(raw + FSIMG_WIRE_PLAN_FORMAT, '\0', 16) != NULL);
}

bool
fsimg_wire_verify_frame_valid(const fsimg_frame_t *frame, bool payload)
{
	uint64_t expected_scope;

	if (frame->length != FSIMG_WIRE_VERIFY_BYTES)
		return (false);
	expected_scope = payload ? FSIMG_VALIDATION_PAYLOAD : FSIMG_VALIDATION_STRUCTURAL;
	return (fsimg_wire_get64(frame->payload + FSIMG_WIRE_VERIFY_SCOPE) == expected_scope &&
	    fsimg_wire_get64(frame->payload + FSIMG_WIRE_VERIFY_COMPLETE) == 1);
}

bool
fsimg_wire_entry_frame_valid(const fsimg_frame_t *frame, bool file_only, uint64_t path_limit)
{
	const unsigned char *raw = frame->payload;
	uint64_t entry_kind, name_length;

	if (frame->length != FSIMG_WIRE_ENTRY_BYTES)
		return (false);
	entry_kind = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_KIND);
	name_length = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_NAME_LENGTH);
	if (entry_kind < FSIMG_ENTRY_FILE || entry_kind > FSIMG_ENTRY_DIRECTORY)
		return (false);
	if (name_length == 0 || name_length >= path_limit)
		return (false);
	if (entry_kind == FSIMG_ENTRY_DIRECTORY &&
	    fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_LENGTH) != 0)
		return (false);
	if (file_only && entry_kind != FSIMG_ENTRY_FILE)
		return (false);
	return (entry_metadata_valid(raw));
}
