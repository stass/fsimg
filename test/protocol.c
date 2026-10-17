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
static void test_error_padding(void);
static void write_seed(const char *);

static void
write_seed(const char *path)
{
	fsimg_worker_request_t request;
	fsimg_error_t error = { .status = FSIMG_ECORRUPT };
	unsigned char raw[FSIMG_WIRE_ERROR_BYTES];
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
test_job_label(void)
{
	fsimg_worker_request_t request, decoded;
	unsigned char raw[FSIMG_JOB_BYTES];
	uint64_t selection;
	unsigned capabilities;

	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_CREATE;
	request.write.label = "MYLABEL";
	CHECK(fsimg_job_encode(&request, raw, NULL) == FSIMG_OK);
	CHECK(fsimg_job_decode(raw, sizeof(raw), &decoded, &selection,
	    &capabilities, NULL) == FSIMG_OK);
	CHECK(decoded.write.label != NULL);
	CHECK(strcmp(decoded.write.label, "MYLABEL") == 0);
	CHECK(selection == 1 && capabilities == 0);

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

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
		write_seed(argv[2]);
		return (0);
	}
	CHECK(argc == 1);
	test_frame_splits(0);
	test_frame_splits(127);
	test_frame_splits(FSIMG_FRAME_BYTES);
	test_malformed_frames();
	test_error_codec();
	test_job_label();
	test_error_padding();
	return (0);
}
