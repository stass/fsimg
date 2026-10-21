/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_PROTOCOL_H
#define FSIMG_PROTOCOL_H

#include "fsimg.h"

#define FSIMG_FRAME_HEADER 16u
#define FSIMG_FRAME_BYTES 16384u
#define FSIMG_JOB_BYTES 816u
#define FSIMG_WIRE_ERROR_BYTES 592u

/* Result layouts use byte offsets into explicitly encoded buffers. These
 * definitions are shared by the worker, supervisor, and command formatter.
 */
typedef enum fsimg_wire_error_field {
	FSIMG_WIRE_ERROR_STATUS = 0,
	FSIMG_WIRE_ERROR_OS_ERROR = 8,
	FSIMG_WIRE_ERROR_OPERATION = 16,
	FSIMG_WIRE_ERROR_RESOURCE = 24,
	FSIMG_WIRE_ERROR_OUTPUT = 32,
	FSIMG_WIRE_ERROR_OFFSET = 40,
	FSIMG_WIRE_ERROR_LIMIT = 48,
	FSIMG_WIRE_ERROR_OBSERVED = 56,
	FSIMG_WIRE_ERROR_FLAGS = 64,
	FSIMG_WIRE_ERROR_CLEANUP = 72,
	FSIMG_WIRE_ERROR_PATH = 80,
	FSIMG_WIRE_ERROR_MESSAGE = 336
} fsimg_wire_error_field_t;

#define FSIMG_WIRE_INFO_BYTES 216u
typedef enum fsimg_wire_info_field {
	FSIMG_WIRE_INFO_VALID = 0,
	FSIMG_WIRE_INFO_CAPABILITIES = 8,
	FSIMG_WIRE_INFO_VOLUME = 16,
	FSIMG_WIRE_INFO_UNITS = 24,
	FSIMG_WIRE_INFO_FREE = 32,
	FSIMG_WIRE_INFO_SECTOR = 40,
	FSIMG_WIRE_INFO_CLUSTER = 48,
	FSIMG_WIRE_INFO_TIME_BASIS = 56,
	FSIMG_WIRE_INFO_SCOPE = 64,
	FSIMG_WIRE_INFO_CLEAN = 72,
	FSIMG_WIRE_INFO_IO_ERROR = 80,
	FSIMG_WIRE_INFO_FORMAT = 88,
	FSIMG_WIRE_INFO_PROFILE = 104,
	FSIMG_WIRE_INFO_LABEL = 144,	/* 48 bytes and a NUL, then zeros */
	FSIMG_WIRE_INFO_UUID = 200
} fsimg_wire_info_field_t;
#define FSIMG_WIRE_INFO_LABEL_BYTES 49u
_Static_assert(FSIMG_WIRE_INFO_UUID + 16 == FSIMG_WIRE_INFO_BYTES,
    "the info record ends with the volume UUID");

/* The image a read job opened: the layers around its volume. STACK is the
 * canonical text of the forced and detected slots with the selected
 * volume's filesystem, SCHEME the partition scheme or empty, CONTAINER the
 * container the disk was peeled from or empty, with its chunk count, its
 * chunk encodings (CODECS) and its identifier, and REASON why no volume was
 * selected when UNSELECTED is set. DISK is the disk the container yields,
 * or the input file's length without one.
 */
#define FSIMG_WIRE_IMAGE_BYTES 376u
#define FSIMG_WIRE_IMAGE_UNSELECTED 0x1u	/* i only: no volume follows */
#define FSIMG_WIRE_IMAGE_VERIFIED 0x2u		/* W only: layers verified */
typedef enum fsimg_wire_image_field {
	FSIMG_WIRE_IMAGE_FLAGS = 0,
	FSIMG_WIRE_IMAGE_DISK = 8,
	FSIMG_WIRE_IMAGE_SECTOR = 16,
	FSIMG_WIRE_IMAGE_SLOTS = 24,
	FSIMG_WIRE_IMAGE_SELECTED = 32,
	FSIMG_WIRE_IMAGE_DISK_ID = 40,
	FSIMG_WIRE_IMAGE_CHECKED = 48,	/* Used partitions verified */
	FSIMG_WIRE_IMAGE_DISK_GUID = 56,
	FSIMG_WIRE_IMAGE_STACK = 72,
	FSIMG_WIRE_IMAGE_SCHEME = 136,
	FSIMG_WIRE_IMAGE_CONTAINER = 152,
	FSIMG_WIRE_IMAGE_CODECS = 168,
	FSIMG_WIRE_IMAGE_CONTAINER_GUID = 200,
	FSIMG_WIRE_IMAGE_CHUNKS = 216,
	/* The file the layers were peeled from: the input or the spool. A
	 * created image's FILE is its planned DISK, before the compression
	 * COMPRESSION names, as a reader would peel it from its spool.
	 */
	FSIMG_WIRE_IMAGE_FILE = 224,
	FSIMG_WIRE_IMAGE_COMPRESSION = 232,
	FSIMG_WIRE_IMAGE_REASON = 248
} fsimg_wire_image_field_t;
#define FSIMG_WIRE_IMAGE_STACK_BYTES 64u
#define FSIMG_WIRE_IMAGE_CODECS_BYTES 32u
#define FSIMG_WIRE_IMAGE_REASON_BYTES 128u
_Static_assert(FSIMG_WIRE_IMAGE_REASON + FSIMG_WIRE_IMAGE_REASON_BYTES ==
    FSIMG_WIRE_IMAGE_BYTES, "the image record ends with the reason");

/* One used partition-table slot, for i. */
#define FSIMG_WIRE_PARTITION_BYTES 224u
#define FSIMG_WIRE_PARTITION_ACTIVE 0x1u
typedef enum fsimg_wire_partition_field {
	FSIMG_WIRE_PARTITION_INDEX = 0,
	FSIMG_WIRE_PARTITION_FLAGS = 8,
	FSIMG_WIRE_PARTITION_OFFSET = 16,
	FSIMG_WIRE_PARTITION_LENGTH = 24,
	FSIMG_WIRE_PARTITION_MBR_TYPE = 32,
	FSIMG_WIRE_PARTITION_CLAIM = 40,
	FSIMG_WIRE_PARTITION_TYPE = 48,
	FSIMG_WIRE_PARTITION_FORMAT = 64,
	FSIMG_WIRE_PARTITION_TYPE_GUID = 80,
	FSIMG_WIRE_PARTITION_GUID = 96,
	FSIMG_WIRE_PARTITION_NAME = 112
} fsimg_wire_partition_field_t;
#define FSIMG_WIRE_PARTITION_NAME_BYTES 112u
_Static_assert(FSIMG_WIRE_PARTITION_NAME + FSIMG_WIRE_PARTITION_NAME_BYTES ==
    FSIMG_WIRE_PARTITION_BYTES, "the partition record ends with the name");

/* OUTPUT is the most bytes the image can take: the volume or disk, or the
 * largest encoding of the disk by its container and compression. PADDING
 * is the zero padding the container's disk granularity added, and CODEC
 * the compression library the output depends on, empty for none.
 */
#define FSIMG_WIRE_PLAN_BYTES 160u
#define FSIMG_WIRE_PLAN_CODEC_BYTES 32u
typedef enum fsimg_wire_plan_field {
	FSIMG_WIRE_PLAN_VOLUME = 0,
	FSIMG_WIRE_PLAN_PAYLOAD = 8,
	FSIMG_WIRE_PLAN_INITIALIZED = 16,
	FSIMG_WIRE_PLAN_ALLOCATED = 24,
	FSIMG_WIRE_PLAN_METADATA = 32,
	FSIMG_WIRE_PLAN_FREE = 40,
	FSIMG_WIRE_PLAN_ENTRIES = 48,
	FSIMG_WIRE_PLAN_LOSSES = 56,
	FSIMG_WIRE_PLAN_SCRATCH = 64,
	FSIMG_WIRE_PLAN_SECTOR = 72,
	FSIMG_WIRE_PLAN_CLUSTER = 80,
	FSIMG_WIRE_PLAN_IO_FLAGS = 88,
	FSIMG_WIRE_PLAN_FORMAT = 96,
	FSIMG_WIRE_PLAN_OUTPUT = 112,
	FSIMG_WIRE_PLAN_PADDING = 120,
	FSIMG_WIRE_PLAN_CODEC = 128
} fsimg_wire_plan_field_t;
_Static_assert(FSIMG_WIRE_PLAN_CODEC + FSIMG_WIRE_PLAN_CODEC_BYTES ==
    FSIMG_WIRE_PLAN_BYTES, "the plan record ends with the codec");

#define FSIMG_WIRE_ENTRY_BYTES 232u
typedef enum fsimg_wire_entry_field {
	FSIMG_WIRE_ENTRY_KIND = 0,
	FSIMG_WIRE_ENTRY_LENGTH = 8,
	FSIMG_WIRE_ENTRY_NAME_LENGTH = 16,
	FSIMG_WIRE_ENTRY_VALID = 24,
	FSIMG_WIRE_ENTRY_REQUIRED = 32,
	FSIMG_WIRE_ENTRY_ATTRIBUTES = 40,
	FSIMG_WIRE_ENTRY_CREATED = 48,
	FSIMG_WIRE_ENTRY_MODIFIED = 88,
	FSIMG_WIRE_ENTRY_ACCESSED = 128,
	FSIMG_WIRE_ENTRY_CHANGED = 168,
	FSIMG_WIRE_ENTRY_UID = 208,
	FSIMG_WIRE_ENTRY_GID = 216,
	FSIMG_WIRE_ENTRY_MODE = 224
} fsimg_wire_entry_field_t;
_Static_assert(FSIMG_WIRE_ENTRY_MODE + 8 == FSIMG_WIRE_ENTRY_BYTES,
    "the entry record ends with the mode");

#define FSIMG_WIRE_TIME_BYTES 40u
typedef enum fsimg_wire_time_field {
	FSIMG_WIRE_TIME_SECONDS = 0,
	FSIMG_WIRE_TIME_PRECISION = 8,
	FSIMG_WIRE_TIME_NANOSECONDS = 16,
	FSIMG_WIRE_TIME_BASIS = 24,
	FSIMG_WIRE_TIME_OFFSET = 32
} fsimg_wire_time_field_t;
_Static_assert(FSIMG_WIRE_ENTRY_CHANGED + FSIMG_WIRE_TIME_BYTES ==
    FSIMG_WIRE_ENTRY_UID, "owner fields follow the last timestamp");

#define FSIMG_WIRE_DONE_BYTES 48u
typedef enum fsimg_wire_done_field {
	FSIMG_WIRE_DONE_ENTRIES = 0,
	FSIMG_WIRE_DONE_LOGICAL = 8,
	FSIMG_WIRE_DONE_LOSSES = 16,
	FSIMG_WIRE_DONE_RAW = 24,
	FSIMG_WIRE_DONE_IMAGE = 32,
	FSIMG_WIRE_DONE_SKIPPED = 40
} fsimg_wire_done_field_t;
_Static_assert(FSIMG_WIRE_DONE_SKIPPED + 8 == FSIMG_WIRE_DONE_BYTES,
    "the completion record ends with the skipped count");

#define FSIMG_WIRE_VERIFY_BYTES 72u
typedef enum fsimg_wire_verify_field {
	FSIMG_WIRE_VERIFY_SCOPE = 0,
	FSIMG_WIRE_VERIFY_COMPLETE = 8,
	FSIMG_WIRE_VERIFY_FILES = 16,
	FSIMG_WIRE_VERIFY_DIRECTORIES = 24,
	FSIMG_WIRE_VERIFY_LOGICAL = 32,
	FSIMG_WIRE_VERIFY_PAYLOAD = 40,
	FSIMG_WIRE_VERIFY_ALLOCATED = 48,
	FSIMG_WIRE_VERIFY_FREE = 56,
	FSIMG_WIRE_VERIFY_BAD_CLUSTERS = 64
} fsimg_wire_verify_field_t;

#define FSIMG_WIRE_LOSS_BYTES 24u
#define FSIMG_WIRE_LOSS_PATH_BYTES 256u
#define FSIMG_WIRE_LOSS_EVENTS 32u
typedef enum fsimg_wire_loss_field {
	FSIMG_WIRE_LOSS_FIELDS = 0,
	FSIMG_WIRE_LOSS_TRUNCATED = 8,
	FSIMG_WIRE_LOSS_LENGTH = 16,
	FSIMG_WIRE_LOSS_PATH = 24
} fsimg_wire_loss_field_t;

#define FSIMG_RELAY_HEADER_BYTES 16u
#define FSIMG_RELAY_STATUS_BYTES 32u
typedef enum fsimg_relay_field {
	FSIMG_RELAY_STREAM = 0,
	FSIMG_RELAY_LENGTH = 8,
	FSIMG_RELAY_STDOUT = 0,
	FSIMG_RELAY_STDERR = 8,
	FSIMG_RELAY_ERRNO = 16,
	FSIMG_RELAY_MARKER = 24
} fsimg_relay_field_t;

typedef enum fsimg_frame_type {
	FSIMG_FRAME_JOB = 1,
	FSIMG_FRAME_SELECTION,
	FSIMG_FRAME_READY,
	FSIMG_FRAME_ACK,
	FSIMG_FRAME_INFO,
	FSIMG_FRAME_PLAN,
	FSIMG_FRAME_ENTRY,
	FSIMG_FRAME_NAME,
	FSIMG_FRAME_ENTRY_END,
	FSIMG_FRAME_PROGRESS,
	FSIMG_FRAME_LOSS,
	FSIMG_FRAME_VERIFY,
	FSIMG_FRAME_ERROR,
	FSIMG_FRAME_DONE,
	FSIMG_FRAME_IMAGE,
	FSIMG_FRAME_PARTITION,
	FSIMG_FRAME_LAST = FSIMG_FRAME_PARTITION
} fsimg_frame_type_t;

typedef struct fsimg_frame {
	fsimg_frame_type_t type;
	uint32_t	 length;
	unsigned char	 payload[FSIMG_FRAME_BYTES];
} fsimg_frame_t;

/* A decoder owns one fixed frame buffer. feed reports consumed input and
 * exposes a frame only when complete; finish rejects a truncated final frame.
 * The caller enforces operation-specific message ordering.
 */
typedef struct fsimg_frame_decoder {
	fsimg_frame_t	 frame;
	unsigned char	 header[FSIMG_FRAME_HEADER];
	size_t		 header_used;
	size_t		 payload_used;
	bool		 complete;
} fsimg_frame_decoder_t;

void fsimg_wire_put64(unsigned char *, uint64_t);
uint64_t fsimg_wire_get64(const unsigned char *);
int64_t fsimg_wire_get_signed64(const unsigned char *);
fsimg_status_t fsimg_frame_header(unsigned char *, fsimg_frame_type_t,
    uint32_t, fsimg_error_t *);
fsimg_status_t fsimg_frame_feed(fsimg_frame_decoder_t *, const void *, size_t,
    size_t *, fsimg_error_t *);
void fsimg_frame_advance(fsimg_frame_decoder_t *);
fsimg_status_t fsimg_frame_finish(const fsimg_frame_decoder_t *, fsimg_error_t *);
fsimg_status_t fsimg_frame_write(int, fsimg_frame_type_t, const void *,
    uint32_t, fsimg_error_t *);
fsimg_status_t fsimg_frame_read(int, fsimg_frame_t *, fsimg_error_t *);
void fsimg_wire_error_encode(unsigned char *, const fsimg_error_t *);
/* Fixed entry and volume records. Absent optional fields encode as zero, so
 * no stale caller bytes cross the boundary. The entry's path follows in NAME
 * frames; its length is the third argument.
 */
void fsimg_wire_entry_encode(unsigned char *, const fsimg_entry_t *, size_t);
void fsimg_wire_info_encode(unsigned char *, const fsimg_volume_info_t *);
void fsimg_wire_partition_encode(unsigned char *, const fsimg_partition_info_t *);
fsimg_status_t fsimg_wire_error_decode(const unsigned char *, uint32_t,
    fsimg_error_t *);

/* Typed field validation is independent of operation-specific message order.
 * An entry frame with content_only set must describe an entry with content:
 * a regular file or a symbolic link.
 */
bool fsimg_wire_info_frame_valid(const fsimg_frame_t *, uint64_t);
/* An image record of an input of the given size: without a container its
 * disk is that size, and a container's disk is what its tables declare. A
 * partition record lies within a disk of the given size, in a table of the
 * given slot count.
 */
bool fsimg_wire_image_frame_valid(const fsimg_frame_t *);
bool fsimg_wire_partition_frame_valid(const fsimg_frame_t *, uint64_t,
    uint64_t);
bool fsimg_wire_plan_frame_valid(const fsimg_frame_t *);
bool fsimg_wire_entry_frame_valid(const fsimg_frame_t *, bool, uint64_t);
bool fsimg_wire_verify_frame_valid(const fsimg_frame_t *, bool);

#endif
