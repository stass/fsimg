/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fsimg-endian.h"
#include "layer.h"
#include "settings.h"
#include "test.h"
#include "worker.h"

/* These byte offsets are the transport contract, independent of the codec. */
enum {
	PLACEMENT_COUNT = 5143,
	PLACEMENT_RECORDS = 5151,
	PLACEMENT_RECORD_BYTES = 1033,
	PLACEMENT_PATH_BYTES = 1025
};

static void test_frame_splits(uint32_t);
static void test_malformed_frames(void);
static void test_error_codec(void);
static void test_job_label(void);
static void test_job_mountpoint(void);
static void test_selection_budget(void);
static void test_error_padding(void);
static void test_extra_data_entry_records(void);
static void test_entry_records(void);
static void test_device_and_socket_entry_records(void);
static void test_invalid_device_and_socket_entry_records(void);
static void test_invalid_entry_records(void);
static void test_info_uuid(void);
static void test_info_geometry_is_informational(void);
static void test_info_missing_bytes_are_bounded(void);
static void test_image_declared_end_is_representable(void);
static void test_partition_missing_bytes_match_disk_extent(void);
static void test_info_label(void);
static void test_info_mountpoint(void);
static void test_job_posix_flags(void);
static void test_signed_wire_values(void);
static void test_job_roots(void);
static void test_job_selection(void);
static void test_read_job_members_roundtrip(void);
static void test_select_volume_replaces_read_job_members(void);
static void test_read_job_zero_members_is_invalid(void);
static void test_job_zstd_levels_roundtrip_and_reject_wide_wire_value(void);
static void test_zstd_unverified_record_requires_verified_zstd(void);
static void test_job_xz_settings_roundtrip_and_reject_invalid_wire_values(void);
static void test_job_erofs_settings_roundtrip_and_reject_invalid_wire_values(void);
static void test_xz_unverified_record_requires_verified_xz(void);
static void test_job_force_requires_a_read_and_named_layer_or_compression(void);
static void test_job_layers(void);
static void test_job_catalogue(void);
static void other_value(const fsimg_setting_t *, fsimg_setting_value_t *);
static bool same_value(const fsimg_setting_t *, const fsimg_setting_value_t *,
    const fsimg_setting_value_t *);
static void check_transported(const fsimg_setting_t *);
static void test_image_records(void);
static void test_missing_partition_records(void);

/* Create jobs in these tests populate the image root from one root. */
static const char *const image_root[] = { "/" };
static bool entry_frame_valid(const fsimg_entry_t *, bool);
static void write_seed(const char *);

static void
write_seed(const char *path)
{
	fsimg_worker_request_t request;
	fsimg_error_t error = { .status = FSIMG_ECORRUPT };
	unsigned char raw[FSIMG_JOB_BYTES > FSIMG_WIRE_INFO_BYTES ?
	    FSIMG_JOB_BYTES : FSIMG_WIRE_INFO_BYTES];
	int fd;

	_Static_assert(sizeof(raw) >= FSIMG_JOB_BYTES &&
	    sizeof(raw) >= FSIMG_WIRE_PLAN_BYTES &&
	    sizeof(raw) >= FSIMG_WIRE_ENTRY_BYTES &&
	    sizeof(raw) >= FSIMG_WIRE_VERIFY_BYTES &&
	    sizeof(raw) >= FSIMG_WIRE_ERROR_BYTES,
	    "seed buffer must hold every encoded record");

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	CHECK(fd != -1);
	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_INFO;
	request.input_fd = 0;
	request.input = (fsimg_input_t){ .kind = FSIMG_INPUT_FILE, .quantum = 1 };
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_JOB, raw, FSIMG_JOB_BYTES, NULL) == FSIMG_OK);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_SELECTION, "/", 1, NULL) == FSIMG_OK);

	bzero(raw, sizeof(raw));
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_VOLUME, 4096);
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_TIME_BASIS, FSIMG_TIME_WALL);
	fsimg_store_le64(raw + FSIMG_WIRE_INFO_SCOPE, FSIMG_VALIDATION_HEADER);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_INFO, raw, FSIMG_WIRE_INFO_BYTES, NULL) == FSIMG_OK);
	bzero(raw, sizeof(raw));
	fsimg_store_le64(raw + FSIMG_WIRE_PLAN_VOLUME, 4096);
	fsimg_store_le64(raw + FSIMG_WIRE_PLAN_ROOT, 112);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_PLAN, raw, FSIMG_WIRE_PLAN_BYTES, NULL) == FSIMG_OK);
	bzero(raw, sizeof(raw));
	fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_KIND, FSIMG_ENTRY_FILE);
	fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_NAME_LENGTH, 2);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_ENTRY, raw, FSIMG_WIRE_ENTRY_BYTES, NULL) == FSIMG_OK);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_NAME, "/x", 2, NULL) == FSIMG_OK);
	CHECK(fsimg_frame_write(fd, FSIMG_FRAME_ENTRY_END, NULL, 0, NULL) == FSIMG_OK);
	bzero(raw, sizeof(raw));
	fsimg_store_le64(raw + FSIMG_WIRE_VERIFY_SCOPE, FSIMG_VALIDATION_STRUCTURAL);
	fsimg_store_le64(raw + FSIMG_WIRE_VERIFY_COMPLETE, 1);
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
		case 4: fsimg_store_le64(header + 8, FSIMG_FRAME_BYTES + 1); break;
		case 5: fsimg_store_le64(header + 8, UINT64_MAX); break;
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
	fsimg_store_le64(raw, 0);
	CHECK(fsimg_wire_error_decode(raw, sizeof(raw), &decoded) == FSIMG_EIO);
	fsimg_store_le64(raw, (uint64_t)(int64_t)FSIMG_EDURABILITY);
	CHECK(fsimg_wire_error_decode(raw, sizeof(raw), &decoded) == FSIMG_EDURABILITY);
	memset(raw + 336, 'X', 256);
	CHECK(fsimg_wire_error_decode(raw, sizeof(raw), &decoded) == FSIMG_EIO);
}

static void
test_partial_error_and_unknown_status_are_rejected(void)
{
	fsimg_error_t error = { .status = FSIMG_EPARTIAL,
	    .operation = FSIMG_OP_EXTRACT };
	fsimg_error_t decoded;
	unsigned char raw[FSIMG_WIRE_ERROR_BYTES];

	fsimg_wire_error_encode(raw, &error);
	CHECK(fsimg_wire_error_decode(raw, sizeof(raw), &decoded) == FSIMG_EIO);
	CHECK(strcmp(decoded.message, "invalid worker error fields") == 0);
	fsimg_store_le64(raw, (uint64_t)(int64_t)-15);
	CHECK(fsimg_wire_error_decode(raw, sizeof(raw), &decoded) == FSIMG_EIO);
}

static void
test_salvage_job_is_extract_only(void)
{
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	for (fsimg_job_kind_t kind = FSIMG_JOB_INFO; kind <= FSIMG_JOB_PLAN; kind++) {
		fsimg_worker_request_init(&request);
		request.kind = kind;
		request.flags = FSIMG_JOB_SALVAGE;
		if (fsimg_job_has_roots(kind)) {
			request.root_paths = image_root;
			request.root_count = 1;
		}
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK((fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK) == (kind == FSIMG_JOB_EXTRACT));
		if (kind == FSIMG_JOB_EXTRACT)
			CHECK(decoded.flags == FSIMG_JOB_SALVAGE);
	}
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
	request.members = (const char *const[]){ "/abc" };
	request.member_count = 1;
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
		fsimg_input_kind_t input;
		bool		 valid;
	} cases[] = {
		{ FSIMG_JOB_LIST, 0, FSIMG_INPUT_STREAM, true },
		{ FSIMG_JOB_LIST, FSIMG_JOB_STDOUT, FSIMG_INPUT_ABSENT, false },
		{ FSIMG_JOB_CREATE, FSIMG_JOB_STDOUT, FSIMG_INPUT_ABSENT, true },
		{ FSIMG_JOB_CREATE, 0, FSIMG_INPUT_STREAM, false },
		{ FSIMG_JOB_PLAN, FSIMG_JOB_STDOUT, FSIMG_INPUT_ABSENT, false },
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
		{ FSIMG_JOB_LIST, "zstd",
#ifdef FSIMG_ZSTD
		    true },
#else
		    false },
#endif
		{ FSIMG_JOB_CREATE, "zstd",
#ifdef FSIMG_ZSTD
		    true },
#else
		    false },
#endif
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
		request.input.kind = cases[i].input;
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
	request.input.kind = FSIMG_INPUT_STREAM;
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
	char label[767];
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

	/* The maximum label of 765 bytes, round-trips;
	 * one byte more does not encode.
	 */
	memset(label, 'L', 765);
	label[765] = '\0';
	request.write.label = label;
	CHECK(strlen(request.write.label) == 765);
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(strcmp(decoded.write.label, request.write.label) == 0);
	label[765] = 'L';
	label[766] = '\0';
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_EINVAL);

	/* The worker refuses every other encoding of the strings: a label with
	 * no terminator in its 766 bytes (216 to 981) or a byte after its
	 * terminator, a presence byte (982) other than 0 or 1 or marking a
	 * label absent, nonzero padding before the UUID, and a byte after the
	 * format name's terminator.
	 */
	request.write.label = "MYLABEL";
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	memset(raw + 216 + 7, 'X', 758);
	CHECK(job_decodes(raw, 216 + 765, 0));
	CHECK(!job_decodes(raw, 216 + 765, 'X'));
	request.write.label = "MYLABEL";
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(job_decodes(raw, 216 + 8, 0));
	CHECK(!job_decodes(raw, 216 + 8, 'X'));
	CHECK(!job_decodes(raw, 982, 2));
	CHECK(!job_decodes(raw, 982, 0));
	CHECK(!job_decodes(raw, 983, 1));
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
	CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_KIND) == FSIMG_ENTRY_SYMLINK);
	CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_LENGTH) == 9);
	CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_NAME_LENGTH) == 5);
	CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_UID) == 1000);
	CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_GID) == 20);
	CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_MODE) == 07777);
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
	CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_UID) == 0);
	CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_MODE) == 0);

	entry = (fsimg_entry_t){ .path = "/dir", .kind = FSIMG_ENTRY_DIRECTORY };
	CHECK(entry_frame_valid(&entry, false) && !entry_frame_valid(&entry, true));
	entry = (fsimg_entry_t){ .path = "/file", .kind = FSIMG_ENTRY_FILE, .size = 3 };
	CHECK(entry_frame_valid(&entry, false) && entry_frame_valid(&entry, true));
}

/* Extra data has no value slots, on either content or directory entries. */
static void
test_extra_data_entry_records(void)
{
	static const fsimg_entry_kind_t kinds[] = {
		FSIMG_ENTRY_FILE, FSIMG_ENTRY_DIRECTORY
	};
	fsimg_frame_t frame = { .type = FSIMG_FRAME_ENTRY,
	    .length = FSIMG_WIRE_ENTRY_BYTES };
	fsimg_entry_t entry = { .path = "/x" };
	unsigned char *raw = frame.payload;

	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		entry.kind = kinds[i];
		entry.metadata = (fsimg_metadata_t){
		    .valid = FSIMG_META_EXTRA_DATA,
		    .required = FSIMG_META_EXTRA_DATA };
		fsimg_wire_entry_encode(raw, &entry, 2);
		CHECK(fsimg_wire_entry_frame_valid(&frame, false, 4096));
		CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_KIND) == kinds[i]);
		CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_VALID) ==
		    FSIMG_META_EXTRA_DATA);
		CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_REQUIRED) ==
		    FSIMG_META_EXTRA_DATA);
		for (size_t offset = FSIMG_WIRE_ENTRY_ATTRIBUTES;
		    offset < FSIMG_WIRE_ENTRY_BYTES; offset += 8)
			CHECK(fsimg_load_le64(raw + offset) == 0);

		entry.metadata.valid |= FSIMG_META_OWNER | FSIMG_META_MODE;
		entry.metadata.uid = 42;
		entry.metadata.gid = 7;
		entry.metadata.mode = 0644;
		fsimg_wire_entry_encode(raw, &entry, 2);
		CHECK(fsimg_wire_entry_frame_valid(&frame, false, 4096));
		CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_VALID) ==
		    entry.metadata.valid);
		CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_UID) == 42);
		CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_GID) == 7);
		CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_MODE) == 0644);

		fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_VALID,
		    FSIMG_META_OWNER | FSIMG_META_MODE);
		CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
	}
}

static void
test_device_and_socket_entry_records(void)
{
	static const fsimg_entry_kind_t kinds[] = {
		FSIMG_ENTRY_CHAR_DEVICE, FSIMG_ENTRY_BLOCK_DEVICE, FSIMG_ENTRY_SOCKET
	};
	fsimg_frame_t frame = { .type = FSIMG_FRAME_ENTRY,
	    .length = FSIMG_WIRE_ENTRY_BYTES };
	fsimg_entry_t entry = { .path = "/x" };
	unsigned char *raw = frame.payload;

	CHECK(FSIMG_WIRE_ENTRY_DEV_MAJOR == 232);
	CHECK(FSIMG_WIRE_ENTRY_DEV_MINOR == 240);
	CHECK(FSIMG_WIRE_ENTRY_BYTES == 248);
	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		entry.kind = kinds[i];
		CHECK(entry_frame_valid(&entry, false));
		CHECK(!entry_frame_valid(&entry, true));
		if (entry.kind == FSIMG_ENTRY_SOCKET)
			continue;
		entry.metadata.valid = FSIMG_META_DEVICE;
		entry.metadata.dev_major = UINT32_MAX;
		entry.metadata.dev_minor = UINT32_MAX;
		fsimg_wire_entry_encode(raw, &entry, 2);
		CHECK(fsimg_wire_entry_frame_valid(&frame, false, 4096));
		CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_DEV_MAJOR) == UINT32_MAX);
		CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_DEV_MINOR) == UINT32_MAX);
		entry.metadata.valid = 0;
		fsimg_wire_entry_encode(raw, &entry, 2);
		CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_DEV_MAJOR) == 0);
		CHECK(fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_DEV_MINOR) == 0);
		CHECK(fsimg_wire_entry_frame_valid(&frame, false, 4096));
	}
}

static void
test_invalid_device_and_socket_entry_records(void)
{
	static const fsimg_entry_kind_t noncontent_kinds[] = {
		FSIMG_ENTRY_CHAR_DEVICE, FSIMG_ENTRY_BLOCK_DEVICE, FSIMG_ENTRY_SOCKET
	};
	static const fsimg_entry_kind_t nondevice_kinds[] = {
		FSIMG_ENTRY_SOCKET, FSIMG_ENTRY_FILE, FSIMG_ENTRY_FIFO,
		FSIMG_ENTRY_DIRECTORY
	};
	static const uint64_t unknown_kinds[] = { 128, 3, 0 };
	static const size_t device_words[] = {
		FSIMG_WIRE_ENTRY_DEV_MAJOR, FSIMG_WIRE_ENTRY_DEV_MINOR
	};
	fsimg_frame_t frame = { .type = FSIMG_FRAME_ENTRY,
	    .length = FSIMG_WIRE_ENTRY_BYTES };
	fsimg_entry_t entry;
	unsigned char *raw = frame.payload;

	for (size_t i = 0; i < sizeof(noncontent_kinds) /
	    sizeof(noncontent_kinds[0]); i++) {
		entry = (fsimg_entry_t){ .path = "/x",
		    .kind = noncontent_kinds[i], .size = 1 };
		CHECK(!entry_frame_valid(&entry, false));
	}
	for (size_t i = 0; i < sizeof(nondevice_kinds) /
	    sizeof(nondevice_kinds[0]); i++) {
		entry = (fsimg_entry_t){ .path = "/x",
		    .kind = nondevice_kinds[i],
		    .metadata = { .valid = FSIMG_META_DEVICE } };
		CHECK(!entry_frame_valid(&entry, false));
	}
	for (size_t i = 0; i < sizeof(device_words) / sizeof(device_words[0]); i++) {
		entry = (fsimg_entry_t){ .path = "/x",
		    .kind = FSIMG_ENTRY_CHAR_DEVICE,
		    .metadata = { .valid = FSIMG_META_DEVICE } };
		fsimg_wire_entry_encode(raw, &entry, 2);
		fsimg_store_le64(raw + device_words[i], UINT64_C(1) << 32);
		CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
		entry = (fsimg_entry_t){ .path = "/x",
		    .kind = FSIMG_ENTRY_CHAR_DEVICE };
		fsimg_wire_entry_encode(raw, &entry, 2);
		fsimg_store_le64(raw + device_words[i], 1);
		CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
	}
	for (size_t i = 0; i < sizeof(unknown_kinds) / sizeof(unknown_kinds[0]); i++) {
		entry = (fsimg_entry_t){ .path = "/x", .kind = FSIMG_ENTRY_FILE };
		fsimg_wire_entry_encode(raw, &entry, 2);
		fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_KIND, unknown_kinds[i]);
		CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
	}
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
	entry.size = 1025;
	CHECK(!entry_frame_valid(&entry, false));
	entry.size = 1024;
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
	fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_MODE, 010000);
	CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
	fsimg_wire_entry_encode(raw, &entry, 2);
	fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_UID, UINT64_C(1) << 32);
	CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
	fsimg_wire_entry_encode(raw, &entry, 2);
	fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_VALID, 0x200);
	CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
	for (uint64_t kind = 0; kind <= 16; kind++) {
		fsimg_wire_entry_encode(raw, &entry, 2);
		fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_KIND, kind);
		CHECK(fsimg_wire_entry_frame_valid(&frame, false, 4096) ==
		    (kind == FSIMG_ENTRY_FILE || kind == FSIMG_ENTRY_DIRECTORY ||
		    kind == FSIMG_ENTRY_FIFO || kind == FSIMG_ENTRY_CHAR_DEVICE));
	}
	/* Absent attributes and timestamps must arrive as zero bytes. */
	fsimg_wire_entry_encode(raw, &entry, 2);
	fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_ATTRIBUTES, 1);
	CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
	fsimg_wire_entry_encode(raw, &entry, 2);
	raw[FSIMG_WIRE_ENTRY_CHANGED + FSIMG_WIRE_TIME_BYTES - 1] = 1;
	CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
	frame.length = FSIMG_WIRE_ENTRY_BYTES - 1;
	fsimg_wire_entry_encode(raw, &entry, 2);
	CHECK(!fsimg_wire_entry_frame_valid(&frame, false, 4096));
}

/* A filesystem can declare bytes beyond its available input. This record
 * reports that declaration; the supervisor owns I/O and output authority.
 */
static void
test_info_geometry_is_informational(void)
{
	fsimg_frame_t frame = { .type = FSIMG_FRAME_INFO,
	    .length = FSIMG_WIRE_INFO_BYTES };
	fsimg_volume_info_t info;

	bzero(&info, sizeof(info));
	info.valid = FSIMG_VOLUME_GEOMETRY;
	info.volume_bytes = UINT64_MAX;
	info.time_basis = FSIMG_TIME_UTC;
	info.scope = FSIMG_VALIDATION_HEADER;
	strcpy(info.format, "iso9660");
	fsimg_wire_info_encode(frame.payload, &info);
	CHECK(fsimg_wire_info_frame_valid(&frame));
}

/* Missing bytes are part of the declared volume, including an empty one. */
static void
test_info_missing_bytes_are_bounded(void)
{
	typedef struct info_missing_case {
		uint64_t volume;
		uint64_t missing;
		bool valid;
	} info_missing_case_t;
	static const info_missing_case_t cases[] = {
		{ .volume = 4096, .missing = 0, .valid = true },
		{ .volume = 4096, .missing = 4096, .valid = true },
		{ .volume = 4096, .missing = 4097, .valid = false },
		{ .volume = 0, .missing = 1, .valid = false }
	};
	fsimg_frame_t frame = { .type = FSIMG_FRAME_INFO,
	    .length = FSIMG_WIRE_INFO_BYTES };
	bool valid, matched = true;

	fsimg_store_le64(frame.payload + FSIMG_WIRE_INFO_TIME_BASIS, FSIMG_TIME_WALL);
	fsimg_store_le64(frame.payload + FSIMG_WIRE_INFO_SCOPE, FSIMG_VALIDATION_HEADER);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fsimg_store_le64(frame.payload + FSIMG_WIRE_INFO_VOLUME, cases[i].volume);
		fsimg_store_le64(frame.payload + FSIMG_WIRE_INFO_MISSING, cases[i].missing);
		valid = fsimg_wire_info_frame_valid(&frame);
		if (valid != cases[i].valid)
			fprintf(stderr, "INFO missing case %zu: volume=%llu missing=%llu expected %d, received %d\n",
			    i, (unsigned long long)cases[i].volume,
			    (unsigned long long)cases[i].missing, cases[i].valid, valid);
		matched &= valid == cases[i].valid;
	}
	CHECK(matched);
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
	CHECK(fsimg_wire_info_frame_valid(&frame));
	CHECK(fsimg_load_le64(frame.payload + FSIMG_WIRE_INFO_VALID) ==
	    (FSIMG_VOLUME_UUID | FSIMG_VOLUME_LABEL));
	CHECK(bcmp(frame.payload + FSIMG_WIRE_INFO_UUID, uuid, sizeof(uuid)) == 0);
	fsimg_store_le64(frame.payload + FSIMG_WIRE_INFO_VALID, 0x10);
	CHECK(!fsimg_wire_info_frame_valid(&frame));

	/* An absent UUID travels as zero bytes. */
	info.valid = 0;
	fsimg_wire_info_encode(frame.payload, &info);
	for (unsigned i = 0; i < 16; i++)
		CHECK(frame.payload[FSIMG_WIRE_INFO_UUID + i] == 0);
	CHECK(fsimg_wire_info_frame_valid(&frame));
	frame.payload[FSIMG_WIRE_INFO_UUID + 15] = 1;
	CHECK(!fsimg_wire_info_frame_valid(&frame));
}

/* The widest label a format stores, 765 bytes without a terminator,
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
	memset(info.label, 'L', 765);
	info.label[765] = '\0';
	info.volume_bytes = 4096;
	info.time_basis = FSIMG_TIME_UTC;
	info.scope = FSIMG_VALIDATION_HEADER;
	strcpy(info.format, "qnx4");
	fsimg_wire_info_encode(frame.payload, &info);
	CHECK(fsimg_wire_info_frame_valid(&frame));
	CHECK(strlen((const char *)frame.payload + FSIMG_WIRE_INFO_LABEL) == 765);
	CHECK(bcmp(frame.payload + FSIMG_WIRE_INFO_LABEL, info.label, 765) == 0);
	frame.payload[FSIMG_WIRE_INFO_LABEL + 765] = 'L';
	CHECK(!fsimg_wire_info_frame_valid(&frame));
	frame.payload[FSIMG_WIRE_INFO_LABEL + 765] = '\0';
	CHECK(fsimg_wire_info_frame_valid(&frame));
	/* The two bytes between the label and the UUID are zero. */
	frame.payload[FSIMG_WIRE_INFO_LABEL + 766] = 1;
	CHECK(!fsimg_wire_info_frame_valid(&frame));
	frame.payload[FSIMG_WIRE_INFO_LABEL + 766] = 0;
	frame.payload[FSIMG_WIRE_INFO_UUID - 1] = 1;
	CHECK(!fsimg_wire_info_frame_valid(&frame));
}

/* The widest mountpoint, 1024 bytes, and a full detail table travel whole.
 * A mountpoint without its terminator, bytes after it or in an absent one,
 * a detail count above sixteen, an empty or unprintable detail name and a
 * nonzero unused detail are refused.
 */
static void
test_info_mountpoint(void)
{
	fsimg_frame_t frame = { .type = FSIMG_FRAME_INFO,
	    .length = FSIMG_WIRE_INFO_BYTES };
	fsimg_volume_info_t info;
	unsigned char *mountpoint = frame.payload + FSIMG_WIRE_INFO_MOUNTPOINT;
	unsigned char *last;

	bzero(&info, sizeof(info));
	info.valid = FSIMG_VOLUME_MOUNTPOINT;
	memset(info.mountpoint, 'm', 1024);
	info.mountpoint[0] = '/';
	info.volume_bytes = 4096;
	info.time_basis = FSIMG_TIME_UTC;
	info.scope = FSIMG_VALIDATION_HEADER;
	strcpy(info.format, "ifs");
	info.detail_count = FSIMG_VOLUME_DETAILS_MAX;
	for (unsigned i = 0; i < FSIMG_VOLUME_DETAILS_MAX; i++) {
		memset(info.details[i].name, 'n', 23);
		info.details[i].name[0] = (char)('a' + i);
		info.details[i].value = UINT64_MAX - i;
	}
	fsimg_wire_info_encode(frame.payload, &info);
	CHECK(fsimg_wire_info_frame_valid(&frame));
	CHECK(strlen((const char *)mountpoint) == 1024);
	CHECK(bcmp(mountpoint, info.mountpoint, 1024) == 0);
	CHECK(fsimg_load_le64(frame.payload + FSIMG_WIRE_INFO_DETAIL_COUNT) ==
	    FSIMG_VOLUME_DETAILS_MAX);
	last = frame.payload + FSIMG_WIRE_INFO_DETAILS +
	    (FSIMG_VOLUME_DETAILS_MAX - 1) * FSIMG_WIRE_INFO_DETAIL_BYTES;
	CHECK(bcmp(last, info.details[15].name, 24) == 0);
	CHECK(fsimg_load_le64(last + FSIMG_WIRE_INFO_DETAIL_NAME_BYTES) ==
	    UINT64_MAX - 15);

	mountpoint[1024] = 'm';
	CHECK(!fsimg_wire_info_frame_valid(&frame));
	mountpoint[1024] = '\0';
	/* The seven bytes between the mountpoint and the detail count. */
	frame.payload[FSIMG_WIRE_INFO_DETAIL_COUNT - 1] = 1;
	CHECK(!fsimg_wire_info_frame_valid(&frame));
	frame.payload[FSIMG_WIRE_INFO_DETAIL_COUNT - 1] = 0;
	last[23] = 'n';
	CHECK(!fsimg_wire_info_frame_valid(&frame));
	last[23] = '\0';
	last[0] = ' ';
	CHECK(!fsimg_wire_info_frame_valid(&frame));
	last[0] = 0x80;
	CHECK(!fsimg_wire_info_frame_valid(&frame));
	last[0] = 'p';
	CHECK(fsimg_wire_info_frame_valid(&frame));

	/* An encoder caps the count; a frame above sixteen is refused. */
	info.detail_count = FSIMG_VOLUME_DETAILS_MAX + 1;
	fsimg_wire_info_encode(frame.payload, &info);
	CHECK(fsimg_load_le64(frame.payload + FSIMG_WIRE_INFO_DETAIL_COUNT) ==
	    FSIMG_VOLUME_DETAILS_MAX);
	fsimg_store_le64(frame.payload + FSIMG_WIRE_INFO_DETAIL_COUNT,
	    FSIMG_VOLUME_DETAILS_MAX + 1);
	CHECK(!fsimg_wire_info_frame_valid(&frame));

	/* Two details: an empty name is refused, as is any byte past them. */
	info.detail_count = 2;
	fsimg_wire_info_encode(frame.payload, &info);
	CHECK(fsimg_wire_info_frame_valid(&frame));
	frame.payload[FSIMG_WIRE_INFO_DETAILS + 2 * FSIMG_WIRE_INFO_DETAIL_BYTES +
	    FSIMG_WIRE_INFO_DETAIL_NAME_BYTES] = 1;
	CHECK(!fsimg_wire_info_frame_valid(&frame));
	info.details[1].name[0] = '\0';
	fsimg_wire_info_encode(frame.payload, &info);
	CHECK(!fsimg_wire_info_frame_valid(&frame));

	/* An absent mountpoint travels as zero bytes. */
	info.detail_count = 0;
	info.valid = 0;
	fsimg_wire_info_encode(frame.payload, &info);
	for (unsigned i = 0; i < FSIMG_WIRE_INFO_MOUNTPOINT_BYTES; i++)
		CHECK(mountpoint[i] == 0);
	CHECK(fsimg_wire_info_frame_valid(&frame));
	mountpoint[0] = '/';
	CHECK(!fsimg_wire_info_frame_valid(&frame));
}

/* NAMES is meaningful only for planning, creation and extraction. All other
 * fields stay valid so a refusal identifies the kind/flag combination.
 */
static void
test_job_names_roundtrip_only_for_create_plan_and_extract(void)
{
	static const struct {
		fsimg_job_kind_t kind;
		bool valid;
	} cases[] = {
		{ FSIMG_JOB_CREATE, true },
		{ FSIMG_JOB_PLAN, true },
		{ FSIMG_JOB_EXTRACT, true },
		{ FSIMG_JOB_LIST, false },
		{ FSIMG_JOB_CAT, false },
		{ FSIMG_JOB_INFO, false },
		{ FSIMG_JOB_VERIFY, false }
	};
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;
	fsimg_status_t status;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fsimg_worker_request_init(&request);
		request.kind = cases[i].kind;
		if (fsimg_job_has_roots(request.kind)) {
			request.root_paths = image_root;
			request.root_count = 1;
		}
		/* The same job without NAMES is the control for every kind. */
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK);
		request.flags |= FSIMG_JOB_NAMES;
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		status = fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL);
		if (status != (cases[i].valid ? FSIMG_OK : FSIMG_EINVAL))
			fprintf(stderr, "NAMES job kind %d: status %d\n", cases[i].kind, status);
		CHECK(status == (cases[i].valid ? FSIMG_OK : FSIMG_EINVAL));
		if (cases[i].valid) {
			CHECK(decoded.kind == cases[i].kind);
			CHECK(decoded.flags == request.flags);
		}
	}
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

/*
 * A signed wire field is a little-endian two's-complement pattern. The
 * patterns here are written out by hand, so they do not depend on the
 * encoder. Each value is encoded as a time record's seconds and compared
 * with its pattern byte for byte, beside a fixed -90 minute offset, and
 * crosses the job's epoch field, whose decoding uses the same signed
 * operation.
 */
static void
test_signed_wire_values(void)
{
	static const struct {
		unsigned char	 bytes[8];
		int64_t		 value;
	} cases[] = {
		{ { 0, 0, 0, 0, 0, 0, 0, 0 }, 0 },
		{ { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f }, INT64_MAX },
		{ { 0, 0, 0, 0, 0, 0, 0, 0x80 }, INT64_MIN },
		{ { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, -1 },
		{ { 0xa6, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, -90 }
	};
	static const unsigned char minus_ninety[8] = {
		0xa6, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	fsimg_worker_request_t request, decoded;
	fsimg_entry_t entry = { .path = "/t", .kind = FSIMG_ENTRY_FILE };
	unsigned char job[FSIMG_JOB_BYTES], raw[FSIMG_WIRE_ENTRY_BYTES];
	unsigned char *time = raw + FSIMG_WIRE_ENTRY_CHANGED;
	uint64_t selection;
	unsigned capabilities;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		CHECK(fsimg_wire_get_signed64(cases[i].bytes) == cases[i].value);

		fsimg_worker_request_init(&request);
		request.kind = FSIMG_JOB_CREATE;
		request.root_paths = image_root;
		request.root_count = 1;
		request.write.epoch = cases[i].value;
		CHECK(fsimg_job_encode(&request, job, NULL) == FSIMG_OK);
		CHECK(fsimg_job_decode(job, sizeof(job), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK);
		CHECK(decoded.write.epoch == cases[i].value);

		entry.metadata.valid = FSIMG_META_CHANGED;
		entry.metadata.changed = (fsimg_timestamp_t){
		    .seconds = cases[i].value, .precision_ns = 1,
		    .basis = FSIMG_TIME_OFFSET, .offset_minutes = -90 };
		fsimg_wire_entry_encode(raw, &entry, 2);
		CHECK(memcmp(time + FSIMG_WIRE_TIME_SECONDS, cases[i].bytes,
		    8) == 0);
		CHECK(memcmp(time + FSIMG_WIRE_TIME_OFFSET, minus_ninety,
		    8) == 0);
		CHECK(fsimg_wire_get_signed64(time + FSIMG_WIRE_TIME_SECONDS) ==
		    cases[i].value);
	}
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
	static const unsigned bad[] = { 983, 1000, 1001, 1012, 1020 };
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

/* The mountpoint travels in a field of 1026 bytes from byte 1544, after the
 * spool limit, the gzip level and the compression, with a presence byte at
 * 2570 and zero padding to the ifsboot settings at 2578. NULL and the
 * empty string stay distinct, the longest value that fits round-trips,
 * and every other encoding is refused.
 */
static void
test_job_mountpoint(void)
{
	enum { FIELD = 1026, MOUNTPOINT = 1544, PRESENT = 1544 + 1026 };
	static char longest[FIELD], longer[FIELD + 1];
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	CHECK(PLACEMENT_COUNT == PRESENT + 8 + 6 * 8 + 2 + 6 + 24 + 2080 + 24 + 357 + 3 * 8);
	memset(longest, 'm', FIELD - 1);
	memset(longer, 'm', FIELD);
	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	request.write.mountpoint = "";
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(raw[PRESENT] == 1);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.write.mountpoint != NULL &&
	    decoded.write.mountpoint[0] == '\0');
	request.write.mountpoint = longest;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.write.mountpoint != NULL &&
	    strcmp(decoded.write.mountpoint, longest) == 0);

	/* A missing terminator, a byte after one, an unknown presence value
	 * and nonzero padding are all refused.
	 */
	CHECK(!job_decodes(raw, MOUNTPOINT + FIELD - 1, 'm'));
	CHECK(!job_decodes(raw, PRESENT, 2));
	for (unsigned i = PRESENT + 1; i < PRESENT + 8; i++)
		CHECK(!job_decodes(raw, i, 1));
	request.write.mountpoint = "";
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(!job_decodes(raw, MOUNTPOINT + 1, 'x'));

	/* An absent mountpoint decodes as NULL, and a value marked absent is
	 * refused.
	 */
	request.write.mountpoint = NULL;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(raw[PRESENT] == 0);
	CHECK(!job_decodes(raw, MOUNTPOINT, 'x'));
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK && decoded.write.mountpoint == NULL);

	/* A value longer than its field does not encode. */
	request.write.mountpoint = longer;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_EINVAL);
}

/*
 * The ifsboot settings travel from byte 2578: the machine and the five
 * addresses as six 64-bit values, a byte of presence bits (bit n for value
 * n, bit 6 for the virtual setting), a byte of values (0x01 virtual, 0x02
 * compress=none) and six zero bytes to the record's end. A setting set to
 * zero stays distinct from one unset; a value without its presence bit, a
 * value past 32 bits, the virtual value without its presence bit, unknown
 * bits and nonzero padding are refused. The file settings' capability
 * bits belong to jobs that create.
 */
static void
test_job_ifsboot_settings(void)
{
	enum { CAPABILITIES = 23 * 8, VALUES = 2578, SET = VALUES + 6 * 8,
	    BITS = SET + 1, PADDING = BITS + 1 };
	fsimg_worker_request_t request, decoded;
	fsimg_layer_options_t *layers = &request.layers;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities, granted;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	layers->ifsboot_machine_set = true;
	layers->ifsboot_virtual_set = true;
	layers->ifsboot_virtual = true;
	layers->ifsboot_startup_vaddr = 0x1234;
	layers->ifsboot_startup_vaddr_set = true;
	layers->ifsboot_imagefs_paddr = UINT32_MAX;
	layers->ifsboot_imagefs_paddr_set = true;
	layers->ifsboot_compress = FSIMG_IFSBOOT_NONE;
	request.setting_fds[FSIMG_FILE_IFSBOOT_PREFIX] = 3;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(raw[SET] == (0x01 | 0x02 | 0x20 | 0x40) && raw[BITS] == 0x03);
	CHECK(raw[VALUES + 8] == 0x34 && raw[VALUES + 9] == 0x12);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.layers.ifsboot_machine_set &&
	    decoded.layers.ifsboot_machine == 0);
	CHECK(decoded.layers.ifsboot_virtual_set &&
	    decoded.layers.ifsboot_virtual);
	CHECK(decoded.layers.ifsboot_startup_vaddr_set &&
	    decoded.layers.ifsboot_startup_vaddr == 0x1234);
	CHECK(!decoded.layers.ifsboot_paddr_bias_set &&
	    !decoded.layers.ifsboot_image_paddr_set &&
	    !decoded.layers.ifsboot_ram_paddr_set);
	CHECK(decoded.layers.ifsboot_imagefs_paddr_set &&
	    decoded.layers.ifsboot_imagefs_paddr == UINT32_MAX);
	CHECK(decoded.layers.ifsboot_compress == FSIMG_IFSBOOT_NONE);
	CHECK(capabilities == (FSIMG_JOB_CAP_ROOTS |
	    FSIMG_JOB_CAP_SETTING(FSIMG_FILE_IFSBOOT_PREFIX)));

	CHECK(!job_decodes(raw, VALUES + 2 * 8, 1));	/* paddr_bias unset */
	CHECK(!job_decodes(raw, VALUES + 8 + 4, 1));	/* past 32 bits */
	CHECK(!job_decodes(raw, SET, 0x80));
	CHECK(!job_decodes(raw, SET, 0x01 | 0x02 | 0x20));
	CHECK(job_decodes(raw, SET, 0x01 | 0x02 | 0x20 | 0x40 | 0x04));
	CHECK(!job_decodes(raw, BITS, 0x04));
	for (unsigned i = PADDING; i < PADDING + 6; i++)
		CHECK(!job_decodes(raw, i, 1));

	/* A job that only reads takes no file setting, encoded or decoded. */
	request.kind = FSIMG_JOB_LIST;
	request.root_paths = NULL;
	request.root_count = 0;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_EINVAL);
	request.setting_fds[FSIMG_FILE_IFSBOOT_PREFIX] = -1;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &granted, NULL) == FSIMG_OK);
	for (unsigned i = 0; i < FSIMG_FILE_SETTINGS; i++) {
		fsimg_store_le64(raw + CAPABILITIES,
		    granted | FSIMG_JOB_CAP_SETTING(i));
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_EINVAL);
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
	fsimg_store_le64(mutated + 1024, 129);
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	fsimg_store_le64(mutated + 1024, 0);
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	bcopy(raw, mutated, sizeof(raw));
	fsimg_store_le64(mutated + 23 * 8, 0);
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	/* The capabilities field, 23, holds only known bits. */
	fsimg_store_le64(mutated + 23 * 8, FSIMG_JOB_CAP_ROOTS);
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	fsimg_store_le64(mutated + 23 * 8,
	    FSIMG_JOB_CAP_ROOTS | (FSIMG_JOB_CAP_ALL + 1u));
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	bcopy(raw, mutated, sizeof(raw));
	fsimg_store_le64(mutated + 24 * 8, 5);
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

static void
test_read_job_members_roundtrip(void)
{
	const char *const members[] = { "/A", "/B/file", "/A/nested" };
	const fsimg_job_kind_t kinds[] = { FSIMG_JOB_LIST, FSIMG_JOB_EXTRACT };
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection_length, paths_length;
	fsimg_error_t error;
	unsigned capabilities;

	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		fsimg_worker_request_init(&request);
		request.kind = kinds[i];
		request.members = members;
		request.member_count = 3;
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded,
		    &selection_length, &capabilities, NULL) == FSIMG_OK);
		CHECK(decoded.kind == kinds[i]);
		CHECK(decoded.member_count == 3 && decoded.members == NULL);
		CHECK(fsimg_job_path(&decoded, 0) == NULL);
		CHECK(fsimg_job_paths_length(&decoded, &paths_length, &error) ==
		    FSIMG_EINVAL);
		CHECK(error.status == FSIMG_EINVAL);
		CHECK(decoded.root_count == 0);
		CHECK(selection_length == strlen(members[0]) + 1 +
		    strlen(members[1]) + 1 + strlen(members[2]) + 1);
		CHECK((capabilities & FSIMG_JOB_CAP_ROOTS) == 0);
	}
}

static void
test_select_volume_replaces_read_job_members(void)
{
	const char *const members[] = { "/A", "/B/file" };
	const fsimg_job_kind_t kinds[] = { FSIMG_JOB_LIST, FSIMG_JOB_EXTRACT };
	fsimg_worker_request_t request;
	uint64_t paths_length;

	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		fsimg_worker_request_init(&request);
		request.kind = kinds[i];
		request.members = members;
		request.member_count = 2;
		fsimg_job_select_volume(&request);
		CHECK(request.member_count == 1);
		CHECK(fsimg_job_path_count(&request) == 1);
		CHECK(strcmp(request.members[0], "/") == 0);
		CHECK(fsimg_job_paths_length(&request, &paths_length, NULL) == FSIMG_OK);
		CHECK(paths_length == 2);
	}
}

static void
test_read_job_zero_members_is_invalid(void)
{
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection_length;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_LIST;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	/* cmd/fsimg/job.c JOB_PATHS_OFFSET holds the eight-byte path count.
	 * Check the valid record before corrupting it so layout drift fails.
	 */
	CHECK(fsimg_load_le64(raw + 1024) == request.member_count);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection_length,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.member_count == request.member_count);
	fsimg_store_le64(raw + 1024, 0);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection_length,
	    &capabilities, NULL) == FSIMG_EINVAL);
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
		{ 1032, FSIMG_SELECT_DISK + 1 }, { 1040, UINT64_C(1) << 32 },
		{ 1048, UINT64_C(1) << 32 }
	};
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES], mutated[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_LIST;
	request.members = (const char *const[]){ "/" };
	request.member_count = 1;
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
		fsimg_store_le64(mutated + bad[i].offset, bad[i].value);
		CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded,
		    &selection, &capabilities, NULL) == FSIMG_EINVAL);
	}
	bcopy(raw, mutated, sizeof(raw));
	mutated[1056 + FSIMG_LAYER_CONTAINER] = FSIMG_SLOT_NAMED + 1;
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	bcopy(raw, mutated, sizeof(raw));
	memset(mutated + 1096, 'A', 112);
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	bcopy(raw, mutated, sizeof(raw));
	mutated[1056 + FSIMG_LAYER_CONTAINER] = FSIMG_SLOT_NAMED;
	strcpy((char *)mutated + 1080, "mbr");
	CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
}

static void
test_job_force_requires_a_read_and_named_layer_or_compression(void)
{
	static const fsimg_job_kind_t reads[] = {
		FSIMG_JOB_INFO, FSIMG_JOB_LIST, FSIMG_JOB_EXTRACT, FSIMG_JOB_VERIFY
	};
	static const fsimg_job_kind_t writes[] = { FSIMG_JOB_CREATE, FSIMG_JOB_PLAN };
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	for (size_t i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
		fsimg_worker_request_init(&request);
		request.kind = reads[i];
		request.flags = FSIMG_JOB_FORCE;
		CHECK(fsimg_stack_parse("fat12/-", &request.stack, NULL) == FSIMG_OK);
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK);
		CHECK(decoded.flags == FSIMG_JOB_FORCE && decoded.kind == reads[i]);
		CHECK(strcmp(fsimg_job_format(&decoded), "fat12") == 0);
		CHECK(fsimg_stack_parse("mbr", &request.stack, NULL) == FSIMG_OK);
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK);
		CHECK(decoded.flags == FSIMG_JOB_FORCE && decoded.kind == reads[i]);
		CHECK(decoded.stack.slots[FSIMG_LAYER_FILESYSTEM].state ==
		    FSIMG_SLOT_UNSPECIFIED);
		CHECK(decoded.stack.slots[FSIMG_LAYER_SCHEME].state == FSIMG_SLOT_NAMED);
		CHECK(strcmp(decoded.stack.slots[FSIMG_LAYER_SCHEME].name, "mbr") == 0);
		bzero(&request.stack, sizeof(request.stack));
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_EINVAL);
		strcpy(request.compression, "gzip");
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK);
		CHECK(decoded.flags == FSIMG_JOB_FORCE && decoded.kind == reads[i]);
		CHECK(strcmp(decoded.compression, "gzip") == 0);
		for (unsigned slot = 0; slot < FSIMG_STACK_SLOTS; slot++)
			CHECK(decoded.stack.slots[slot].state == FSIMG_SLOT_UNSPECIFIED);
	}
	for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
		fsimg_worker_request_init(&request);
		request.kind = writes[i];
		request.root_paths = image_root;
		request.root_count = 1;
		request.flags = FSIMG_JOB_FORCE;
		CHECK(fsimg_stack_parse("fat12/-", &request.stack, NULL) == FSIMG_OK);
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_EINVAL);
	}
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
		{ 1224, UINT64_C(1) << 32 }, { 1232, 4 }, { 1232, 0x100 },
		{ 1520, UINT64_C(1) << 32 }
	};
	static const unsigned text_ends[] = { 1311, 1423, 1439 };
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
		fsimg_store_le64(mutated + bad[i].offset, bad[i].value);
		CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded,
		    &selection, &capabilities, NULL) == FSIMG_EINVAL);
	}
	bcopy(raw, mutated, sizeof(raw));
	mutated[1232] = 0x02;
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

/* A value of a row's kind other than its default. */
static void
other_value(const fsimg_setting_t *row, fsimg_setting_value_t *value)
{
	static const fsimg_placement_t placement = { .path = "/a@b", .offset = 42 };
	const fsimg_setting_choice_t *choice;

	*value = row->initial;
	for (choice = row->choices; choice != NULL && choice->spelling != NULL;
	    choice++) {
		if (!same_value(row, &choice->value, &row->initial)) {
			*value = choice->value;
			return;
		}
	}
	switch (row->kind) {
	case FSIMG_KIND_PLACEMENTS:
		value->placements.items = &placement;
		value->placements.count = 1;
		break;
	case FSIMG_KIND_XZ_CHECK:
		value->xz_check = FSIMG_XZ_SHA256;
		break;
	case FSIMG_KIND_U32:
		value->u32 = row->initial.u32 + 1;
		break;
	case FSIMG_KIND_U64:
		value->u64 = UINT64_C(1) << 40;
		break;
	case FSIMG_KIND_I64:
		value->i64 = -INT64_C(62135596800);
		break;
	case FSIMG_KIND_BOOL:
		value->flag = true;
		break;
	case FSIMG_KIND_TEXT:
		value->text = "x";
		break;
	case FSIMG_KIND_ID16:
		memset(value->id, 0x5a, sizeof(value->id));
		break;
	case FSIMG_KIND_TEXT_POINTER:
		value->text = "m";
		break;
	default:
		CHECK(!"a transported kind");
	}
}

static bool
same_value(const fsimg_setting_t *row, const fsimg_setting_value_t *a,
    const fsimg_setting_value_t *b)
{
	switch (row->kind) {
	case FSIMG_KIND_PLACEMENTS:
		if (a->placements.count != b->placements.count)
			return (false);
		for (uint32_t i = 0; i < a->placements.count; i++) {
			if (a->placements.items[i].offset != b->placements.items[i].offset ||
			    strcmp(a->placements.items[i].path, b->placements.items[i].path) != 0)
				return (false);
		}
		return (true);
	case FSIMG_KIND_U32:
		return (a->u32 == b->u32);
	case FSIMG_KIND_U64:
		return (a->u64 == b->u64);
	case FSIMG_KIND_I64:
		return (a->i64 == b->i64);
	case FSIMG_KIND_BOOL:
		return (a->flag == b->flag);
	case FSIMG_KIND_ISO_VIEW:
		return (a->iso_view == b->iso_view);
	case FSIMG_KIND_BYTE_ORDER:
		return (a->byte_order == b->byte_order);
	case FSIMG_KIND_DMG_COMPRESSION:
		return (a->dmg_compression == b->dmg_compression);
	case FSIMG_KIND_XZ_CHECK:
		return (a->xz_check == b->xz_check);
	case FSIMG_KIND_IFSBOOT_COMPRESSION:
		return (a->ifsboot_compression == b->ifsboot_compression);
	case FSIMG_KIND_EROFS_COMPRESSION:
		return (a->erofs_compression == b->erofs_compression);
	case FSIMG_KIND_EROFS_METABOX:
		return (a->erofs_metabox == b->erofs_metabox);
	case FSIMG_KIND_TEXT:
	case FSIMG_KIND_TEXT_POINTER:
		return ((a->text == NULL) == (b->text == NULL) &&
		    (a->text == NULL || strcmp(a->text, b->text) == 0));
	case FSIMG_KIND_ID16:
		return (bcmp(a->id, b->id, sizeof(a->id)) == 0);
	default:
		return (false);
	}
}

/* One row's non-default value, with its presence flag, survives its read
 * or create job's encoding and decoding.
 */
static void
check_transported(const fsimg_setting_t *row)
{
	fsimg_worker_request_t request, decoded;
	fsimg_setting_value_t value, received;
	unsigned char raw[FSIMG_JOB_BYTES];
	fsimg_setting_records_t records;
	uint64_t selection;
	unsigned capabilities;
	char key[32];

	fsimg_worker_request_init(&request);
	request.kind = row->record == FSIMG_RECORD_READ ?
	    FSIMG_JOB_LIST : FSIMG_JOB_CREATE;
	if (request.kind == FSIMG_JOB_CREATE) {
		request.root_paths = image_root;
		request.root_count = 1;
	}
	other_value(row, &value);
	CHECK(!same_value(row, &value, &row->initial));
	fsimg_setting_store(row, &(fsimg_setting_targets_t){
	    .write = &request.write, .read = &request.read,
	    .layers = &request.layers }, &value);
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	records = (fsimg_setting_records_t){ .write = &decoded.write,
	    .read = &decoded.read,
	    .layers = &decoded.layers };
	fsimg_setting_load(row, &records, &received);
	fsimg_setting_key(row, key, sizeof(key));
	if (!same_value(row, &value, &received) ||
	    fsimg_setting_is_default(row, &records))
		fprintf(stderr, "%s does not travel in the job record\n", key);
	CHECK(same_value(row, &value, &received));
	CHECK(!fsimg_setting_is_default(row, &records));
}

/*
 * Every stored setting of the catalogue travels in the job record: the
 * codec keeps its explicit layout, and this coverage, which names rows
 * only by catalogue identity, fails for a row added without a transport
 * decision. File settings travel as descriptors and oem stores nothing.
 */
static void
test_job_catalogue(void)
{
	const fsimg_setting_t *row;
	const fsimg_layer_t *layer;

	for (size_t i = 0; i < fsimg_filesystem_settings.count; i++) {
		row = &fsimg_filesystem_settings.rows[i];
		if (row->record == FSIMG_RECORD_WRITE || row->record == FSIMG_RECORD_READ)
			check_transported(row);
	}
	for (size_t i = 0; (layer = fsimg_layer_at(i)) != NULL; i++) {
		for (size_t j = 0; layer->settings != NULL &&
		    j < layer->settings->count; j++) {
			row = &layer->settings->rows[j];
			if (row->record == FSIMG_RECORD_LAYERS)
				check_transported(row);
		}
	}
}

/* A scheme's available and missing bytes describe a representable end. */
static void
test_image_declared_end_is_representable(void)
{
	typedef struct image_missing_case {
		uint64_t missing;
		bool valid;
	} image_missing_case_t;
	static const image_missing_case_t cases[] = {
		{ .missing = UINT64_MAX - 4096, .valid = true },
		{ .missing = UINT64_MAX - 4095, .valid = false }
	};
	fsimg_frame_t frame = { .type = FSIMG_FRAME_IMAGE,
	    .length = FSIMG_WIRE_IMAGE_BYTES };
	bool valid, matched = true;

	fsimg_store_le64(frame.payload + FSIMG_WIRE_IMAGE_DISK, 4096);
	fsimg_store_le64(frame.payload + FSIMG_WIRE_IMAGE_FILE, 4096);
	strcpy((char *)frame.payload + FSIMG_WIRE_IMAGE_SCHEME, "mbr");
	fsimg_store_le64(frame.payload + FSIMG_WIRE_IMAGE_SECTOR, 512);
	fsimg_store_le64(frame.payload + FSIMG_WIRE_IMAGE_SLOTS, 4);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fsimg_store_le64(frame.payload + FSIMG_WIRE_IMAGE_MISSING, cases[i].missing);
		valid = fsimg_wire_image_frame_valid(&frame);
		if (valid != cases[i].valid)
			fprintf(stderr, "IMAGE declared end case %zu: missing=%llu expected %d, received %d\n",
			    i, (unsigned long long)cases[i].missing, cases[i].valid, valid);
		matched &= valid == cases[i].valid;
	}
	CHECK(matched);
}

/* Clipping keeps the available prefix; an absent partition has no claim.
 * Offset, available length and missing bytes describe a representable end.
 */
static void
test_partition_missing_bytes_match_disk_extent(void)
{
	typedef struct partition_missing_case {
		uint64_t offset;
		uint64_t length;
		uint64_t missing;
		fsimg_claim_t claim;
		bool valid;
	} partition_missing_case_t;
	const uint64_t disk_bytes = 4096;
	const uint64_t slot_count = 4;
	static const partition_missing_case_t cases[] = {
		/* Whole slots. */
		{ .offset = 512, .length = 3584, .missing = 0, .claim = FSIMG_CLAIM_NONE, .valid = true },
		{ .offset = 512, .length = 512, .missing = 0, .claim = FSIMG_CLAIM_NONE, .valid = true },
		/* Clipped slots. */
		{ .offset = 512, .length = 3584, .missing = 512, .claim = FSIMG_CLAIM_NONE, .valid = true },
		{ .offset = 512, .length = 3584, .missing = 512, .claim = FSIMG_CLAIM_FORMAT, .valid = true },
		/* Wholly absent slots. */
		{ .offset = 4096, .length = 0, .missing = 512, .claim = FSIMG_CLAIM_NONE, .valid = true },
		{ .offset = 8192, .length = 0, .missing = 512, .claim = FSIMG_CLAIM_NONE, .valid = true },
		{ .offset = UINT64_MAX - 512, .length = 0, .missing = 512, .claim = FSIMG_CLAIM_NONE, .valid = true },
		/* Impossible extents. */
		{ .offset = 512, .length = 0, .missing = 512, .claim = FSIMG_CLAIM_NONE, .valid = false },
		{ .offset = 512, .length = 512, .missing = 512, .claim = FSIMG_CLAIM_NONE, .valid = false },
		{ .offset = 4096, .length = 512, .missing = 512, .claim = FSIMG_CLAIM_NONE, .valid = false },
		{ .offset = 8192, .length = 512, .missing = 512, .claim = FSIMG_CLAIM_NONE, .valid = false },
		{ .offset = 512, .length = 3585, .missing = 512, .claim = FSIMG_CLAIM_NONE, .valid = false },
		{ .offset = 4096, .length = 0, .missing = 0, .claim = FSIMG_CLAIM_NONE, .valid = false },
		/* Declared-end overflow boundary. */
		{ .offset = 512, .length = 3584, .missing = UINT64_MAX - 4096, .claim = FSIMG_CLAIM_NONE, .valid = true },
		{ .offset = 8192, .length = 0, .missing = UINT64_MAX, .claim = FSIMG_CLAIM_NONE, .valid = false },
		{ .offset = 512, .length = 3584, .missing = UINT64_MAX - 4095, .claim = FSIMG_CLAIM_NONE, .valid = false },
		/* Claimed although absent. */
		{ .offset = 4096, .length = 0, .missing = 512, .claim = FSIMG_CLAIM_UNSUPPORTED, .valid = false },
		{ .offset = 4096, .length = 0, .missing = 512, .claim = FSIMG_CLAIM_CORRUPT, .valid = false },
		{ .offset = 4096, .length = 0, .missing = 512, .claim = FSIMG_CLAIM_AMBIGUOUS, .valid = false },
		{ .offset = 4096, .length = 0, .missing = 512, .claim = FSIMG_CLAIM_FORMAT, .valid = false }
	};
	fsimg_frame_t frame = { .type = FSIMG_FRAME_PARTITION,
	    .length = FSIMG_WIRE_PARTITION_BYTES };
	bool valid, matched = true;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		bzero(frame.payload, frame.length);
		fsimg_store_le64(frame.payload + FSIMG_WIRE_PARTITION_INDEX, 1);
		fsimg_store_le64(frame.payload + FSIMG_WIRE_PARTITION_OFFSET, cases[i].offset);
		fsimg_store_le64(frame.payload + FSIMG_WIRE_PARTITION_LENGTH, cases[i].length);
		fsimg_store_le64(frame.payload + FSIMG_WIRE_PARTITION_MISSING, cases[i].missing);
		fsimg_store_le64(frame.payload + FSIMG_WIRE_PARTITION_CLAIM, cases[i].claim);
		if (cases[i].claim == FSIMG_CLAIM_FORMAT)
			strcpy((char *)frame.payload + FSIMG_WIRE_PARTITION_FORMAT, "fat16");
		valid = fsimg_wire_partition_frame_valid(&frame, slot_count, disk_bytes);
		if (valid != cases[i].valid)
			fprintf(stderr, "PARTITION missing case %zu: offset=%llu length=%llu missing=%llu claim=%d expected %d, received %d\n",
			    i, (unsigned long long)cases[i].offset,
			    (unsigned long long)cases[i].length,
			    (unsigned long long)cases[i].missing, cases[i].claim,
			    cases[i].valid, valid);
		matched &= valid == cases[i].valid;
	}
	CHECK(matched);
}

/* Missing partitions retain their declared offset even beyond the disk. */
static void
test_missing_partition_records(void)
{
	fsimg_partition_info_t partition = { .index = 2, .used = true,
	    .offset = 3145728, .missing_bytes = 524288,
	    .claim = FSIMG_CLAIM_NONE };
	fsimg_frame_t frame;

	bzero(&frame, sizeof(frame));
	frame.type = FSIMG_FRAME_PARTITION;
	frame.length = FSIMG_WIRE_PARTITION_BYTES;
	fsimg_wire_partition_encode(frame.payload, &partition);
	CHECK(fsimg_load_le64(frame.payload + FSIMG_WIRE_PARTITION_MISSING) == 524288);
	CHECK(fsimg_wire_partition_frame_valid(&frame, 4, 2097152));

	partition.missing_bytes = 0;
	fsimg_wire_partition_encode(frame.payload, &partition);
	CHECK(!fsimg_wire_partition_frame_valid(&frame, 4, 2097152));
	partition.offset = 1048576;
	fsimg_wire_partition_encode(frame.payload, &partition);
	CHECK(!fsimg_wire_partition_frame_valid(&frame, 4, 2097152));

	partition.missing_bytes = 524288;
	partition.length = 1048577;
	fsimg_wire_partition_encode(frame.payload, &partition);
	CHECK(!fsimg_wire_partition_frame_valid(&frame, 4, 2097152));
}

/* Scheme descriptors decide which large sector the image frame accepts. */
static void
test_image_sector_sizes_follow_the_scheme(void)
{
	static const struct {
		const char *scheme;
		uint32_t large_sector;
	} cases[] = { { "gpt", 4096 }, { "apm", 2048 }, { "mbr", 0 } };
	static const uint32_t sectors[] = { 0, 512, 2048, 4096 };
	fsimg_frame_t frame = { .type = FSIMG_FRAME_IMAGE,
	    .length = FSIMG_WIRE_IMAGE_BYTES };

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		for (size_t j = 0; j < sizeof(sectors) / sizeof(sectors[0]); j++) {
			bzero(frame.payload, frame.length);
			strcpy((char *)frame.payload + FSIMG_WIRE_IMAGE_STACK, cases[i].scheme);
			strcpy((char *)frame.payload + FSIMG_WIRE_IMAGE_SCHEME, cases[i].scheme);
			fsimg_store_le64(frame.payload + FSIMG_WIRE_IMAGE_DISK, 8388608);
			fsimg_store_le64(frame.payload + FSIMG_WIRE_IMAGE_FILE, 8388608);
			fsimg_store_le64(frame.payload + FSIMG_WIRE_IMAGE_SLOTS, 3);
			fsimg_store_le64(frame.payload + FSIMG_WIRE_IMAGE_SECTOR, sectors[j]);
			CHECK(fsimg_wire_image_frame_valid(&frame) ==
			    (sectors[j] == 512 || (sectors[j] != 0 &&
			    sectors[j] == cases[i].large_sector)));
		}
	}
}

/* APM's type text field is zero-filled and bounded independently of
 * the partition name. A partition frame must include the complete type field.
 */
static void
test_partition_frame_preserves_and_bounds_apm_type(void)
{
	fsimg_partition_info_t partition = { .index = 2, .used = true,
	    .offset = 32768, .length = 1048576 };
	fsimg_frame_t frame = { .type = FSIMG_FRAME_PARTITION,
	    .length = FSIMG_WIRE_PARTITION_BYTES };
	unsigned char *type = frame.payload + FSIMG_WIRE_PARTITION_APM_TYPE;

	strcpy(partition.apm_type, "Apple_HFS");
	memset(frame.payload, 0xa5, frame.length);
	fsimg_wire_partition_encode(frame.payload, &partition);
	CHECK(strcmp((const char *)type, "Apple_HFS") == 0);
	for (size_t i = strlen(partition.apm_type); i < FSIMG_WIRE_PARTITION_APM_TYPE_BYTES; i++)
		CHECK(type[i] == 0);
	CHECK(fsimg_wire_partition_frame_valid(&frame, 3, 2097152));
	frame.length -= FSIMG_WIRE_PARTITION_APM_TYPE_BYTES;
	CHECK(!fsimg_wire_partition_frame_valid(&frame, 3, 2097152));
	frame.length = FSIMG_WIRE_PARTITION_BYTES;
	memset(partition.apm_type, 'A', 32);
	partition.apm_type[32] = '\0';
	fsimg_wire_partition_encode(frame.payload, &partition);
	CHECK(strcmp((const char *)type, partition.apm_type) == 0);
	CHECK(fsimg_wire_partition_frame_valid(&frame, 3, 2097152));
	memset(type, 'A', FSIMG_WIRE_PARTITION_APM_TYPE_BYTES);
	CHECK(!fsimg_wire_partition_frame_valid(&frame, 3, 2097152));
	type[FSIMG_WIRE_PARTITION_APM_TYPE_BYTES - 1] = '\0';
	CHECK(fsimg_wire_partition_frame_valid(&frame, 3, 2097152));
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
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_DISK, 8388608);
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FILE, 8388608);
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_STACK, "fat32");
	CHECK(fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_MISSING, 123);
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_MISSING, 0);
	CHECK(fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FILE, 8388607);
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FILE, 8388608);
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "gzip");
	CHECK(fsimg_wire_image_frame_valid(&frame));
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "zstd");
#ifdef FSIMG_ZSTD
	CHECK(fsimg_wire_image_frame_valid(&frame));
#else
	CHECK(!fsimg_wire_image_frame_valid(&frame));
#endif
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "dmg");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	bzero(raw + FSIMG_WIRE_IMAGE_COMPRESSION, 16);
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_SLOTS, 4);
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_SCHEME, "mbr");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_SECTOR, 512);
	CHECK(fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_MISSING, 123);
	CHECK(fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FLAGS, FSIMG_WIRE_IMAGE_VERIFIED);
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_MISSING, 0);
	CHECK(fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FLAGS, 0);
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_SELECTED, 5);
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_SELECTED, 1);
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_REASON, "stale");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FLAGS, FSIMG_WIRE_IMAGE_UNSELECTED);
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_SELECTED, 0);
	CHECK(fsimg_wire_image_frame_valid(&frame));
	raw[FSIMG_WIRE_IMAGE_REASON] = '\0';
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	raw[FSIMG_WIRE_IMAGE_REASON] = 's';
	memset(raw + FSIMG_WIRE_IMAGE_STACK, 'x', FSIMG_WIRE_IMAGE_STACK_BYTES);
	CHECK(!fsimg_wire_image_frame_valid(&frame));

	/* Only a container has header details, and only a verified one
	 * unverified fields, each a word of printable ASCII.
	 */
	bzero(&frame, sizeof(frame));
	frame.type = FSIMG_FRAME_IMAGE;
	frame.length = FSIMG_WIRE_IMAGE_BYTES;
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_DISK, 8388608);
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FILE, 4194368);
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_STACK, "ifs/hbcifs");
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_CONTAINER, "hbcifs");
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_CODECS, "lzo1x");
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_CHUNKS, 1);
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_DETAIL_COUNT, 1);
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_DETAILS, "field_0x14");
	CHECK(fsimg_wire_image_frame_valid(&frame));
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_UNVERIFIED, "0x14,0x1c");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FLAGS, FSIMG_WIRE_IMAGE_VERIFIED);
	CHECK(fsimg_wire_image_frame_valid(&frame));
	raw[FSIMG_WIRE_IMAGE_UNVERIFIED + 4] = ' ';
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	raw[FSIMG_WIRE_IMAGE_UNVERIFIED + 4] = ',';
	bzero(raw + FSIMG_WIRE_IMAGE_CONTAINER, 16);
	bzero(raw + FSIMG_WIRE_IMAGE_CODECS, FSIMG_WIRE_IMAGE_CODECS_BYTES);
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_CHUNKS, 0);
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FILE, 8388608);
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	bzero(raw + FSIMG_WIRE_IMAGE_UNVERIFIED, FSIMG_WIRE_IMAGE_UNVERIFIED_BYTES);
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_DETAIL_COUNT, 0);
	bzero(raw + FSIMG_WIRE_IMAGE_DETAILS, FSIMG_WIRE_INFO_DETAIL_BYTES);
	CHECK(fsimg_wire_image_frame_valid(&frame));
	/* Only a container has header fields, a full table of them at most,
	 * each a printable name, and zeros past the count. */
	{
		fsimg_volume_detail_t details[FSIMG_IMAGE_DETAILS_MAX + 1];

		bzero(details, sizeof(details));
		for (size_t i = 0; i < FSIMG_IMAGE_DETAILS_MAX + 1; i++) {
			snprintf(details[i].name, sizeof(details[i].name),
			    "field_%zu", i);
			details[i].value = 0x1000 + i;
		}
		bzero(&frame, sizeof(frame));
		frame.type = FSIMG_FRAME_IMAGE;
		frame.length = FSIMG_WIRE_IMAGE_BYTES;
		fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_DISK, 65536);
		fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FILE, 49184);
		strcpy((char *)raw + FSIMG_WIRE_IMAGE_STACK, "ifs/ifsboot");
		strcpy((char *)raw + FSIMG_WIRE_IMAGE_CONTAINER, "ifsboot");
		fsimg_wire_details_encode(raw, FSIMG_WIRE_IMAGE_DETAIL_COUNT,
		    FSIMG_WIRE_IMAGE_DETAILS, FSIMG_IMAGE_DETAILS_MAX, details,
		    FSIMG_IMAGE_DETAILS_MAX + 1);
		CHECK(fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_DETAIL_COUNT) ==
		    FSIMG_IMAGE_DETAILS_MAX);
		CHECK(fsimg_wire_image_frame_valid(&frame));
		fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_DETAIL_COUNT,
		    FSIMG_IMAGE_DETAILS_MAX + 1);
		CHECK(!fsimg_wire_image_frame_valid(&frame));
		fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_DETAIL_COUNT, 2);
		CHECK(!fsimg_wire_image_frame_valid(&frame));
		bzero(raw + FSIMG_WIRE_IMAGE_DETAILS + 2 *
		    FSIMG_WIRE_INFO_DETAIL_BYTES, (FSIMG_IMAGE_DETAILS_MAX - 2) *
		    FSIMG_WIRE_INFO_DETAIL_BYTES);
		CHECK(fsimg_wire_image_frame_valid(&frame));
		raw[FSIMG_WIRE_IMAGE_DETAILS + 3] = ' ';
		CHECK(!fsimg_wire_image_frame_valid(&frame));
		raw[FSIMG_WIRE_IMAGE_DETAILS + 3] = 'l';
		raw[FSIMG_WIRE_IMAGE_DETAILS] = '\0';
		CHECK(!fsimg_wire_image_frame_valid(&frame));
		raw[FSIMG_WIRE_IMAGE_DETAILS] = 'f';
		CHECK(fsimg_wire_image_frame_valid(&frame));
		bzero(raw + FSIMG_WIRE_IMAGE_CONTAINER, 16);
		fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_DISK, 49184);
		CHECK(!fsimg_wire_image_frame_valid(&frame));
		bzero(raw + FSIMG_WIRE_IMAGE_DETAIL_COUNT, 8 + 2 *
		    FSIMG_WIRE_INFO_DETAIL_BYTES);
		CHECK(fsimg_wire_image_frame_valid(&frame));
	}

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

/* Mutate independent wire fields of an otherwise valid device job. Neither
 * descriptor metadata nor a native struct can supply missing wire bounds.
 */
static void
test_job_device_input(void)
{
	enum { KIND = PLACEMENT_COUNT - 3 * 8 - 24 - 357, LENGTH = KIND + 8,
	    QUANTUM = LENGTH + 8, CAPABILITIES = 23 * 8 };
	static const struct {
		unsigned offset;
		uint64_t value;
	} invalid[] = {
		{ KIND, FSIMG_INPUT_STREAM + 1 },
		{ LENGTH, 0 },
		{ LENGTH, 65535 },
		{ LENGTH, UINT64_C(1) << 63 },
		{ QUANTUM, 0 },
		{ QUANTUM, 1536 },
		{ KIND, FSIMG_INPUT_FILE },
		{ KIND, FSIMG_INPUT_ABSENT },
		{ KIND, FSIMG_INPUT_STREAM },
		{ CAPABILITIES, 0 }
	};
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES], mutated[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.input_fd = 0;
	request.input = (fsimg_input_t){ .kind = FSIMG_INPUT_DEVICE,
	    .length = 65536, .quantum = 4096 };
	request.kind = FSIMG_JOB_INFO;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.input.kind == FSIMG_INPUT_DEVICE &&
	    decoded.input.length == 65536 && decoded.input.quantum == 4096);
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		bcopy(raw, mutated, sizeof(raw));
		fsimg_store_le64(mutated + invalid[i].offset, invalid[i].value);
		CHECK(fsimg_job_decode(mutated, sizeof(mutated), &decoded,
		    &selection, &capabilities, NULL) == FSIMG_EINVAL);
	}
	/* Absent and relay descriptions have no geometry. A device cannot
	 * enter through a create job's image role.
	 */
	for (fsimg_input_kind_t kind = FSIMG_INPUT_ABSENT;
	    kind <= FSIMG_INPUT_STREAM; kind++) {
		if (kind != FSIMG_INPUT_ABSENT && kind != FSIMG_INPUT_STREAM)
			continue;
		request.input_fd = -1;
		request.input = (fsimg_input_t){ .kind = kind };
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK);
		fsimg_store_le64(raw + QUANTUM, 1);
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_EINVAL);
	}
	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	request.input_fd = 0;
	request.input = (fsimg_input_t){ .kind = FSIMG_INPUT_DEVICE,
	    .length = 65536, .quantum = 512 };
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
}

/* Pin the plan's wire size and geometry offsets separately from the codec:
 * a 160-byte record and inconsistent inode counts are invalid.
 */
static void
test_plan_geometry_record(void)
{
	fsimg_frame_t frame;
	fsimg_frame_decoder_t decoder;
	unsigned char encoded[FSIMG_FRAME_HEADER + FSIMG_WIRE_PLAN_BYTES];
	unsigned char *raw = frame.payload;
	size_t consumed;

	bzero(&frame, sizeof(frame));
	frame.type = FSIMG_FRAME_PLAN;
	frame.length = 224;
	CHECK(FSIMG_WIRE_PLAN_BYTES == 224);
	CHECK(FSIMG_WIRE_PLAN_CODEC_BYTES == 64);
	CHECK(FSIMG_WIRE_PLAN_UNITS == 128);
	CHECK(FSIMG_WIRE_PLAN_INODES == 136);
	CHECK(FSIMG_WIRE_PLAN_FREE_INODES == 144);
	CHECK(FSIMG_WIRE_PLAN_ROOT == 152);
	CHECK(FSIMG_WIRE_PLAN_CODEC == 160);
	fsimg_store_le64(raw + FSIMG_WIRE_PLAN_VOLUME, 8695808);
	fsimg_store_le64(raw + FSIMG_WIRE_PLAN_OUTPUT, 8695808);
	fsimg_store_le64(raw + FSIMG_WIRE_PLAN_CLUSTER, 2048);
	fsimg_store_le64(raw + FSIMG_WIRE_PLAN_SECTOR, 512);
	fsimg_store_le64(raw + 128, 4238);
	fsimg_store_le64(raw + 136, 16);
	fsimg_store_le64(raw + 144, 12);
	strcpy((char *)raw + FSIMG_WIRE_PLAN_FORMAT, "qnx6");
	fsimg_store_le64(raw + 152, 65535);
	strcpy((char *)raw + 160, "zlib fixture-version, liblzma fixture-version");
	CHECK(fsimg_load_le64(raw + FSIMG_WIRE_PLAN_ROOT) == 65535);
	CHECK(fsimg_wire_plan_frame_valid(&frame));
	CHECK(fsimg_frame_header(encoded, FSIMG_FRAME_PLAN,
	    FSIMG_WIRE_PLAN_BYTES, NULL) == FSIMG_OK);
	bcopy(raw, encoded + FSIMG_FRAME_HEADER, FSIMG_WIRE_PLAN_BYTES);
	for (size_t split = 0; split <= sizeof(encoded); split++) {
		bzero(&decoder, sizeof(decoder));
		CHECK(fsimg_frame_feed(&decoder, encoded, split,
		    &consumed, NULL) == FSIMG_OK);
		CHECK(consumed == split);
		if (split < sizeof(encoded)) {
			CHECK(fsimg_frame_feed(&decoder, encoded + split,
			    sizeof(encoded) - split, &consumed, NULL) == FSIMG_OK);
			CHECK(consumed == sizeof(encoded) - split);
		}
		CHECK(decoder.complete);
		CHECK(fsimg_wire_plan_frame_valid(&decoder.frame));
		CHECK(fsimg_load_le64(decoder.frame.payload +
		    FSIMG_WIRE_PLAN_ROOT) == 65535);
		CHECK(strcmp((char *)decoder.frame.payload +
		    FSIMG_WIRE_PLAN_CODEC,
		    "zlib fixture-version, liblzma fixture-version") == 0);
	}
	fsimg_store_le64(raw + 152, 65536);
	CHECK(!fsimg_wire_plan_frame_valid(&frame));
	fsimg_store_le64(raw + 152, 0);
	CHECK(fsimg_wire_plan_frame_valid(&frame));
	frame.length = 160;
	CHECK(!fsimg_wire_plan_frame_valid(&frame));
	frame.length = 192;
	CHECK(!fsimg_wire_plan_frame_valid(&frame));
	frame.length = 223;
	CHECK(!fsimg_wire_plan_frame_valid(&frame));
	frame.length = 225;
	CHECK(!fsimg_wire_plan_frame_valid(&frame));
	frame.length = 224;
	fsimg_store_le64(raw + 144, 16);
	CHECK(fsimg_wire_plan_frame_valid(&frame));
	fsimg_store_le64(raw + 144, 17);
	CHECK(!fsimg_wire_plan_frame_valid(&frame));
	fsimg_store_le64(raw + 136, 0);
	fsimg_store_le64(raw + 144, 1);
	CHECK(!fsimg_wire_plan_frame_valid(&frame));
	fsimg_store_le64(raw + 144, 0);
	CHECK(fsimg_wire_plan_frame_valid(&frame));
	fsimg_store_le64(raw + 136, UINT64_MAX);
	fsimg_store_le64(raw + 144, UINT64_MAX);
	CHECK(fsimg_wire_plan_frame_valid(&frame));
}

static void
test_job_minimize_flag(void)
{
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	CHECK(fsimg_stack_parse("qnx6", &request.stack, NULL) == FSIMG_OK);
	request.write.flags |= FSIMG_WRITE_MINIMIZE;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK((decoded.write.flags & FSIMG_WRITE_MINIMIZE) != 0);
}

/* The compression block follows the six ifsboot values and eight flags
 * bytes. Wire integers stay 64-bit even though public levels are 32-bit.
 */
static void
test_job_xz_settings_roundtrip_and_reject_invalid_wire_values(void)
{
	enum { XZ_LEVEL = 2634, XZ_CHECK = 2642 };
	static const uint32_t levels[] = { 0, 6, 9 };
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	CHECK(FSIMG_JOB_BYTES == 13415);
	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		for (unsigned check = FSIMG_XZ_CRC64; check <= FSIMG_XZ_SHA256; check++) {
			request.layers.xz_level = levels[i];
			request.layers.xz_check = (fsimg_xz_check_t)check;
			CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
			CHECK(fsimg_load_le64(raw + XZ_LEVEL) == levels[i]);
			CHECK(fsimg_load_le64(raw + XZ_CHECK) == check);
			CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded,
			    &selection, &capabilities, NULL) == FSIMG_OK);
			CHECK(decoded.layers.xz_level == levels[i]);
			CHECK(decoded.layers.xz_check == (fsimg_xz_check_t)check);
			fsimg_store_le64(raw + XZ_CHECK, FSIMG_XZ_SHA256 + 1);
			CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded,
			    &selection, &capabilities, NULL) == FSIMG_EINVAL);
			fsimg_store_le64(raw + XZ_CHECK, check);
			fsimg_store_le64(raw + XZ_LEVEL, (uint64_t)UINT32_MAX + 1);
			CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded,
			    &selection, &capabilities, NULL) == FSIMG_EINVAL);
		}
	}
}

/* The EROFS settings are three 64-bit fields after the ISO descriptor
 * settings: compression, addressing, metabox.
 */
static void
test_job_erofs_settings_roundtrip_and_reject_invalid_wire_values(void)
{
	enum { COMPRESSION = 5119, ADDRESSING = 5127, METABOX = 5135 };
	static const struct {
		fsimg_erofs_compression_t compression;
		uint32_t addressing;
		fsimg_erofs_metabox_t metabox;
	} cases[] = {
		{ FSIMG_EROFS_COMPRESSION_NONE, 0, FSIMG_EROFS_METABOX_NONE },
		{ FSIMG_EROFS_COMPRESSION_LZ4, 32, FSIMG_EROFS_METABOX_PLAIN },
		{ FSIMG_EROFS_COMPRESSION_NONE, 48, FSIMG_EROFS_METABOX_LZ4 },
		{ FSIMG_EROFS_COMPRESSION_LZ4, UINT32_MAX,
		    FSIMG_EROFS_METABOX_NONE }
	};
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	CHECK(PLACEMENT_COUNT == METABOX + 8);
	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		request.write.erofs_compression = cases[i].compression;
		request.write.erofs_addressing = cases[i].addressing;
		request.write.erofs_metabox = cases[i].metabox;
		memset(raw, 0xa5, sizeof(raw));
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK(fsimg_load_le64(raw + COMPRESSION) == (uint64_t)cases[i].compression);
		CHECK(fsimg_load_le64(raw + ADDRESSING) == cases[i].addressing);
		CHECK(fsimg_load_le64(raw + METABOX) == (uint64_t)cases[i].metabox);
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK);
		CHECK(decoded.write.erofs_compression == cases[i].compression);
		CHECK(decoded.write.erofs_addressing == cases[i].addressing);
		CHECK(decoded.write.erofs_metabox == cases[i].metabox);
	}
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	fsimg_store_le64(raw + COMPRESSION, FSIMG_EROFS_COMPRESSION_LZ4 + 1);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	fsimg_store_le64(raw + COMPRESSION, request.write.erofs_compression);
	fsimg_store_le64(raw + ADDRESSING, (uint64_t)UINT32_MAX + 1);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	fsimg_store_le64(raw + ADDRESSING, request.write.erofs_addressing);
	fsimg_store_le64(raw + METABOX, FSIMG_EROFS_METABOX_LZ4 + 1);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
	fsimg_store_le64(raw + METABOX, request.write.erofs_metabox);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
}

static void
test_xz_unverified_record_requires_verified_xz(void)
{
	fsimg_frame_t frame;
	unsigned char *raw = frame.payload;

	bzero(&frame, sizeof(frame));
	frame.type = FSIMG_FRAME_IMAGE;
	frame.length = FSIMG_WIRE_IMAGE_BYTES;
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_DISK, 1048576);
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FILE, 1048576);
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_STACK, "fat12");
	CHECK(fsimg_wire_image_frame_valid(&frame));
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "xz");
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_UNVERIFIED, "xz:check");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FLAGS, FSIMG_WIRE_IMAGE_VERIFIED);
#ifdef FSIMG_XZ
	CHECK(fsimg_wire_image_frame_valid(&frame));
#else
	CHECK(!fsimg_wire_image_frame_valid(&frame));
#endif
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "gzip");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "xz");
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_UNVERIFIED, "xz:other");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
}

static void
test_job_zstd_levels_roundtrip_and_reject_wide_wire_value(void)
{
	enum { ZSTD_LEVEL = 2650 };
	static const uint32_t levels[] = { 0, 3, 19 };
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		request.layers.zstd_level = levels[i];
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK(fsimg_load_le64(raw + ZSTD_LEVEL) == levels[i]);
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded,
		    &selection, &capabilities, NULL) == FSIMG_OK);
		CHECK(decoded.layers.zstd_level == levels[i]);
		fsimg_store_le64(raw + ZSTD_LEVEL, (uint64_t)UINT32_MAX + 1);
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded,
		    &selection, &capabilities, NULL) == FSIMG_EINVAL);
	}
}

static void
test_zstd_unverified_record_requires_verified_zstd(void)
{
	fsimg_frame_t frame;
	unsigned char *raw = frame.payload;

	bzero(&frame, sizeof(frame));
	frame.type = FSIMG_FRAME_IMAGE;
	frame.length = FSIMG_WIRE_IMAGE_BYTES;
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_DISK, 1048576);
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FILE, 1048576);
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_STACK, "fat12");
	CHECK(fsimg_wire_image_frame_valid(&frame));
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "zstd");
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_UNVERIFIED, "zstd:checksum");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FLAGS, FSIMG_WIRE_IMAGE_VERIFIED);
#ifdef FSIMG_ZSTD
	CHECK(fsimg_wire_image_frame_valid(&frame));
#else
	CHECK(!fsimg_wire_image_frame_valid(&frame));
#endif
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "xz");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "gzip");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "zstd");
	strcpy((char *)raw + FSIMG_WIRE_IMAGE_UNVERIFIED, "zstd:other");
	CHECK(!fsimg_wire_image_frame_valid(&frame));
}

/* Literal offsets relative to ISO_WIRE_BASE are the wire oracle,
 * independent of job.c: view 0, level 8, bootload 16, BIOS path 24,
 * EFI path 1049, presence 2074, reserved 2075..2079, input kind 2080,
 * input length 2088, and transfer quantum 2096.
 */
enum { ISO_WIRE_BASE = 2658, ISO_WIRE_TAIL_BYTES = 2104 };

static void
iso_wire_place_string(unsigned char *expected, size_t offset, const char *text)
{
	CHECK(offset < ISO_WIRE_TAIL_BYTES);
	CHECK(strlen(text) < ISO_WIRE_TAIL_BYTES - offset);
	bcopy(text, expected + offset, strlen(text) + 1);
}

static void
test_iso_jobs_have_canonical_wire_bytes(void)
{
	unsigned char create_tail[ISO_WIRE_TAIL_BYTES] = {
		[8] = 2, [16] = 8, [2074] = 3
	};
	unsigned char efi_tail[ISO_WIRE_TAIL_BYTES] = {
		[8] = 2, [16] = 0x34, [17] = 0x12, [2074] = 2
	};
	static const unsigned char read_tail[ISO_WIRE_TAIL_BYTES] = { [0] = 2 };
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	iso_wire_place_string(create_tail, 24, "/b/bios.img");
	iso_wire_place_string(efi_tail, 1049, "/efi/boot.img");
	CHECK(FSIMG_JOB_BYTES == 13415);
	fsimg_worker_request_init(&request);
	CHECK(request.read.iso_view == FSIMG_ISO_VIEW_AUTO);
	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	request.write.iso_level = 2;
	request.write.boot_load_sectors = 8;
	request.write.boot_bios = "/b/bios.img";
	request.write.boot_efi = "";
	memset(raw, 0xa5, sizeof(raw));
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(bcmp(raw + ISO_WIRE_BASE, create_tail, sizeof(create_tail)) == 0);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.write.iso_level == 2 && decoded.write.boot_load_sectors == 8);
	CHECK(strcmp(decoded.write.boot_bios, "/b/bios.img") == 0);
	CHECK(decoded.write.boot_bios == decoded.boot_bios_storage);
	CHECK(decoded.write.boot_efi != NULL && decoded.write.boot_efi[0] == '\0');
	CHECK(decoded.write.boot_efi == decoded.boot_efi_storage);
	request.write.boot_load_sectors = 0x1234;
	request.write.boot_bios = NULL;
	request.write.boot_efi = "/efi/boot.img";
	memset(raw, 0xa5, sizeof(raw));
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(bcmp(raw + ISO_WIRE_BASE, efi_tail, sizeof(efi_tail)) == 0);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.write.boot_load_sectors == 0x1234);
	CHECK(decoded.write.boot_bios == NULL);
	CHECK(strcmp(decoded.write.boot_efi, "/efi/boot.img") == 0);

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_LIST;
	request.read.iso_view = FSIMG_ISO_VIEW_JOLIET;
	memset(raw, 0xa5, sizeof(raw));
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(bcmp(raw + ISO_WIRE_BASE, read_tail, sizeof(read_tail)) == 0);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.read.iso_view == FSIMG_ISO_VIEW_JOLIET);
	CHECK(decoded.write.boot_bios == NULL && decoded.write.boot_efi == NULL);
}

static void
test_iso_jobs_reject_malformed_fields(void)
{
	static const struct {
		const char *name;
		unsigned offset;
		unsigned char value;
	} invalid[] = {
		{ "view beyond enum", 0, 4 },
		{ "view high byte", 7, 1 },
		{ "level beyond uint32", 12, 1 },
		{ "bootload beyond uint32", 20, 1 },
		{ "unknown presence", 2074, 4 },
		{ "BIOS without presence", 24, '/' },
		{ "EFI without presence", 1049, '/' },
		{ "reserved byte 1", 2075, 1 },
		{ "reserved byte 2", 2076, 1 },
		{ "reserved byte 3", 2077, 1 },
		{ "reserved byte 4", 2078, 1 },
		{ "reserved byte 5", 2079, 1 }
	};
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES], bad[FSIMG_JOB_BYTES];
	char path[1026];
	uint64_t selection;
	unsigned capabilities;
	fsimg_status_t status;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_LIST;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		bcopy(raw, bad, sizeof(raw));
		bad[ISO_WIRE_BASE + invalid[i].offset] = invalid[i].value;
		status = fsimg_job_decode(bad, sizeof(bad), &decoded,
		    &selection, &capabilities, NULL);
		if (status != FSIMG_EINVAL)
			fprintf(stderr, "%s accepted\n", invalid[i].name);
		CHECK(status == FSIMG_EINVAL);
	}
	for (unsigned field = 0; field < 2; field++) {
		size_t path_offset = ISO_WIRE_BASE + (field == 0 ? 24 : 1049);

		bcopy(raw, bad, sizeof(raw));
		bad[ISO_WIRE_BASE + 2074] = (unsigned char)(1u << field);
		/* A present empty string is valid; bytes after its NUL are not. */
		CHECK(fsimg_job_decode(bad, sizeof(bad), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK);
		bad[path_offset + 1] = 'x';
		CHECK(fsimg_job_decode(bad, sizeof(bad), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_EINVAL);
		memset(bad + path_offset, 'x', 1025);
		CHECK(fsimg_job_decode(bad, sizeof(bad), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_EINVAL);
		memset(path, 'x', 1024);
		path[1024] = '\0';
		request.write.boot_bios = field == 0 ? path : NULL;
		request.write.boot_efi = field == 1 ? path : NULL;
		CHECK(fsimg_job_encode(&request, bad, NULL) == FSIMG_OK);
		CHECK(fsimg_job_decode(bad, sizeof(bad), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK);
		CHECK(strlen(field == 0 ? decoded.write.boot_bios :
		    decoded.write.boot_efi) == 1024);
		path[1024] = 'x';
		path[1025] = '\0';
		CHECK(fsimg_job_encode(&request, bad, NULL) == FSIMG_EINVAL);
	}
}

static void
test_info_text_records_reject_invalid_counts_and_strings(void)
{
	fsimg_frame_t frame = { .type = FSIMG_FRAME_INFO,
	    .length = FSIMG_WIRE_INFO_BYTES };
	fsimg_volume_info_t info;
	unsigned char canonical[FSIMG_WIRE_INFO_BYTES];
	unsigned char *text = frame.payload + FSIMG_WIRE_INFO_TEXTS;
	static const uint64_t bad_counts[] = { 17, UINT64_MAX,
	    UINT64_C(1) << 32 };

	bzero(&info, sizeof(info));
	info.time_basis = FSIMG_TIME_UTC;
	info.scope = FSIMG_VALIDATION_HEADER;
	strcpy(info.format, "iso9660");
	info.text_count = 1;
	strcpy(info.texts[0].name, "application");
	strcpy(info.texts[0].value, "A \"QUOTED\" \\ APPLICATION");
	fsimg_wire_info_encode(frame.payload, &info);
	CHECK(FSIMG_WIRE_INFO_TEXT_COUNT == 2480);
	CHECK(FSIMG_WIRE_INFO_TEXTS == 2488);
	CHECK(FSIMG_WIRE_INFO_TEXT_BYTES == 224);
	CHECK(fsimg_load_le64(frame.payload + 2480) == 1);
	CHECK(strcmp((char *)text, "application") == 0);
	CHECK(strcmp((char *)text + 24, info.texts[0].value) == 0);
	CHECK(fsimg_wire_info_frame_valid(&frame));
	bcopy(frame.payload, canonical, sizeof(canonical));

	for (unsigned i = 0; i < sizeof(bad_counts) / sizeof(bad_counts[0]); i++) {
		fsimg_store_le64(frame.payload + 2480, bad_counts[i]);
		CHECK(!fsimg_wire_info_frame_valid(&frame));
	}
	fsimg_store_le64(frame.payload + 2480, 0);
	CHECK(!fsimg_wire_info_frame_valid(&frame));
	bcopy(canonical, frame.payload, sizeof(canonical));
	text[224] = 'X';
	CHECK(!fsimg_wire_info_frame_valid(&frame));
	bcopy(canonical, frame.payload, sizeof(canonical));
	memset(text + 24, 'X', 193);
	CHECK(!fsimg_wire_info_frame_valid(&frame));
	bcopy(canonical, frame.payload, sizeof(canonical));
	text[217] = 1;
	CHECK(!fsimg_wire_info_frame_valid(&frame));
	bcopy(canonical, frame.payload, sizeof(canonical));
	text[0] = '\0';
	CHECK(!fsimg_wire_info_frame_valid(&frame));

	/* Two texts of one name are not a report the encoder writes. */
	info.text_count = 2;
	strcpy(info.texts[1].name, "application");
	strcpy(info.texts[1].value, "OTHER");
	fsimg_wire_info_encode(frame.payload, &info);
	CHECK(!fsimg_wire_info_frame_valid(&frame));
	strcpy(info.texts[1].name, "publisher");
	fsimg_wire_info_encode(frame.payload, &info);
	CHECK(fsimg_wire_info_frame_valid(&frame));

	/* The capacity itself is valid, including a 192-byte value. */
	for (unsigned i = 0; i < FSIMG_VOLUME_TEXTS_MAX; i++) {
		snprintf(info.texts[i].name, sizeof(info.texts[i].name),
		    "text_%u", i);
		memset(info.texts[i].value, 'A', 192);
		info.texts[i].value[192] = '\0';
	}
	info.text_count = FSIMG_VOLUME_TEXTS_MAX;
	fsimg_wire_info_encode(frame.payload, &info);
	CHECK(fsimg_wire_info_frame_valid(&frame));
}

static void test_force_location_job_automatic_fs(void);

static void
test_force_location_job_automatic_fs(void)
{
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_LIST;
	request.flags = FSIMG_JOB_FORCE;
	request.selector = FSIMG_SELECT_AUTOMATIC;
	CHECK(fsimg_stack_parse("fat12", &request.stack, NULL) == FSIMG_OK);
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
}

static void test_force_location_job_automatic_scheme(void);

static void
test_force_location_job_automatic_scheme(void)
{
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_LIST;
	request.flags = FSIMG_JOB_FORCE;
	request.selector = FSIMG_SELECT_AUTOMATIC;
	CHECK(fsimg_stack_parse("fat12/mbr", &request.stack, NULL) == FSIMG_OK);
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_EINVAL);
}

static void test_force_location_job_empty_scheme(void);

static void
test_force_location_job_empty_scheme(void)
{
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_LIST;
	request.flags = FSIMG_JOB_FORCE;
	request.selector = FSIMG_SELECT_AUTOMATIC;
	CHECK(fsimg_stack_parse("fat12/-", &request.stack, NULL) == FSIMG_OK);
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
}

static void test_force_location_job_partition(void);

static void
test_force_location_job_partition(void)
{
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_LIST;
	request.flags = FSIMG_JOB_FORCE;
	request.selector = FSIMG_SELECT_INDEX;
	request.partition_index = 1;
	CHECK(fsimg_stack_parse("fat12/mbr", &request.stack, NULL) == FSIMG_OK);
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
}

static void test_force_location_job_scheme_only(void);

static void
test_force_location_job_scheme_only(void)
{
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_LIST;
	request.flags = FSIMG_JOB_FORCE;
	request.selector = FSIMG_SELECT_AUTOMATIC;
	CHECK(fsimg_stack_parse("mbr", &request.stack, NULL) == FSIMG_OK);
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
}

static void
test_job_placements_roundtrip_and_request_ownership(void)
{
	fsimg_worker_request_t request, decoded;
	fsimg_placement_t items[FSIMG_PLACEMENT_MAX];
	char paths[FSIMG_PLACEMENT_MAX][PLACEMENT_PATH_BYTES];
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	CHECK(FSIMG_JOB_BYTES == 13415);
	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	for (uint32_t i = 0; i < FSIMG_PLACEMENT_MAX; i++) {
		CHECK(snprintf(paths[i], sizeof(paths[i]), "/file@%u", i) > 0);
		items[i] = (fsimg_placement_t){ .path = paths[i], .offset = i };
	}
	memset(paths[7], 'p', 1024);
	paths[7][0] = '/';
	paths[7][1024] = '\0';
	items[7].offset = UINT64_MAX;
	request.write.placements = items;
	request.write.placement_count = FSIMG_PLACEMENT_MAX;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_load_le64(raw + PLACEMENT_COUNT) == FSIMG_PLACEMENT_MAX);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.write.placement_count == FSIMG_PLACEMENT_MAX);
	CHECK(decoded.write.placements == decoded.placement_storage);
	for (uint32_t i = 0; i < FSIMG_PLACEMENT_MAX; i++) {
		CHECK(decoded.write.placements[i].path == decoded.placement_paths[i]);
		CHECK(decoded.write.placements[i].path != items[i].path);
		CHECK(strcmp(decoded.write.placements[i].path, items[i].path) == 0);
		CHECK(decoded.write.placements[i].offset == items[i].offset);
	}
	bzero(raw, sizeof(raw));
	paths[0][1] = 'X';
	CHECK(strcmp(decoded.write.placements[0].path, "/file@0") == 0);
	request.write.placement_count = 0;
	request.write.placements = NULL;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.write.placement_count == 0 && decoded.write.placements == NULL);
}

static void
test_job_placement_decode_refusals_have_valid_controls(void)
{
	static const struct {
		const char *name;
		size_t offset;
		size_t length;
		unsigned char byte;
	} cases[] = {
		{ "count above capacity", PLACEMENT_COUNT, 1, 9 },
		{ "unterminated path", PLACEMENT_RECORDS + 8, PLACEMENT_PATH_BYTES, 'x' },
		{ "empty path", PLACEMENT_RECORDS + 8, 2, 0 },
		{ "path padding", PLACEMENT_RECORDS + 8 + 3, 1, 'x' },
		{ "unused offset", PLACEMENT_RECORDS + PLACEMENT_RECORD_BYTES, 1, 1 },
		{ "unused path", PLACEMENT_RECORDS + PLACEMENT_RECORD_BYTES + 8, 1, 'x' }
	};
	fsimg_worker_request_t request, decoded;
	fsimg_placement_t item = { .path = "/k", .offset = 0 };
	unsigned char raw[FSIMG_JOB_BYTES], bad[FSIMG_JOB_BYTES];
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	request.write.placements = &item;
	request.write.placement_count = 1;
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		bcopy(raw, bad, sizeof(bad));
		memset(bad + cases[i].offset, cases[i].byte, cases[i].length);
		status = fsimg_job_decode(bad, sizeof(bad), &decoded, &selection,
		    &capabilities, &error);
		if (status != FSIMG_EINVAL)
			fprintf(stderr, "%s: expected invalid job, got %d\n", cases[i].name, status);
		CHECK(status == FSIMG_EINVAL);
		CHECK(strcmp(error.message, "invalid worker job description") == 0);
		bcopy(raw + cases[i].offset, bad + cases[i].offset, cases[i].length);
		CHECK(fsimg_job_decode(bad, sizeof(bad), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK);
		CHECK(decoded.write.placement_count == 1);
		CHECK(strcmp(decoded.write.placements[0].path, "/k") == 0);
	}
}

static void
test_job_placement_encode_refusals_have_valid_controls(void)
{
	char unterminated[PLACEMENT_PATH_BYTES];
	fsimg_placement_t valid = { .path = "/k", .offset = 0 };
	fsimg_placement_t empty = { .path = "", .offset = 0 };
	fsimg_placement_t absent = { .path = NULL, .offset = 0 };
	fsimg_placement_t long_path = { .path = unterminated, .offset = 0 };
	const struct {
		const char *name;
		const fsimg_placement_t *items;
		uint32_t count;
	} cases[] = {
		{ "count above capacity", &valid, 9 },
		{ "absent array", NULL, 1 },
		{ "empty path", &empty, 1 },
		{ "absent path", &absent, 1 },
		{ "unterminated path", &long_path, 1 }
	};
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t selection;
	unsigned capabilities;

	memset(unterminated, 'x', sizeof(unterminated));
	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_CREATE;
	request.root_paths = image_root;
	request.root_count = 1;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		request.write.placements = cases[i].items;
		request.write.placement_count = cases[i].count;
		status = fsimg_job_encode(&request, raw, &error);
		if (status != FSIMG_EINVAL)
			fprintf(stderr, "%s: expected invalid job, got %d\n", cases[i].name, status);
		CHECK(status == FSIMG_EINVAL);
		CHECK(strcmp(error.message, "invalid worker job description") == 0);
		request.write.placements = &valid;
		request.write.placement_count = 1;
		CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
		CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
		    &capabilities, NULL) == FSIMG_OK);
		CHECK(decoded.write.placement_count == 1);
		CHECK(strcmp(decoded.write.placements[0].path, "/k") == 0);
	}
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
		write_seed(argv[2]);
		return (0);
	}
	CHECK(argc == 1);
	test_force_location_job_automatic_fs();
	test_force_location_job_automatic_scheme();
	test_force_location_job_empty_scheme();
	test_force_location_job_partition();
	test_force_location_job_scheme_only();
	test_iso_jobs_have_canonical_wire_bytes();
	test_iso_jobs_reject_malformed_fields();
	test_job_zstd_levels_roundtrip_and_reject_wide_wire_value();
	test_zstd_unverified_record_requires_verified_zstd();
	test_plan_geometry_record();
	test_job_xz_settings_roundtrip_and_reject_invalid_wire_values();
	test_job_erofs_settings_roundtrip_and_reject_invalid_wire_values();
	test_xz_unverified_record_requires_verified_xz();
	test_job_minimize_flag();
	test_job_qnx6_settings();
	test_job_mountpoint();
	test_job_ifsboot_settings();
	test_job_selection();
	test_read_job_members_roundtrip();
	test_select_volume_replaces_read_job_members();
	test_read_job_zero_members_is_invalid();
	test_job_force_requires_a_read_and_named_layer_or_compression();
	test_job_layers();
	test_job_catalogue();
	test_job_placements_roundtrip_and_request_ownership();
	test_job_placement_decode_refusals_have_valid_controls();
	test_job_placement_encode_refusals_have_valid_controls();
	test_image_sector_sizes_follow_the_scheme();
	test_partition_frame_preserves_and_bounds_apm_type();
	test_image_records();
	test_image_declared_end_is_representable();
	test_partition_missing_bytes_match_disk_extent();
	test_missing_partition_records();
	test_frame_splits(0);
	test_frame_splits(127);
	test_frame_splits(FSIMG_FRAME_BYTES);
	test_malformed_frames();
	test_error_codec();
	test_job_label();
	test_job_stream_flags();
	test_job_device_input();
	test_selection_budget();
	test_error_padding();
	test_entry_records();
	test_extra_data_entry_records();
	test_device_and_socket_entry_records();
	test_invalid_device_and_socket_entry_records();
	test_invalid_entry_records();
	test_info_text_records_reject_invalid_counts_and_strings();
	test_info_geometry_is_informational();
	test_info_missing_bytes_are_bounded();
	test_info_uuid();
	test_info_label();
	test_info_mountpoint();
	test_job_names_roundtrip_only_for_create_plan_and_extract();
	test_job_posix_flags();
	test_job_roots();
	test_signed_wire_values();
	test_partial_error_and_unknown_status_are_rejected();
	test_salvage_job_is_extract_only();
	return (0);
}
