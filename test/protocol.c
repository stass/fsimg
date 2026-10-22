/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "test.h"
#include "worker.h"

static void test_frame_splits(uint32_t);
static void test_malformed_frames(void);
static void test_error_codec(void);
static void test_job_label(void);
static void test_selection_budget(void);
static void test_error_padding(void);
static void test_entry_records(void);
static void test_invalid_entry_records(void);
static void test_info_uuid(void);
static void test_info_label(void);
static void test_job_posix_flags(void);
static void test_job_roots(void);
static void test_job_selection(void);
static void test_job_layers(void);
static void test_image_records(void);

/* Create jobs in these tests populate the image root from one root. */
static const char *const image_root[] = { "/" };
static bool entry_frame_valid(const fsimg_entry_t *, bool);
static void write_seed(const char *);

static void
write_seed(const char *path)
{
	fsimg_worker_request_t request;
	fsimg_error_t error = { .status = FSIMG_ECORRUPT };
	unsigned char raw[FSIMG_JOB_BYTES];
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	CHECK(fd != -1);
	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_INFO;
	request.input_fd = 0;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_JOB, raw, FSIMG_JOB_BYTES, NULL) == FSIMG_OK);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_SELECTION, "/", 1, NULL) == FSIMG_OK);

	bzero(raw, sizeof(raw));
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_VOLUME, 4096);
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_TIME_BASIS, FSIMG_TIME_WALL);
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_SCOPE, FSIMG_VALIDATION_HEADER);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_INFO, raw, FSIMG_WIRE_INFO_BYTES, NULL) == FSIMG_OK);
	bzero(raw, sizeof(raw));
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_VOLUME, 4096);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_PLAN, raw, FSIMG_WIRE_PLAN_BYTES, NULL) == FSIMG_OK);
	bzero(raw, sizeof(raw));
	fsimg_wire_put64(raw + FSIMG_WIRE_ENTRY_KIND, FSIMG_ENTRY_FILE);
	fsimg_wire_put64(raw + FSIMG_WIRE_ENTRY_NAME_LENGTH, 2);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_ENTRY, raw, FSIMG_WIRE_ENTRY_BYTES, NULL) == FSIMG_OK);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_NAME, "/x", 2, NULL) == FSIMG_OK);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_ENTRY_END, NULL, 0, NULL) == FSIMG_OK);
	bzero(raw, sizeof(raw));
	fsimg_wire_put64(raw + FSIMG_WIRE_VERIFY_SCOPE, FSIMG_VALIDATION_STRUCTURAL);
	fsimg_wire_put64(raw + FSIMG_WIRE_VERIFY_COMPLETE, 1);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_VERIFY, raw, FSIMG_WIRE_VERIFY_BYTES, NULL) == FSIMG_OK);
	fsimg_wire_error_encode(raw, &error);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_ERROR, raw, FSIMG_WIRE_ERROR_BYTES, NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
}

static void
test_frame_splits(uint32_t length)
{
	fsimg_frame_decoder_t decoder;
	unsigned char encoded[FSIMG_FRAME_HEADER + FSIMG_FRAME_BYTES];
	size_t consumed, remaining;

	CHECK(fsimg_frame_header(encoded, FSIMG_FRAME_NAME, length, NULL) == FSIMG_OK);
	for (uint32_t i = 0; i < length; i++)
		encoded[FSIMG_FRAME_HEADER + i] = (unsigned char)(i * 37);
	for (size_t split = 0; split <= FSIMG_FRAME_HEADER + length; split++) {
		bzero(&decoder, sizeof(decoder));
		CHECK(fsimg_frame_feed(&decoder, encoded, split, &consumed, NULL) == FSIMG_OK);
		CHECK(consumed == split);
		if (split < FSIMG_FRAME_HEADER + length) {
			CHECK(!decoder.complete);
			if (split != 0)
				CHECK(fsimg_frame_finish(&decoder, NULL) == FSIMG_EIO);
		}
		remaining = FSIMG_FRAME_HEADER + length - split;
		if (remaining != 0)
			CHECK(fsimg_frame_feed(&decoder, encoded + split, remaining,
			    &consumed, NULL) == FSIMG_OK);
		CHECK(decoder.complete && decoder.frame.length == length);
		CHECK(decoder.frame.type == FSIMG_FRAME_NAME);
		CHECK(bcmp(decoder.frame.payload, encoded + FSIMG_FRAME_HEADER, length) == 0);
		fsimg_frame_advance(&decoder);
		CHECK(fsimg_frame_finish(&decoder, NULL) == FSIMG_OK);
	}
}

static void
test_malformed_frames(void)
{
	fsimg_frame_decoder_t decoder;
	unsigned char header[FSIMG_FRAME_HEADER];
	size_t consumed;

	for (unsigned scenario = 0; scenario < 6; scenario++) {
		CHECK(fsimg_frame_header(header, FSIMG_FRAME_READY, 0, NULL) == FSIMG_OK);
		switch (scenario) {
		case 0: header[0] = 'X'; break;
		case 1: header[4] = 2; break;
		case 2: header[5] = 0; break;
		case 3: header[6] = 1; break;
		case 4: fsimg_wire_put64(header + 8, FSIMG_FRAME_BYTES + 1); break;
		case 5: fsimg_wire_put64(header + 8, UINT64_MAX); break;
		}
		bzero(&decoder, sizeof(decoder));
		CHECK(fsimg_frame_feed(&decoder, header, sizeof(header), &consumed,
		    NULL) == FSIMG_EIO);
	}
}

static void
test_error_codec(void)
{
	fsimg_error_t error = { .status = FSIMG_ECORRUPT,
	    .operation = FSIMG_OP_READER, .output_state = FSIMG_OUTPUT_INCOMPLETE };
	fsimg_error_t decoded;
	unsigned char raw[FSIMG_WIRE_ERROR_BYTES];

	strcpy(error.path, "/file");
	strcpy(error.message, "corrupt allocation");
	fsimg_wire_error_encode(raw, &error);
	CHECK(fsimg_wire_error_decode(raw, sizeof(raw), &decoded) == FSIMG_ECORRUPT);
	CHECK(strcmp(error.path, decoded.path) == 0);
	CHECK(decoded.output_state == FSIMG_OUTPUT_INCOMPLETE);
	fsimg_wire_put64(raw, 0);
	CHECK(fsimg_wire_error_decode(raw, sizeof(raw), &decoded) == FSIMG_EIO);
	fsimg_wire_put64(raw, (uint64_t)(int64_t)FSIMG_EDURABILITY);
	CHECK(fsimg_wire_error_decode(raw, sizeof(raw), &decoded) == FSIMG_EDURABILITY);
	memset(raw + 336, 'X', 256);
	CHECK(fsimg_wire_error_decode(raw, sizeof(raw), &decoded) == FSIMG_EIO);
}

static void
test_error_padding(void)
{
	fsimg_error_t error = { .status = FSIMG_EIO };
	unsigned char raw[FSIMG_WIRE_ERROR_BYTES];

	memset(error.path, 0xa5, sizeof(error.path));
	memset(error.message, 0xa5, sizeof(error.message));
	strcpy(error.path, "/x");
	strcpy(error.message, "failure");
	fsimg_wire_error_encode(raw, &error);
	for (unsigned i = 83; i < 336; i++)
		CHECK(raw[i] == 0);
	for (unsigned i = 344; i < FSIMG_WIRE_ERROR_BYTES; i++)
		CHECK(raw[i] == 0);
}

static void
test_selection_budget(void)
{
	fsimg_worker_request_t request;
	fsimg_error_t error;
	unsigned char raw[FSIMG_JOB_BYTES];

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_LIST;
	request.selection = "/abc";
	request.limits.max_path_bytes = 4;
	CHECK(fsimg_job_encode(&request, raw, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_PATH);
	CHECK(error.limit == 4 && error.observed >= 5);
}

/* Whether a job record still decodes with one byte replaced. */
static bool
job_decodes(const unsigned char *raw, unsigned offset, unsigned char value)
{
	fsimg_worker_request_t decoded;
	unsigned char copy[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	bcopy(raw, copy, sizeof(copy));
	copy[offset] = value;
	return (fsimg_job_decode(copy, sizeof(copy), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
}

/* Standard input belongs to read jobs, standard output to a
 * creation; the worker refuses each flag on a job of the other side.
 */
static void
test_job_stream_flags(void)
{
	static const struct {
		fsimg_job_kind_t kind;
		unsigned	 flags;
		bool		 valid;
	} cases[] = {
		{ FSIMG_JOB_LIST, FSIMG_JOB_STDIN, true },
		{ FSIMG_JOB_LIST, FSIMG_JOB_STDOUT, false },
		{ FSIMG_JOB_CREATE, FSIMG_JOB_STDOUT, true },
		{ FSIMG_JOB_CREATE, FSIMG_JOB_STDIN, false },
		{ FSIMG_JOB_PLAN, FSIMG_JOB_STDOUT, false },
	};
	/* The compression a job carries must be one its side can apply: a
	 * create writes it and a read decodes it.
	 */
	static const struct {
		fsimg_job_kind_t kind;
		const char	*name;
		bool		 valid;
	} compressions[] = {
		{ FSIMG_JOB_LIST, "gzip", true },
		{ FSIMG_JOB_VERIFY, "gzip", true },
		{ FSIMG_JOB_CREATE, "gzip", true },
		{ FSIMG_JOB_LIST, "zstd", false },
		{ FSIMG_JOB_CREATE, "zstd", false },
		{ FSIMG_JOB_LIST, "dmg", false },
	};
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fsimg_worker_request_init(&request);
		request.kind = cases[i].kind;
		request.flags = cases[i].flags;
		if (fsimg_job_has_roots(request.kind)) {
			request.root_paths = image_root;
			request.root_count = 1;
		}
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK((fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK) == cases[i].valid);
		if (cases[i].valid)
			CHECK(decoded.flags == cases[i].flags);
	}
	for (unsigned i = 0; i < sizeof(compressions) / sizeof(compressions[0]);
	    i++) {
		fsimg_worker_request_init(&request);
		request.kind = compressions[i].kind;
		snprintf(request.compression, sizeof(request.compression), "%s",
		    compressions[i].name);
		if (fsimg_job_has_roots(request.kind)) {
			request.root_paths = image_root;
			request.root_count = 1;
		}
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK((fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK) == compressions[i].valid);
		if (compressions[i].valid)
			CHECK(strcmp(decoded.compression, compressions[i].name) == 0);
	}
	/* A read job's standard input is its input capability. */
	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_LIST;
	request.flags = FSIMG_JOB_STDIN;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK((capabilities & FSIMG_JOB_CAP(FSIMG_CAPABILITY_INPUT)) != 0);
}

static void
test_job_label(void)
{
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	request.write.label = "MYLABEL";
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.write.label != NULL);
	CHECK(strcmp(decoded.write.label, "MYLABEL") == 0);
	CHECK(selection == 2 && capabilities == FSIMG_JOB_CAP_ROOTS);

	/* The longest label any format stores, QNX4's 48 bytes, round-trips;
	 * one byte more does not encode.
	 */
	request.write.label = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV";
	CHECK(strlen(request.write.label) == 48);
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(strcmp(decoded.write.label, request.write.label) == 0);
	request.write.label = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVW";
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_EINVAL);

	/* The worker refuses every other encoding of the strings: a label with
	 * no terminator in its 49 bytes (216 to 264) or a byte after its
	 * terminator, a presence byte (265) other than 0 or 1 or marking a
	 * label absent, nonzero padding before the UUID, and a byte after the
	 * format name's terminator.
	 */
	request.write.label = "MYLABEL";
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	memset(raw + 216 + 7, 'X', 41);
	CHECK(job_decodes(raw, 216 + 48, 0));
	CHECK(!job_decodes(raw, 216 + 48, 'X'));
	request.write.label = "MYLABEL";
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(job_decodes(raw, 216 + 8, 0));
	CHECK(!job_decodes(raw, 216 + 8, 'X'));
	CHECK(!job_decodes(raw, 265, 2));
	CHECK(!job_decodes(raw, 265, 0));
	CHECK(!job_decodes(raw, 271, 1));
	CHECK(!job_decodes(raw, 200 + 15, 'X'));

	/* An explicit empty exFAT label differs from the default selected by NULL. */
	request.write.label = "";
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.write.label != NULL && decoded.write.label[0] == '\0');
	request.write.label = NULL;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK && decoded.write.label == NULL);
}

static bool
entry_frame_valid(const fsimg_entry_t *entry, bool content_only)
{
	fsimg_frame_t frame = { .type = FSIMG_FRAME_ENTRY,
	    .length = FSIMG_WIRE_ENTRY_BYTES };

	fsimg_wire_entry_encode(frame.payload, entry, strlen(entry->path));
	return (fsimg_wire_entry_frame_valid(&frame, content_only, 4096));
}

/* Entry records carry every kind and the POSIX fields; absent fields are
 * encoded as zero whatever the caller's struct holds.
 */
static void
test_entry_records(void)
{
	fsimg_frame_t frame = { .type = FSIMG_FRAME_ENTRY,
	    .length = FSIMG_WIRE_ENTRY_BYTES };
	fsimg_entry_t entry = { .path = "/link", .kind = FSIMG_ENTRY_SYMLINK,
	    .size = 9 };
	const unsigned char *raw = frame.payload;

	entry.metadata.valid = FSIMG_META_CHANGED | FSIMG_META_OWNER | FSIMG_META_MODE;
	entry.metadata.required = FSIMG_META_MODE;
	entry.metadata.uid = 1000;
	entry.metadata.gid = 20;
	entry.metadata.mode = 07777;
	entry.metadata.changed = (fsimg_timestamp_t){ .seconds = -2,
	    .precision_ns = 1, .nanoseconds = 7, .basis = FSIMG_TIME_OFFSET,
	    .offset_minutes = -90 };
	fsimg_wire_entry_encode(frame.payload, &entry, 5);
	CHECK(fsimg_wire_entry_frame_valid(&frame, false, 4096));
	CHECK(fsimg_wire_entry_frame_valid(&frame, true, 4096));
	CHECK(fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_KIND) == FSIMG_ENTRY_SYMLINK);
	CHECK(fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_LENGTH) == 9);
	CHECK(fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_NAME_LENGTH) == 5);
	CHECK(fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_UID) == 1000);
	CHECK(fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_GID) == 20);
	CHECK(fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_MODE) == 07777);
	CHECK(fsimg_wire_get_signed64(raw + FSIMG_WIRE_ENTRY_CHANGED +
	    FSIMG_WIRE_TIME_SECONDS) == -2);
	CHECK(fsimg_wire_get_signed64(raw + FSIMG_WIRE_ENTRY_CHANGED +
	    FSIMG_WIRE_TIME_OFFSET) == -90);

	entry = (fsimg_entry_t){ .path = "/pipe", .kind = FSIMG_ENTRY_FIFO };
	entry.metadata.uid = 77;
	entry.metadata.mode = 0177777;
	fsimg_wire_entry_encode(frame.payload, &entry, 5);
	CHECK(fsimg_wire_entry_frame_valid(&frame, false, 4096));
	CHECK(!fsimg_wire_entry_frame_valid(&frame, true, 4096));
	CHECK(fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_UID) == 0);
	CHECK(fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_MODE) == 0);

	entry = (fsimg_entry_t){ .path = "/dir", .kind = FSIMG_ENTRY_DIRECTORY };
	CHECK(entry_frame_valid(&entry, false) && !entry_frame_valid(&entry, true));
	entry = (fsimg_entry_t){ .path = "/file", .kind = FSIMG_ENTRY_FILE, .size = 3 };
	CHECK(entry_frame_valid(&entry, false) && entry_frame_valid(&entry, true));
}

/* The supervisor rejects every record the worker could not legitimately
 * produce, so a compromised worker cannot smuggle an unknown kind or mode.
 */
static void
test_invalid_entry_records(void)
{
	fsimg_frame_t frame = { .type = FSIMG_FRAME_ENTRY,
	    .length = FSIMG_WIRE_ENTRY_BYTES };
	fsimg_entry_t entry;
	unsigned char *raw = frame.payload;

	entry = (fsimg_entry_t){ .path = "/x", .kind = FSIMG_ENTRY_SYMLINK };
	CHECK(!entry_frame_valid(&entry, false));
	entry.size = 1024;
	CHECK(!entry_frame_valid(&entry, false));
	entry.size = 1023;
	CHECK(entry_frame_valid(&entry, false));
	entry = (fsimg_entry_t){ .path = "/x", .kind = FSIMG_ENTRY_FIFO, .size = 1 };
	CHECK(!entry_frame_valid(&entry, false));
	entry = (fsimg_entry_t){ .path = "/x", .kind = FSIMG_ENTRY_FILE };
	entry.metadata.valid = FSIMG_META_CHANGED;
	entry.metadata.changed = (fsimg_timestamp_t){ .nanoseconds = 1000000000,
	    .precision_ns = 1, .basis = FSIMG_TIME_UTC };
	CHECK(!entry_frame_valid(&entry, false));

	entry = (fsimg_entry_t){ .path = "/x", .kind = FSIMG_ENTRY_FILE };
	entry.metadata.valid = FSIMG_META_MODE;
	fsimg_wire_entry_encode(raw, &entry, 2);
	CHECK(fsimg_wire_entry_frame_valid(&frame, false, 4096));
	fsimg_wire_put64(raw + FSIMG_WIRE_ENTRY_MODE, 010000);
	CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
	fsimg_wire_entry_encode(raw, &entry, 2);
	fsimg_wire_put64(raw + FSIMG_WIRE_ENTRY_UID, UINT64_C(1) << 32);
	CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
	fsimg_wire_entry_encode(raw, &entry, 2);
	fsimg_wire_put64(raw + FSIMG_WIRE_ENTRY_VALID, 0x80);
	CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
	for (uint64_t kind = 0; kind <= 16; kind++) {
		fsimg_wire_entry_encode(raw, &entry, 2);
		fsimg_wire_put64(raw + FSIMG_WIRE_ENTRY_KIND, kind);
		CHECK(fsimg_wire_entry_frame_valid(&frame, false, 4096) ==
		    (kind == FSIMG_ENTRY_FILE || kind == FSIMG_ENTRY_DIRECTORY ||
		    kind == FSIMG_ENTRY_FIFO));
	}
	/* Absent attributes and timestamps must arrive as zero bytes. */
	fsimg_wire_entry_encode(raw, &entry, 2);
	fsimg_wire_put64(raw + FSIMG_WIRE_ENTRY_ATTRIBUTES, 1);
	CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
	fsimg_wire_entry_encode(raw, &entry, 2);
	raw[FSIMG_WIRE_ENTRY_CHANGED + FSIMG_WIRE_TIME_BYTES - 1] = 1;
	CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
	frame.length = FSIMG_WIRE_ENTRY_BYTES - 1;
	fsimg_wire_entry_encode(raw, &entry, 2);
	CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
}

static void
test_info_uuid(void)
{
	static const unsigned char uuid[16] = { 0x01, 0x23, 0x45, 0x67, 0x89,
	    0xab, 0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10 };
	fsimg_frame_t frame = { .type = FSIMG_FRAME_INFO,
	    .length = FSIMG_WIRE_INFO_BYTES };
	fsimg_volume_info_t info;

	bzero(&info, sizeof(info));
	info.valid = FSIMG_VOLUME_UUID | FSIMG_VOLUME_LABEL;
	bcopy(uuid, info.uuid, sizeof(uuid));
	info.volume_bytes = 4096;
	info.time_basis = FSIMG_TIME_UTC;
	info.scope = FSIMG_VALIDATION_HEADER;
	strcpy(info.format, "qnx6");
	fsimg_wire_info_encode(frame.payload, &info);
	CHECK(fsimg_wire_info_frame_valid(&frame, 4096));
	CHECK(fsimg_wire_get64(frame.payload + FSIMG_WIRE_INFO_VALID) ==
	    (FSIMG_VOLUME_UUID | FSIMG_VOLUME_LABEL));
	CHECK(bcmp(frame.payload + FSIMG_WIRE_INFO_UUID, uuid, sizeof(uuid)) == 0);
	fsimg_wire_put64(frame.payload + FSIMG_WIRE_INFO_VALID, 0x10);
	CHECK(!fsimg_wire_info_frame_valid(&frame, 4096));

	/* An absent UUID travels as zero bytes. */
	info.valid = 0;
	fsimg_wire_info_encode(frame.payload, &info);
	for (unsigned i = 0; i < 16; i++)
		CHECK(frame.payload[FSIMG_WIRE_INFO_UUID + i] == 0);
	CHECK(fsimg_wire_info_frame_valid(&frame, 4096));
	frame.payload[FSIMG_WIRE_INFO_UUID + 15] = 1;
	CHECK(!fsimg_wire_info_frame_valid(&frame, 4096));
}

/* The widest label a format stores, 48 bytes without a terminator on disk,
 * travels whole; a label field without its terminator is refused.
 */
static void
test_info_label(void)
{
	fsimg_frame_t frame = { .type = FSIMG_FRAME_INFO,
	    .length = FSIMG_WIRE_INFO_BYTES };
	fsimg_volume_info_t info;

	bzero(&info, sizeof(info));
	info.valid = FSIMG_VOLUME_LABEL;
	memset(info.label, 'L', 48);
	info.label[48] = '\0';
	info.volume_bytes = 4096;
	info.time_basis = FSIMG_TIME_UTC;
	info.scope = FSIMG_VALIDATION_HEADER;
	strcpy(info.format, "qnx4");
	fsimg_wire_info_encode(frame.payload, &info);
	CHECK(fsimg_wire_info_frame_valid(&frame, 4096));
	CHECK(strlen((const char *)frame.payload + FSIMG_WIRE_INFO_LABEL) == 48);
	CHECK(bcmp(frame.payload + FSIMG_WIRE_INFO_LABEL, info.label, 48) == 0);
	frame.payload[FSIMG_WIRE_INFO_LABEL + 48] = 'L';
	CHECK(!fsimg_wire_info_frame_valid(&frame, 4096));
	frame.payload[FSIMG_WIRE_INFO_LABEL + 48] = '\0';
	CHECK(fsimg_wire_info_frame_valid(&frame, 4096));
	/* The seven bytes between the label and the UUID are zero. */
	frame.payload[FSIMG_WIRE_INFO_LABEL + 49] = 1;
	CHECK(!fsimg_wire_info_frame_valid(&frame, 4096));
	frame.payload[FSIMG_WIRE_INFO_LABEL + 49] = 0;
	frame.payload[FSIMG_WIRE_INFO_UUID - 1] = 1;
	CHECK(!fsimg_wire_info_frame_valid(&frame, 4096));
}

static void
test_job_posix_flags(void)
{
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_EXTRACT;
	request.flags = FSIMG_JOB_SKIP_UNSUPPORTED;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.flags == FSIMG_JOB_SKIP_UNSUPPORTED);

	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	request.flags = 0;
	request.write.flags |= FSIMG_WRITE_IMPORT_POSIX;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK((decoded.write.flags & FSIMG_WRITE_IMPORT_POSIX) != 0);
}

/* The QNX6 settings cross the job wire unchanged; a byte order, inode
 * capacity or reserve outside its type, or a nonzero padding byte between
 * them, is refused.
 */
static void
test_job_qnx6_settings(void)
{
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES], mutated[FSIMG_JOB_BYTES];
	static const unsigned bad[] = { 270, 288, 289, 300, 308 };
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	request.write.byte_order = FSIMG_BYTE_ORDER_BIG;
	request.write.inode_capacity = 4000000;
	request.write.reserve_percent = 0;
	for (unsigned i = 0; i < 16; i++)
		request.write.volume_uuid[i] = (unsigned char)(0xf0 + i);
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.write.byte_order == FSIMG_BYTE_ORDER_BIG);
	CHECK(decoded.write.inode_capacity == 4000000);
	CHECK(decoded.write.reserve_percent == 0);
	CHECK(bcmp(decoded.write.volume_uuid, request.write.volume_uuid, 16) == 0);
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		bcopy(raw, mutated, sizeof(raw));
		mutated[bad[i]] = 2;
		CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded,
		    &selection, &capabilities, NULL) == FSIMG_EINVAL);
	}
}

/* Roots travel with create jobs only: their count, the roots capability and
 * the packed path sequence length must agree, and the count cannot exceed
 * the descriptor limit.
 */
static void
test_job_roots(void)
{
	static const char *const paths[] = { "/", "/EFI", "/a/b" };
	static const char *const tagged[] = {
		FSIMG_JOB_MANIFEST, FSIMG_JOB_CONTENTS, "/x"
	};
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES], mutated[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_CREATE;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_EINVAL);
	request.kind = FSIMG_JOB_LIST;
	request.root_paths = paths;
	request.root_count = 1;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_EINVAL);
	request.kind = FSIMG_JOB_PLAN;
	request.root_count = 3;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.root_count == 3 && selection == 2 + 5 + 5);
	CHECK((capabilities & FSIMG_JOB_CAP_ROOTS) != 0);

	/* A count over the descriptor limit, a zero count, a missing roots
	 * capability, and a sequence too short for the count are refused.
	 */
	bcopy(raw, mutated, sizeof(raw));
	fsimg_wire_put64(mutated + 312, 129);
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	fsimg_wire_put64(mutated + 312, 0);
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	bcopy(raw, mutated, sizeof(raw));
	fsimg_wire_put64(mutated + 23 * 8, 0);
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	/* The capabilities field, 23, holds only known bits. */
	fsimg_wire_put64(mutated + 23 * 8, FSIMG_JOB_CAP_ROOTS);
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	fsimg_wire_put64(mutated + 23 * 8,
	    FSIMG_JOB_CAP_ROOTS | (FSIMG_JOB_CAP_ALL + 1u));
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	bcopy(raw, mutated, sizeof(raw));
	fsimg_wire_put64(mutated + 24 * 8, 5);
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);

	/* The path limit bounds every root's image path, but not a manifest
	 * root's tags, which are protocol: a limit shorter than
	 * FSIMG_JOB_CONTENTS still carries them, on both sides of the wire.
	 */
	request.limits.max_path_bytes = 4;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_ELIMIT);
	request.root_paths = tagged;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(selection == sizeof(FSIMG_JOB_MANIFEST) +
	    sizeof(FSIMG_JOB_CONTENTS) + 3);
}

/* The stack and selector cross the job wire unchanged. A slot state or
 * selector outside its enum, an unterminated name or selector text, and a
 * stack the grammar refuses (a scheme name in the container slot) fail.
 */
static void
test_job_selection(void)
{
	static const struct {
		unsigned	 offset;
		uint64_t	 value;
	} bad[] = {
		{ 320, FSIMG_SELECT_DISK + 1 }, { 328, UINT64_C(1) << 32 },
		{ 336, UINT64_C(1) << 32 }
	};
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES], mutated[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_LIST;
	request.selection = "/";
	CHECK(fsimg_stack_parse("fat32/gpt", &request.stack, NULL) == FSIMG_OK);
	request.selector = FSIMG_SELECT_NAME;
	strcpy(request.partition_text, "EFI system");
	request.layers.gpt_sector_bytes = 4096;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(bcmp(&decoded.stack, &request.stack, sizeof(request.stack)) == 0);
	CHECK(decoded.selector == FSIMG_SELECT_NAME && decoded.layers.gpt_sector_bytes == 4096);
	CHECK(strcmp(decoded.partition_text, "EFI system") == 0);
	CHECK(strcmp(fsimg_job_format(&decoded), "fat32") == 0);

	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		bcopy(raw, mutated, sizeof(raw));
		fsimg_wire_put64(mutated + bad[i].offset, bad[i].value);
		CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded,
		    &selection, &capabilities, NULL) == FSIMG_EINVAL);
	}
	bcopy(raw, mutated, sizeof(raw));
	mutated[344 + FSIMG_LAYER_CONTAINER] = FSIMG_SLOT_NAMED + 1;
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	bcopy(raw, mutated, sizeof(raw));
	memset(mutated + 384, 'A', 112);
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	bcopy(raw, mutated, sizeof(raw));
	mutated[344 + FSIMG_LAYER_CONTAINER] = FSIMG_SLOT_NAMED;
	strcpy((char *)mutated + 368, "mbr");
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
}

/* Every layer setting crosses the job wire unchanged. Unknown MBR flags,
 * an identifier past 32 bits or present without its flag, a gzip level
 * past 32 bits, nonzero padding and unterminated text each fail, so a
 * record has one encoding.
 */
static void
test_job_layers(void)
{
	static const struct {
		unsigned	 offset;
		uint64_t	 value;
	} bad[] = {
		{ 512, UINT64_C(1) << 32 }, { 520, 4 }, { 520, 0x100 },
		{ 808, UINT64_C(1) << 32 }
	};
	static const unsigned text_ends[] = { 599, 711, 727 };
	static const unsigned char guid[16] = { 0x11, 0x22, 0x33, 0x44, 0x55,
	    0x66, 0x47, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01 };
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES], mutated[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_INFO;
	CHECK(fsimg_stack_parse("fat32/mbr", &request.stack, NULL) == FSIMG_OK);
	request.layers.gpt_sector_bytes = 4096;
	request.layers.gpt_disk_bytes = UINT64_C(1) << 40;
	request.layers.mbr_disk_bytes = UINT64_C(3) << 20;
	request.layers.mbr_id = 0xdeadbeef;
	request.layers.mbr_id_set = true;
	request.layers.mbr_active = true;
	bcopy(guid, request.layers.gpt_disk_guid, sizeof(guid));
	bcopy(guid, request.layers.gpt_guid, sizeof(guid));
	request.layers.gpt_guid[15] = 0x02;
	memset(request.layers.gpt_type, 't', sizeof(request.layers.gpt_type) - 1);
	memset(request.layers.gpt_name, 'n', sizeof(request.layers.gpt_name) - 1);
	strcpy(request.layers.mbr_type, "0x0c");
	request.layers.gzip_level = 9;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(bcmp(&decoded.layers, &request.layers, sizeof(request.layers)) == 0);

	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		bcopy(raw, mutated, sizeof(raw));
		fsimg_wire_put64(mutated + bad[i].offset, bad[i].value);
		CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded,
		    &selection, &capabilities, NULL) == FSIMG_EINVAL);
	}
	bcopy(raw, mutated, sizeof(raw));
	mutated[520] = 0x02;
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	/* The last byte of each text field: the full GPT type's and name's
	 * terminators, and padding after the MBR type's.
	 */
	for (unsigned i = 0; i < sizeof(text_ends) / sizeof(text_ends[0]); i++) {
		bcopy(raw, mutated, sizeof(raw));
		mutated[text_ends[i]] = 'x';
		CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded,
		    &selection, &capabilities, NULL) == FSIMG_EINVAL);
	}
	memset(request.layers.mbr_type, 'm', sizeof(request.layers.mbr_type));
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_EINVAL);
}

/* An image record's disk is its file without a container, its compression
 * is one the build reads, it has a table only under a scheme, and a
 * partition record lies within the disk and the table.
 */
static void
test_image_records(void)
{
	fsimg_partition_info_t partition;
	fsimg_frame_t frame;
	unsigned char *raw = frame.payload;

	bzero(&frame, sizeof(frame));
	frame.type = FSIMG_FRAME_IMAGE;
	frame.length = FSIMG_WIRE_IMAGE_BYTES;
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_DISK, 8388608);
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_FILE, 8388608);
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_STACK, "fat32");
	CHECK(fsimg_wire_image_frame_valid(&frame));
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_FILE, 8388607);
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_FILE, 8388608);
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "gzip");
	CHECK(fsimg_wire_image_frame_valid(&frame));
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "zstd");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "dmg");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	bzero(raw + FSIMG_WIRE_IMAGE_COMPRESSION, 16);
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_SLOTS, 4);
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_SCHEME, "mbr");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_SECTOR, 512);
	CHECK(fsimg_wire_image_frame_valid(&frame));
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_SELECTED, 5);
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_SELECTED, 1);
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_REASON, "stale");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_FLAGS, FSIMG_WIRE_IMAGE_UNSELECTED);
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_SELECTED, 0);
	CHECK(fsimg_wire_image_frame_valid(&frame));
	raw[FSIMG_WIRE_IMAGE_REASON] = '\0';
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	raw[FSIMG_WIRE_IMAGE_REASON] = 's';
	memset(raw + FSIMG_WIRE_IMAGE_STACK, 'x', FSIMG_WIRE_IMAGE_STACK_BYTES);
	CHECK(!fsimg_wire_image_frame_valid(&frame));

	bzero(&partition, sizeof(partition));
	partition.index = 2;
	partition.offset = 1048576;
	partition.length = 1048576;
	partition.claim = FSIMG_CLAIM_FORMAT;
	strcpy(partition.format, "fat12");
	strcpy(partition.name, "EFI");
	bzero(&frame, sizeof(frame));
	frame.type = FSIMG_FRAME_PARTITION;
	frame.length = FSIMG_WIRE_PARTITION_BYTES;
	fsimg_wire_partition_encode(raw, &partition);
	CHECK(fsimg_wire_partition_frame_valid(&frame, 4, 2097152));
	CHECK(!fsimg_wire_partition_frame_valid(&frame, 4, 2097151));
	CHECK(!fsimg_wire_partition_frame_valid(&frame, 1, 2097152));
	partition.claim = FSIMG_CLAIM_NONE;
	fsimg_wire_partition_encode(raw, &partition);
	CHECK(!fsimg_wire_partition_frame_valid(&frame, 4, 2097152));
	partition.format[0] = '\0';
	partition.offset = UINT64_MAX;
	fsimg_wire_partition_encode(raw, &partition);
	CHECK(!fsimg_wire_partition_frame_valid(&frame, 4, UINT64_MAX));
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
		write_seed(argv[2]);
		return (0);
	}
	CHECK(argc == 1);
	test_job_qnx6_settings();
	test_job_selection();
	test_job_layers();
	test_image_records();
	test_frame_splits(0);
	test_frame_splits(127);
	test_frame_splits(FSIMG_FRAME_BYTES);
	test_malformed_frames();
	test_error_codec();
	test_job_label();
	test_job_stream_flags();
	test_selection_budget();
	test_error_padding();
	test_entry_records();
	test_invalid_entry_records();
	test_info_uuid();
	test_info_label();
	test_job_posix_flags();
	test_job_roots();
	return (0);
}
