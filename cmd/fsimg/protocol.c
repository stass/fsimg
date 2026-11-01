/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fsimg-endian.h"
#include "internal.h"
#include "layer.h"
#include "protocol.h"

static void encode_time(unsigned char *, const fsimg_timestamp_t *, bool);
static bool time_field_valid(const unsigned char *);
static bool bytes_zero(const unsigned char *, size_t);
static bool entry_metadata_valid(const unsigned char *);
static fsimg_status_t protocol_error(fsimg_error_t *, const char *);
static fsimg_status_t transfer_frame_bytes(int, void *, size_t, bool,
    fsimg_error_t *);
static fsimg_status_t decode_header(fsimg_frame_decoder_t *, fsimg_error_t *);

/* A signed wire field is the two's-complement bit pattern of its value,
 * little-endian. Each cast's operand is representable, so no host's
 * conversion of an out-of-range value takes part, INT64_MIN included.
 */
int64_t
fsimg_wire_get_signed64(const unsigned char *bytes)
{
	uint64_t value;

	value = fsimg_load_le64(bytes);
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
	    type > FSIMG_FRAME_LAST)
		return (protocol_error(error, "invalid worker frame type or length"));
	bzero(header, FSIMG_FRAME_HEADER);
	bcopy("FSIP", header, 4);
	header[4] = 1;
	header[5] = (unsigned char)type;
	fsimg_store_le64(header + 8, length);
	return (FSIMG_OK);
}

static fsimg_status_t
decode_header(fsimg_frame_decoder_t *decoder, fsimg_error_t *error)
{
	uint64_t length;

	length = fsimg_load_le64(decoder->header + 8);
	if (bcmp(decoder->header, "FSIP", 4) != 0 || decoder->header[4] != 1 ||
	    decoder->header[6] != 0 || decoder->header[7] != 0 ||
	    decoder->header[5] < FSIMG_FRAME_JOB || decoder->header[5] > FSIMG_FRAME_LAST ||
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
	fsimg_store_le64(raw + FSIMG_WIRE_ERROR_STATUS,
	    (uint64_t)(int64_t)error->status);
	fsimg_store_le64(raw + FSIMG_WIRE_ERROR_OS_ERROR, (unsigned)error->os_error);
	fsimg_store_le64(raw + FSIMG_WIRE_ERROR_OPERATION, error->operation);
	fsimg_store_le64(raw + FSIMG_WIRE_ERROR_RESOURCE, error->resource);
	fsimg_store_le64(raw + FSIMG_WIRE_ERROR_OUTPUT, error->output_state);
	fsimg_store_le64(raw + FSIMG_WIRE_ERROR_OFFSET, error->offset);
	fsimg_store_le64(raw + FSIMG_WIRE_ERROR_LIMIT, error->limit);
	fsimg_store_le64(raw + FSIMG_WIRE_ERROR_OBSERVED, error->observed);
	fsimg_store_le64(raw + FSIMG_WIRE_ERROR_FLAGS, flags);
	fsimg_store_le64(raw + FSIMG_WIRE_ERROR_CLEANUP, (unsigned)error->cleanup_error);
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
	code = fsimg_load_le64(raw + FSIMG_WIRE_ERROR_STATUS);
	flags = fsimg_load_le64(raw + FSIMG_WIRE_ERROR_FLAGS);
	if (code < (uint64_t)(int64_t)FSIMG_EDURABILITY ||
	    fsimg_load_le64(raw + FSIMG_WIRE_ERROR_OS_ERROR) > INT32_MAX ||
	    fsimg_load_le64(raw + FSIMG_WIRE_ERROR_OPERATION) > FSIMG_OP_IMAGE ||
	    fsimg_load_le64(raw + FSIMG_WIRE_ERROR_RESOURCE) > FSIMG_RESOURCE_SPOOL ||
	    fsimg_load_le64(raw + FSIMG_WIRE_ERROR_OUTPUT) > FSIMG_OUTPUT_UNCERTAIN ||
	    flags > 7 ||
	    fsimg_load_le64(raw + FSIMG_WIRE_ERROR_CLEANUP) > INT32_MAX ||
	    memchr(raw + FSIMG_WIRE_ERROR_PATH, '\0', 256) == NULL ||
	    memchr(raw + FSIMG_WIRE_ERROR_MESSAGE, '\0', 256) == NULL)
		return (protocol_error(error, "invalid worker error fields"));
	bzero(&decoded, sizeof(decoded));
	decoded.status = (fsimg_status_t)
	    fsimg_wire_get_signed64(raw + FSIMG_WIRE_ERROR_STATUS);
	decoded.os_error = (int)fsimg_load_le64(raw + FSIMG_WIRE_ERROR_OS_ERROR);
	decoded.operation = (fsimg_operation_t)
	    fsimg_load_le64(raw + FSIMG_WIRE_ERROR_OPERATION);
	decoded.resource = (fsimg_resource_t)
	    fsimg_load_le64(raw + FSIMG_WIRE_ERROR_RESOURCE);
	decoded.output_state = (fsimg_output_state_t)
	    fsimg_load_le64(raw + FSIMG_WIRE_ERROR_OUTPUT);
	decoded.offset = fsimg_load_le64(raw + FSIMG_WIRE_ERROR_OFFSET);
	decoded.limit = fsimg_load_le64(raw + FSIMG_WIRE_ERROR_LIMIT);
	decoded.observed = fsimg_load_le64(raw + FSIMG_WIRE_ERROR_OBSERVED);
	decoded.offset_valid = (flags & 1) != 0;
	decoded.path_truncated = (flags & 2) != 0;
	decoded.message_truncated = (flags & 4) != 0;
	decoded.cleanup_error = (int)fsimg_load_le64(raw + FSIMG_WIRE_ERROR_CLEANUP);
	bcopy(raw + FSIMG_WIRE_ERROR_PATH, decoded.path, 256);
	bcopy(raw + FSIMG_WIRE_ERROR_MESSAGE, decoded.message, 256);
	if (error != NULL)
		*error = decoded;
	return (decoded.status);
}

static void
encode_time(unsigned char *raw, const fsimg_timestamp_t *time, bool valid)
{
	bzero(raw, FSIMG_WIRE_TIME_BYTES);
	if (!valid)
		return;
	fsimg_store_le64(raw + FSIMG_WIRE_TIME_SECONDS, (uint64_t)time->seconds);
	fsimg_store_le64(raw + FSIMG_WIRE_TIME_PRECISION, time->precision_ns);
	fsimg_store_le64(raw + FSIMG_WIRE_TIME_NANOSECONDS, time->nanoseconds);
	fsimg_store_le64(raw + FSIMG_WIRE_TIME_BASIS, time->basis);
	fsimg_store_le64(raw + FSIMG_WIRE_TIME_OFFSET,
	    (uint64_t)(int64_t)time->offset_minutes);
}

void
fsimg_wire_entry_encode(unsigned char *raw, const fsimg_entry_t *entry,
    size_t path_length)
{
	const fsimg_metadata_t *metadata = &entry->metadata;

	bzero(raw, FSIMG_WIRE_ENTRY_BYTES);
	fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_KIND, entry->kind);
	fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_LENGTH, entry->size);
	fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_NAME_LENGTH, path_length);
	fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_VALID, metadata->valid);
	fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_REQUIRED, metadata->required);
	if ((metadata->valid & FSIMG_META_ATTRIBUTES) != 0)
		fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_ATTRIBUTES,
		    metadata->attributes);
	encode_time(raw + FSIMG_WIRE_ENTRY_CREATED, &metadata->created,
	    (metadata->valid & FSIMG_META_CREATED) != 0);
	encode_time(raw + FSIMG_WIRE_ENTRY_MODIFIED, &metadata->modified,
	    (metadata->valid & FSIMG_META_MODIFIED) != 0);
	encode_time(raw + FSIMG_WIRE_ENTRY_ACCESSED, &metadata->accessed,
	    (metadata->valid & FSIMG_META_ACCESSED) != 0);
	encode_time(raw + FSIMG_WIRE_ENTRY_CHANGED, &metadata->changed,
	    (metadata->valid & FSIMG_META_CHANGED) != 0);
	if ((metadata->valid & FSIMG_META_OWNER) != 0) {
		fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_UID, metadata->uid);
		fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_GID, metadata->gid);
	}
	if ((metadata->valid & FSIMG_META_MODE) != 0)
		fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_MODE, metadata->mode);
}

_Static_assert(sizeof(((fsimg_volume_info_t *)NULL)->label) ==
    FSIMG_WIRE_INFO_LABEL_BYTES, "the wire label holds the public label");
_Static_assert(sizeof(((fsimg_volume_info_t *)NULL)->mountpoint) ==
    FSIMG_WIRE_INFO_MOUNTPOINT_BYTES,
    "the wire mountpoint holds the public mountpoint");
_Static_assert(sizeof(((fsimg_volume_detail_t *)NULL)->name) ==
    FSIMG_WIRE_INFO_DETAIL_NAME_BYTES,
    "the wire detail name holds the public detail name");
_Static_assert(sizeof(((fsimg_layer_report_t *)NULL)->unverified) ==
    FSIMG_WIRE_IMAGE_UNVERIFIED_BYTES,
    "the wire unverified fields hold the public list");

/* A detail table as the info and image records lay it out: the count at
 * count_offset of raw, then one entry of FSIMG_WIRE_INFO_DETAIL_BYTES per
 * detail from table_offset, its name and then its value, at most capacity
 * of them; raw is zeroed.
 */
void
fsimg_wire_details_encode(unsigned char *raw, size_t count_offset,
    size_t table_offset, uint32_t capacity,
    const fsimg_volume_detail_t *details, uint32_t count)
{
	if (count > capacity)
		count = capacity;
	fsimg_store_le64(raw + count_offset, count);
	for (uint32_t i = 0; i < count; i++) {
		unsigned char *detail = raw + table_offset +
		    i * FSIMG_WIRE_INFO_DETAIL_BYTES;

		bcopy(details[i].name, detail, strnlen(details[i].name,
		    sizeof(details[i].name) - 1));
		fsimg_store_le64(detail + FSIMG_WIRE_INFO_DETAIL_NAME_BYTES,
		    details[i].value);
	}
}

/* Strings are copied up to their terminators, which the reader guarantees
 * inside each fixed array; the zeroed record supplies the wire terminator.
 */
void
fsimg_wire_info_encode(unsigned char *raw, const fsimg_volume_info_t *info)
{
	bzero(raw, FSIMG_WIRE_INFO_BYTES);
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_VALID, info->valid);
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_CAPABILITIES, info->capabilities);
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_VOLUME, info->volume_bytes);
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_UNITS, info->allocation_units);
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_FREE, info->free_bytes);
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_SECTOR, info->sector_bytes);
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_CLUSTER, info->allocation_unit_bytes);
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_TIME_BASIS, info->time_basis);
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_SCOPE, info->scope);
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_CLEAN, info->clean ? 1 : 0);
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_IO_ERROR, info->io_error ? 1 : 0);
	bcopy(info->format, raw + FSIMG_WIRE_INFO_FORMAT,
	    strnlen(info->format, sizeof(info->format) - 1));
	bcopy(info->profile, raw + FSIMG_WIRE_INFO_PROFILE,
	    strnlen(info->profile, sizeof(info->profile) - 1));
	bcopy(info->label, raw + FSIMG_WIRE_INFO_LABEL,
	    strnlen(info->label, sizeof(info->label) - 1));
	if ((info->valid & FSIMG_VOLUME_UUID) != 0)
		bcopy(info->uuid, raw + FSIMG_WIRE_INFO_UUID, sizeof(info->uuid));
	if ((info->valid & FSIMG_VOLUME_MOUNTPOINT) != 0)
		bcopy(info->mountpoint, raw + FSIMG_WIRE_INFO_MOUNTPOINT,
		    strnlen(info->mountpoint, sizeof(info->mountpoint) - 1));
	fsimg_wire_details_encode(raw, FSIMG_WIRE_INFO_DETAIL_COUNT,
	    FSIMG_WIRE_INFO_DETAILS, FSIMG_VOLUME_DETAILS_MAX, info->details,
	    info->detail_count);
}

/* Only a used slot crosses the boundary, so the record has no used flag. */
void
fsimg_wire_partition_encode(unsigned char *raw, const fsimg_partition_info_t *info)
{
	bzero(raw, FSIMG_WIRE_PARTITION_BYTES);
	fsimg_store_le64(raw + FSIMG_WIRE_PARTITION_INDEX, info->index);
	fsimg_store_le64(raw + FSIMG_WIRE_PARTITION_FLAGS,
	    info->active ? FSIMG_WIRE_PARTITION_ACTIVE : 0);
	fsimg_store_le64(raw + FSIMG_WIRE_PARTITION_OFFSET, info->offset);
	fsimg_store_le64(raw + FSIMG_WIRE_PARTITION_LENGTH, info->length);
	fsimg_store_le64(raw + FSIMG_WIRE_PARTITION_MBR_TYPE, info->mbr_type);
	fsimg_store_le64(raw + FSIMG_WIRE_PARTITION_CLAIM, info->claim);
	bcopy(info->type, raw + FSIMG_WIRE_PARTITION_TYPE,
	    strnlen(info->type, sizeof(info->type) - 1));
	bcopy(info->format, raw + FSIMG_WIRE_PARTITION_FORMAT,
	    strnlen(info->format, sizeof(info->format) - 1));
	bcopy(info->type_guid, raw + FSIMG_WIRE_PARTITION_TYPE_GUID,
	    sizeof(info->type_guid));
	bcopy(info->guid, raw + FSIMG_WIRE_PARTITION_GUID, sizeof(info->guid));
	bcopy(info->name, raw + FSIMG_WIRE_PARTITION_NAME,
	    strnlen(info->name, FSIMG_WIRE_PARTITION_NAME_BYTES - 1));
}

static bool
time_field_valid(const unsigned char *raw)
{
	uint64_t basis, precision, nanoseconds;
	int64_t offset;

	basis = fsimg_load_le64(raw + FSIMG_WIRE_TIME_BASIS);
	precision = fsimg_load_le64(raw + FSIMG_WIRE_TIME_PRECISION);
	nanoseconds = fsimg_load_le64(raw + FSIMG_WIRE_TIME_NANOSECONDS);
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
bytes_zero(const unsigned char *raw, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		if (raw[i] != 0)
			return (false);
	}
	return (true);
}

/* Every field has the range its public type and documented bits allow;
 * an absent field must be zero, as the encoder writes it.
 */
static bool
entry_metadata_valid(const unsigned char *raw)
{
	static const struct {
		uint32_t	 field;
		unsigned	 offset;
	} times[] = {
		{ FSIMG_META_CREATED, FSIMG_WIRE_ENTRY_CREATED },
		{ FSIMG_META_MODIFIED, FSIMG_WIRE_ENTRY_MODIFIED },
		{ FSIMG_META_ACCESSED, FSIMG_WIRE_ENTRY_ACCESSED },
		{ FSIMG_META_CHANGED, FSIMG_WIRE_ENTRY_CHANGED }
	};
	uint64_t valid, required, attributes, uid, gid, mode;

	valid = fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_VALID);
	required = fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_REQUIRED);
	attributes = fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_ATTRIBUTES);
	uid = fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_UID);
	gid = fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_GID);
	mode = fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_MODE);
	if ((valid & ~(uint64_t)FSIMG_META_KNOWN) != 0 || (required & ~valid) != 0 ||
	    attributes > 15 || uid > UINT32_MAX || gid > UINT32_MAX ||
	    mode > FSIMG_MODE_BITS)
		return (false);
	if ((valid & FSIMG_META_OWNER) == 0 && (uid != 0 || gid != 0))
		return (false);
	if ((valid & FSIMG_META_MODE) == 0 && mode != 0)
		return (false);
	if ((valid & FSIMG_META_ATTRIBUTES) == 0 && attributes != 0)
		return (false);
	for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
		if ((valid & times[i].field) == 0 ?
		    !bytes_zero(raw + times[i].offset, FSIMG_WIRE_TIME_BYTES) :
		    !time_field_valid(raw + times[i].offset))
			return (false);
	}
	return (true);
}

/* A NUL-terminated string field of length bytes: the terminator lies in
 * the field and every byte after it is zero, as the encoder writes it.
 */
static bool
string_field_valid(const unsigned char *raw, size_t length)
{
	const unsigned char *nul = memchr(raw, '\0', length);

	return (nul != NULL && bytes_zero(nul, length - (size_t)(nul - raw)));
}

/* A string field holding printable ASCII without blanks, possibly empty. */
static bool
word_field_valid(const unsigned char *raw, size_t length)
{
	if (!string_field_valid(raw, length))
		return (false);
	for (size_t i = 0; raw[i] != '\0'; i++) {
		if (raw[i] <= 0x20 || raw[i] >= 0x7f)
			return (false);
	}
	return (true);
}

/* A detail table laid out as fsimg_wire_details_encode() writes it: at
 * most capacity used entries, each a nonempty name of printable ASCII and
 * its value, and zeros after them.
 */
static bool
details_valid(const unsigned char *raw, size_t count_offset,
    size_t table_offset, uint32_t capacity)
{
	uint64_t count = fsimg_load_le64(raw + count_offset);

	if (count > capacity)
		return (false);
	for (uint64_t i = 0; i < capacity; i++) {
		const unsigned char *detail = raw + table_offset +
		    i * FSIMG_WIRE_INFO_DETAIL_BYTES;

		if (i >= count) {
			if (!bytes_zero(detail, FSIMG_WIRE_INFO_DETAIL_BYTES))
				return (false);
			continue;
		}
		if (detail[0] == '\0' || !word_field_valid(detail,
		    FSIMG_WIRE_INFO_DETAIL_NAME_BYTES))
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
	valid = fsimg_load_le64(raw + FSIMG_WIRE_INFO_VALID);
	capabilities = fsimg_load_le64(raw + FSIMG_WIRE_INFO_CAPABILITIES);
	basis = fsimg_load_le64(raw + FSIMG_WIRE_INFO_TIME_BASIS);
	scope = fsimg_load_le64(raw + FSIMG_WIRE_INFO_SCOPE);
	if (valid > (FSIMG_VOLUME_GEOMETRY | FSIMG_VOLUME_LABEL | FSIMG_VOLUME_FREE |
	    FSIMG_VOLUME_UUID | FSIMG_VOLUME_MOUNTPOINT) || capabilities > 7 ||
	    basis < FSIMG_TIME_UTC || basis > FSIMG_TIME_PER_TIMESTAMP)
		return (false);
	if (scope < FSIMG_VALIDATION_HEADER || scope > FSIMG_VALIDATION_PAYLOAD)
		return (false);
	if ((valid & FSIMG_VOLUME_UUID) == 0 &&
	    !bytes_zero(raw + FSIMG_WIRE_INFO_UUID, 16))
		return (false);
	if (!string_field_valid(raw + FSIMG_WIRE_INFO_MOUNTPOINT,
	    FSIMG_WIRE_INFO_DETAIL_COUNT - FSIMG_WIRE_INFO_MOUNTPOINT) ||
	    memchr(raw + FSIMG_WIRE_INFO_MOUNTPOINT, '\0',
	    FSIMG_WIRE_INFO_MOUNTPOINT_BYTES) == NULL)
		return (false);
	if ((valid & FSIMG_VOLUME_MOUNTPOINT) == 0 &&
	    raw[FSIMG_WIRE_INFO_MOUNTPOINT] != '\0')
		return (false);
	if (!details_valid(raw, FSIMG_WIRE_INFO_DETAIL_COUNT,
	    FSIMG_WIRE_INFO_DETAILS, FSIMG_VOLUME_DETAILS_MAX))
		return (false);

	if (fsimg_load_le64(raw + FSIMG_WIRE_INFO_VOLUME) > volume_limit)
		return (false);
	if (fsimg_load_le64(raw + FSIMG_WIRE_INFO_SECTOR) > UINT32_MAX ||
	    fsimg_load_le64(raw + FSIMG_WIRE_INFO_CLUSTER) > UINT32_MAX)
		return (false);
	if (fsimg_load_le64(raw + FSIMG_WIRE_INFO_CLEAN) > 1 ||
	    fsimg_load_le64(raw + FSIMG_WIRE_INFO_IO_ERROR) > 1)
		return (false);
	if (!bytes_zero(raw + FSIMG_WIRE_INFO_LABEL + FSIMG_WIRE_INFO_LABEL_BYTES,
	    FSIMG_WIRE_INFO_UUID - FSIMG_WIRE_INFO_LABEL -
	    FSIMG_WIRE_INFO_LABEL_BYTES))
		return (false);
	return (memchr(raw + FSIMG_WIRE_INFO_FORMAT, '\0', 16) != NULL &&
	    memchr(raw + FSIMG_WIRE_INFO_PROFILE, '\0', 40) != NULL &&
	    memchr(raw + FSIMG_WIRE_INFO_LABEL, '\0',
	    FSIMG_WIRE_INFO_LABEL_BYTES) != NULL);
}

/*
 * The compression is none or a registered one the build reads. Without a
 * container the disk is the file the layers were peeled from, so the two
 * lengths match exactly, and every container field is zero; a container
 * is a registered one, and its disk is the length its tables declare,
 * which bounds what follows. Only a container has details. Only a
 * verified image names unverified entries: the compression's own entry
 * first when it has one, then a container's fields, so without a
 * container the list is empty or exactly the compression's entry. Without
 * a scheme every table field is zero;
 * with one, the sector size is GPT's or MBR's and the selection lies in
 * the table. The caller checks the file against the input it knows.
 */
bool
fsimg_wire_image_frame_valid(const fsimg_frame_t *frame)
{
	const unsigned char *raw = frame->payload;
	const fsimg_layer_t *layer;
	const char *unverified = (const char *)raw + FSIMG_WIRE_IMAGE_UNVERIFIED;
	const char *own = "";
	uint64_t flags, sector, slots, selected, checked;
	bool scheme;

	if (frame->length != FSIMG_WIRE_IMAGE_BYTES)
		return (false);
	flags = fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_FLAGS);
	sector = fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_SECTOR);
	slots = fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_SLOTS);
	selected = fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_SELECTED);
	checked = fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_CHECKED);
	if (memchr(raw + FSIMG_WIRE_IMAGE_STACK, '\0', FSIMG_WIRE_IMAGE_STACK_BYTES) == NULL ||
	    memchr(raw + FSIMG_WIRE_IMAGE_SCHEME, '\0', 16) == NULL ||
	    memchr(raw + FSIMG_WIRE_IMAGE_CONTAINER, '\0', 16) == NULL ||
	    memchr(raw + FSIMG_WIRE_IMAGE_CODECS, '\0',
	    FSIMG_WIRE_IMAGE_CODECS_BYTES) == NULL ||
	    memchr(raw + FSIMG_WIRE_IMAGE_COMPRESSION, '\0', 16) == NULL ||
	    memchr(raw + FSIMG_WIRE_IMAGE_REASON, '\0',
	    FSIMG_WIRE_IMAGE_REASON_BYTES) == NULL)
		return (false);
	if (!details_valid(raw, FSIMG_WIRE_IMAGE_DETAIL_COUNT,
	    FSIMG_WIRE_IMAGE_DETAILS, FSIMG_IMAGE_DETAILS_MAX) ||
	    !word_field_valid(raw + FSIMG_WIRE_IMAGE_UNVERIFIED,
	    FSIMG_WIRE_IMAGE_UNVERIFIED_BYTES))
		return (false);
	if (raw[FSIMG_WIRE_IMAGE_COMPRESSION] != '\0') {
		layer = fsimg_layer_find((const char *)raw +
		    FSIMG_WIRE_IMAGE_COMPRESSION);
		if (layer == NULL || layer->compression == NULL)
			return (false);
		if (layer->compression->unverified != NULL)
			own = layer->compression->unverified;
	}
	if ((flags & ~(uint64_t)(FSIMG_WIRE_IMAGE_UNSELECTED |
	    FSIMG_WIRE_IMAGE_VERIFIED)) != 0 ||
	    fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_DISK_ID) > UINT32_MAX ||
	    ((flags & FSIMG_WIRE_IMAGE_VERIFIED) == 0 && unverified[0] != '\0'))
		return (false);
	if (raw[FSIMG_WIRE_IMAGE_CONTAINER] == '\0') {
		if (fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_DISK) !=
		    fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_FILE) ||
		    raw[FSIMG_WIRE_IMAGE_CODECS] != '\0' ||
		    fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_CHUNKS) != 0 ||
		    !bytes_zero(raw + FSIMG_WIRE_IMAGE_CONTAINER_GUID, 16) ||
		    fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_DETAIL_COUNT) != 0 ||
		    (unverified[0] != '\0' && strcmp(unverified, own) != 0))
			return (false);
	} else {
		layer = fsimg_layer_find((const char *)raw + FSIMG_WIRE_IMAGE_CONTAINER);
		if (layer == NULL || layer->container == NULL)
			return (false);
	}
	/* A reason exactly when no volume was selected, and then no slot. */
	if ((flags & FSIMG_WIRE_IMAGE_UNSELECTED) != 0 ?
	    raw[FSIMG_WIRE_IMAGE_REASON] == '\0' || selected != 0 :
	    raw[FSIMG_WIRE_IMAGE_REASON] != '\0')
		return (false);
	scheme = raw[FSIMG_WIRE_IMAGE_SCHEME] != '\0';
	if (!scheme)
		return (sector == 0 && slots == 0 && selected == 0 && checked == 0 &&
		    (flags & FSIMG_WIRE_IMAGE_UNSELECTED) == 0 &&
		    fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_DISK_ID) == 0 &&
		    bytes_zero(raw + FSIMG_WIRE_IMAGE_DISK_GUID, 16));
	layer = fsimg_layer_find((const char *)raw + FSIMG_WIRE_IMAGE_SCHEME);
	/* MBR sectors are 512 bytes; GPT's are 512 or 4096. */
	return (layer != NULL && layer->scheme != NULL && (sector == 512 ||
	    (sector == 4096 && strcmp(layer->name, "gpt") == 0)) && slots != 0 &&
	    slots <= FSIMG_GPT_MAX_ENTRIES && selected <= slots && checked <= slots);
}

/* A used slot of a table with slots entries, lying within the disk. */
bool
fsimg_wire_partition_frame_valid(const fsimg_frame_t *frame, uint64_t slots,
    uint64_t disk_bytes)
{
	const unsigned char *raw = frame->payload;
	uint64_t index, offset, length, claim;
	bool named;

	if (frame->length != FSIMG_WIRE_PARTITION_BYTES)
		return (false);
	index = fsimg_load_le64(raw + FSIMG_WIRE_PARTITION_INDEX);
	offset = fsimg_load_le64(raw + FSIMG_WIRE_PARTITION_OFFSET);
	length = fsimg_load_le64(raw + FSIMG_WIRE_PARTITION_LENGTH);
	claim = fsimg_load_le64(raw + FSIMG_WIRE_PARTITION_CLAIM);
	if (index == 0 || index > slots || length == 0 || offset > disk_bytes ||
	    length > disk_bytes - offset)
		return (false);
	if (fsimg_load_le64(raw + FSIMG_WIRE_PARTITION_FLAGS) >
	    FSIMG_WIRE_PARTITION_ACTIVE ||
	    fsimg_load_le64(raw + FSIMG_WIRE_PARTITION_MBR_TYPE) > UINT8_MAX ||
	    claim > FSIMG_CLAIM_AMBIGUOUS)
		return (false);
	if (memchr(raw + FSIMG_WIRE_PARTITION_TYPE, '\0', 16) == NULL ||
	    memchr(raw + FSIMG_WIRE_PARTITION_FORMAT, '\0', 16) == NULL ||
	    memchr(raw + FSIMG_WIRE_PARTITION_NAME, '\0',
	    FSIMG_WIRE_PARTITION_NAME_BYTES) == NULL)
		return (false);
	named = raw[FSIMG_WIRE_PARTITION_FORMAT] != '\0';
	return (named == (claim == FSIMG_CLAIM_FORMAT));
}

bool
fsimg_wire_plan_frame_valid(const fsimg_frame_t *frame)
{
	const unsigned char *raw = frame->payload;

	if (frame->length != FSIMG_WIRE_PLAN_BYTES)
		return (false);
	if (fsimg_load_le64(raw + FSIMG_WIRE_PLAN_VOLUME) == 0)
		return (false);
	if (fsimg_load_le64(raw + FSIMG_WIRE_PLAN_SECTOR) > UINT32_MAX ||
	    fsimg_load_le64(raw + FSIMG_WIRE_PLAN_CLUSTER) > UINT32_MAX)
		return (false);
	if (fsimg_load_le64(raw + FSIMG_WIRE_PLAN_IO_FLAGS) > 7 ||
	    fsimg_load_le64(raw + FSIMG_WIRE_PLAN_OUTPUT) <
	    fsimg_load_le64(raw + FSIMG_WIRE_PLAN_VOLUME) ||
	    fsimg_load_le64(raw + FSIMG_WIRE_PLAN_FREE_INODES) >
	    fsimg_load_le64(raw + FSIMG_WIRE_PLAN_INODES) ||
	    fsimg_load_le64(raw + FSIMG_WIRE_PLAN_ROOT) > UINT16_MAX)
		return (false);
	return (memchr(raw + FSIMG_WIRE_PLAN_FORMAT, '\0', 16) != NULL &&
	    memchr(raw + FSIMG_WIRE_PLAN_CODEC, '\0',
	    FSIMG_WIRE_PLAN_CODEC_BYTES) != NULL);
}

bool
fsimg_wire_verify_frame_valid(const fsimg_frame_t *frame, bool payload)
{
	uint64_t expected_scope;

	if (frame->length != FSIMG_WIRE_VERIFY_BYTES)
		return (false);
	expected_scope = payload ? FSIMG_VALIDATION_PAYLOAD : FSIMG_VALIDATION_STRUCTURAL;
	return (fsimg_load_le64(frame->payload + FSIMG_WIRE_VERIFY_SCOPE) == expected_scope &&
	    fsimg_load_le64(frame->payload + FSIMG_WIRE_VERIFY_COMPLETE) == 1);
}

/* The kind is one of the closed set, with the size that kind allows:
 * any file length, a link target of 1 through FSIMG_SYMLINK_TARGET_MAX
 * bytes, the entry model's ceiling, and zero for directories and FIFOs.
 */
bool
fsimg_wire_entry_frame_valid(const fsimg_frame_t *frame, bool content_only,
    uint64_t path_limit)
{
	const unsigned char *raw = frame->payload;
	uint64_t entry_kind, name_length, length;
	bool has_content;

	if (frame->length != FSIMG_WIRE_ENTRY_BYTES)
		return (false);
	entry_kind = fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_KIND);
	name_length = fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_NAME_LENGTH);
	length = fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_LENGTH);
	if (name_length == 0 || name_length >= path_limit)
		return (false);
	switch (entry_kind) {
	case FSIMG_ENTRY_FILE:
		has_content = true;
		break;
	case FSIMG_ENTRY_SYMLINK:
		if (length == 0 || length > FSIMG_SYMLINK_TARGET_MAX)
			return (false);
		has_content = true;
		break;
	case FSIMG_ENTRY_DIRECTORY:
	case FSIMG_ENTRY_FIFO:
		if (length != 0)
			return (false);
		has_content = false;
		break;
	default:
		return (false);
	}
	if (content_only && !has_content)
		return (false);
	return (entry_metadata_valid(raw));
}
