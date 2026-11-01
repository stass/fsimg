/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "fsimg-endian.h"
#include "host.h"
#include "io.h"
#include "socket.h"
#include "test.h"
#include "worker.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FSIMG_TEST_ASAN 1
#endif
#endif

typedef enum worker_scenario {
	SCENARIO_DONE_BEFORE_READY = 0,
	SCENARIO_DONE_BEFORE_INFO,
	SCENARIO_BAD_EXIT,
	SCENARIO_DUPLICATE_DONE,
	SCENARIO_NAME_WITHOUT_ENTRY,
	SCENARIO_TRUNCATED_HEADER,
	SCENARIO_MISSING_DONE,
	SCENARIO_STALLED_PARSER,
	SCENARIO_LONG_LIST_NAME,
	SCENARIO_STARTUP_NONREADER,
	SCENARIO_PREMATURE_RAW,
	SCENARIO_BLOCKED_RAW,
	SCENARIO_BROKEN_CONSUMER,
	SCENARIO_STARTUP_ERROR,
	SCENARIO_STALLED_RELAY,
	SCENARIO_FORGED_PUBLISHED,
	SCENARIO_FORGED_UNCERTAIN,
	SCENARIO_ACK_ERROR,
	SCENARIO_INVALID_TIME_BASIS,
	SCENARIO_BLOCKED_WARNING,
	SCENARIO_SLOW_CONSUMER,
	SCENARIO_VERIFY_INVALID_SCOPE,
	SCENARIO_VERIFY_HEADER_SCOPE,
	SCENARIO_VERIFY_UNREQUESTED_PAYLOAD,
	SCENARIO_VERIFY_MISSING_PAYLOAD,
	SCENARIO_RUNTIME_FAILURE,
	SCENARIO_PARTIAL_EXTRACTION,
	SCENARIO_ASAN_FAILURE,
	SCENARIO_SKIPPED_EXTRACTION,
	SCENARIO_OMITTED_EXTRACTION,
	SCENARIO_INFO_WITHOUT_IMAGE,
	SCENARIO_IMAGE_WRONG_DISK,
	SCENARIO_PARTITION_OUTSIDE,
	SCENARIO_PARTITION_UNSELECTED_LIST,
	SCENARIO_VHD_WHOLE_FILE,
	SCENARIO_EXCESS_LOSS,
	SCENARIO_LIMIT_LOSS,
} worker_scenario_t;

/* The point at which a forged worker interrupts its ordinary result stream. */
typedef enum names_position {
	NAMES_BEFORE_HEADER,
	NAMES_AFTER_IMAGE,
	NAMES_AFTER_INFO,
	NAMES_AFTER_PLAN,
	NAMES_CHUNK_WITHOUT_ENTRY
} names_position_t;

typedef struct names_case {
	const char	*name;
	fsimg_job_kind_t kind;
	unsigned	 flags;
	names_position_t position;
	size_t		 names;
	uint64_t	 plan_entries;
	uint64_t	 done_entries;
	uint64_t	 done_skipped;
	bool		 encoded;
	const char	*message; /* NULL means success. */
} names_case_t;

static const names_case_t names_cases[] = {
	{ .name = "create refuses unsolicited entry", .kind = FSIMG_JOB_CREATE,
	  .names = 1, .message = "invalid worker entry start" },
	{ .name = "extract refuses unsolicited entry", .kind = FSIMG_JOB_EXTRACT,
	  .position = NAMES_AFTER_INFO, .names = 1,
	  .message = "invalid worker entry start" },
	{ .name = "create refuses entry after plan", .kind = FSIMG_JOB_CREATE,
	  .flags = FSIMG_JOB_NAMES, .position = NAMES_AFTER_PLAN, .names = 1,
	  .message = "invalid worker entry start" },
	{ .name = "encoded create refuses entry after image", .kind = FSIMG_JOB_CREATE,
	  .flags = FSIMG_JOB_NAMES, .position = NAMES_AFTER_IMAGE, .names = 1,
	  .encoded = true, .message = "invalid worker entry start" },
	{ .name = "create refuses more planned entries than names", .kind = FSIMG_JOB_CREATE,
	  .flags = FSIMG_JOB_NAMES, .names = 1, .plan_entries = 2, .done_entries = 2,
	  .message = "worker plan entry count differs from its names" },
	{ .name = "create refuses fewer planned entries than names", .kind = FSIMG_JOB_CREATE,
	  .flags = FSIMG_JOB_NAMES, .names = 2, .plan_entries = 1, .done_entries = 1,
	  .message = "worker plan entry count differs from its names" },
	{ .name = "create refuses more completed entries than planned", .kind = FSIMG_JOB_CREATE,
	  .plan_entries = 2, .done_entries = 3,
	  .message = "worker creation count differs from its plan" },
	{ .name = "create refuses fewer completed entries than planned", .kind = FSIMG_JOB_CREATE,
	  .plan_entries = 2, .done_entries = 1,
	  .message = "worker creation count differs from its plan" },
	{ .name = "verbose create refuses more completed entries than planned", .kind = FSIMG_JOB_CREATE,
	  .flags = FSIMG_JOB_NAMES, .names = 2, .plan_entries = 2, .done_entries = 3,
	  .message = "worker creation count differs from its plan" },
	{ .name = "verbose create refuses fewer completed entries than planned", .kind = FSIMG_JOB_CREATE,
	  .flags = FSIMG_JOB_NAMES, .names = 2, .plan_entries = 2, .done_entries = 1,
	  .message = "worker creation count differs from its plan" },
	{ .name = "plan refuses more completed entries than planned", .kind = FSIMG_JOB_PLAN,
	  .plan_entries = 2, .done_entries = 3,
	  .message = "worker creation count differs from its plan" },
	{ .name = "plan refuses fewer completed entries than planned", .kind = FSIMG_JOB_PLAN,
	  .plan_entries = 2, .done_entries = 1,
	  .message = "worker creation count differs from its plan" },
	{ .name = "verbose plan refuses more completed entries than planned", .kind = FSIMG_JOB_PLAN,
	  .flags = FSIMG_JOB_NAMES, .names = 2, .plan_entries = 2, .done_entries = 3,
	  .message = "worker creation count differs from its plan" },
	{ .name = "verbose plan refuses fewer completed entries than planned", .kind = FSIMG_JOB_PLAN,
	  .flags = FSIMG_JOB_NAMES, .names = 2, .plan_entries = 2, .done_entries = 1,
	  .message = "worker creation count differs from its plan" },
	{ .name = "extract refuses fewer completed entries", .kind = FSIMG_JOB_EXTRACT,
	  .flags = FSIMG_JOB_NAMES | FSIMG_JOB_SKIP_UNSUPPORTED,
	  .position = NAMES_AFTER_INFO, .names = 4, .done_entries = 1, .done_skipped = 2,
	  .message = "worker extraction counts differ from its names" },
	{ .name = "extract refuses more completed entries", .kind = FSIMG_JOB_EXTRACT,
	  .flags = FSIMG_JOB_NAMES | FSIMG_JOB_SKIP_UNSUPPORTED,
	  .position = NAMES_AFTER_INFO, .names = 4, .done_entries = 3, .done_skipped = 2,
	  .message = "worker extraction counts differ from its names" },
	{ .name = "extract refuses fewer skipped entries", .kind = FSIMG_JOB_EXTRACT,
	  .flags = FSIMG_JOB_NAMES | FSIMG_JOB_SKIP_UNSUPPORTED,
	  .position = NAMES_AFTER_INFO, .names = 4, .done_entries = 2, .done_skipped = 1,
	  .message = "worker extraction counts differ from its names" },
	{ .name = "extract refuses more skipped entries", .kind = FSIMG_JOB_EXTRACT,
	  .flags = FSIMG_JOB_NAMES | FSIMG_JOB_SKIP_UNSUPPORTED,
	  .position = NAMES_AFTER_INFO, .names = 4, .done_entries = 2, .done_skipped = 3,
	  .message = "worker extraction counts differ from its names" },
	/* The total is correct, but the split by kind is forged. */
	{ .name = "extract refuses swapped completed and skipped counts", .kind = FSIMG_JOB_EXTRACT,
	  .flags = FSIMG_JOB_NAMES | FSIMG_JOB_SKIP_UNSUPPORTED,
	  .position = NAMES_AFTER_INFO, .names = 4, .done_entries = 3, .done_skipped = 1,
	  .message = "worker extraction counts differ from its names" },
	{ .name = "create refuses name chunk without entry", .kind = FSIMG_JOB_CREATE,
	  .flags = FSIMG_JOB_NAMES, .position = NAMES_CHUNK_WITHOUT_ENTRY,
	  .message = "invalid worker name chunk" },
	{ .name = "extract refuses entry before info", .kind = FSIMG_JOB_EXTRACT,
	  .flags = FSIMG_JOB_NAMES, .names = 1, .message = "invalid worker entry start" },
	{ .name = "create accepts names before plan", .kind = FSIMG_JOB_CREATE,
	  .flags = FSIMG_JOB_NAMES, .names = 2, .plan_entries = 2, .done_entries = 2 },
	{ .name = "encoded create accepts names before image", .kind = FSIMG_JOB_CREATE,
	  .flags = FSIMG_JOB_NAMES, .names = 2, .plan_entries = 2, .done_entries = 2,
	  .encoded = true },
	{ .name = "plan accepts names before plan", .kind = FSIMG_JOB_PLAN,
	  .flags = FSIMG_JOB_NAMES, .names = 2, .plan_entries = 2, .done_entries = 2 },
	{ .name = "extract accepts completed and skipped names", .kind = FSIMG_JOB_EXTRACT,
	  .flags = FSIMG_JOB_NAMES | FSIMG_JOB_SKIP_UNSUPPORTED,
	  .position = NAMES_AFTER_INFO, .names = 4, .done_entries = 2, .done_skipped = 2 }
};

typedef enum coordinator_scenario {
	COORDINATOR_EXIT_BLOCKED_INPUT = 1000,
	COORDINATOR_CLOCK_FIRST_FAILURE,
	COORDINATOR_CLOCK_LATER_FAILURE,
	COORDINATOR_CLOCK_STOP_FAILURE,
	COORDINATOR_SPOOLED_DISK_OVER_LIMIT,
	COORDINATOR_SPOOLED_DISK_AT_LIMIT,
	COORDINATOR_ENCODED_PLAN_BELOW_DISK,
	COORDINATOR_ENCODED_PLAN_AT_DISK,
	COORDINATOR_ENCODED_PLAN_ABOVE_DISK,
	COORDINATOR_COMPRESSION_MISMATCH,
	COORDINATOR_STAGED_RAW_SHORT,
	COORDINATOR_STAGED_RAW_LONG,
	COORDINATOR_STAGED_RAW_EXACT,
	COORDINATOR_STAGED_ENCODED_OVER_PLAN,
	COORDINATOR_STAGED_ENCODED_ZERO,
	COORDINATOR_STAGED_ENCODED_AT_PLAN,
	COORDINATOR_STAGED_ENCODED_BELOW_PLAN,
	COORDINATOR_STREAM_OVER_PLAN,
	COORDINATOR_STREAM_SHORT,
	COORDINATOR_STREAM_EXACT,
	COORDINATOR_READ_FORBIDDEN_CONTAINER,
	COORDINATOR_READ_FORBIDDEN_SCHEME,
	COORDINATOR_READ_MISSING_CONTAINER,
	COORDINATOR_READ_MISSING_COMPRESSION,
	COORDINATOR_READ_BARE_STACK,
	COORDINATOR_READ_VHD_STACK,
	COORDINATOR_SUCCESS_BLOCKED_INPUT
} coordinator_scenario_t;

typedef enum coordinator_job {
	COORDINATOR_READ_FILE,
	COORDINATOR_READ_STREAM,
	COORDINATOR_CREATE_STAGING,
	COORDINATOR_CREATE_STREAM
} coordinator_job_t;

typedef enum coordinator_clock_fault {
	COORDINATOR_CLOCK_NONE,
	COORDINATOR_CLOCK_FROM_FIRST,
	COORDINATOR_CLOCK_FROM_SECOND,
	COORDINATOR_CLOCK_AT_READY
} coordinator_clock_fault_t;

/* Scenario names identify tests; only a table index crosses the exec boundary. */
typedef struct coordinator_case {
	coordinator_scenario_t scenario;
	const char	*name;
	coordinator_job_t job;
	bool		 stall_ignoring_sigterm;
	bool		 exit_before_done;
	coordinator_clock_fault_t clock_fault;
	uint64_t	 spool_limit;
	const char	*requested_stack;
	const char	*requested_compression;
	bool		 send_image;
	uint64_t	 image_disk_bytes;
	uint64_t	 image_file_bytes;
	const char	*image_scheme;
	const char	*image_container;
	const char	*image_compression;
	uint64_t	 plan_volume_bytes;
	uint64_t	 plan_output_bytes;
	uint64_t	 done_image_bytes;
	uint64_t	 raw_bytes;
	fsimg_status_t	 status;
	const char	*message;
	bool		 message_contains;
	uint64_t	 minimum_ms;
	uint64_t	 maximum_ms;
	uint64_t	 deadline_ms;
} coordinator_case_t;

/* Parent setup and the re-executed worker share each complete case. */
static const coordinator_case_t coordinator_cases[] = {
	{
		.scenario = COORDINATOR_EXIT_BLOCKED_INPUT,
		.name = "worker exit with blocked input and output",
		.job = COORDINATOR_READ_STREAM,
		.exit_before_done = true,
		.minimum_ms = 2000,
		.status = FSIMG_EIO,
		.message = "worker ended before completion",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_CLOCK_FIRST_FAILURE,
		.name = "monotonic clock fails from first query",
		.job = COORDINATOR_READ_FILE,
		.stall_ignoring_sigterm = true,
		.clock_fault = COORDINATOR_CLOCK_FROM_FIRST,
		.message_contains = true,
		.maximum_ms = 2500,
		.deadline_ms = 8000,
		.status = FSIMG_EIO,
		.message = "monotonic clock",
	},
	{
		.scenario = COORDINATOR_CLOCK_LATER_FAILURE,
		.name = "monotonic clock fails after first query",
		.job = COORDINATOR_READ_FILE,
		.stall_ignoring_sigterm = true,
		.clock_fault = COORDINATOR_CLOCK_FROM_SECOND,
		.maximum_ms = 1000,
		.deadline_ms = 8000,
		.status = FSIMG_ECANCELLED,
		.message = "worker cancelled or deadline exceeded",
	},
	{
		.scenario = COORDINATOR_CLOCK_STOP_FAILURE,
		.name = "monotonic clock fails with SIGTERM-resistant worker",
		.job = COORDINATOR_READ_FILE,
		.stall_ignoring_sigterm = true,
		.clock_fault = COORDINATOR_CLOCK_AT_READY,
		.maximum_ms = 1000,
		.status = FSIMG_ECANCELLED,
		.message = "worker cancelled or deadline exceeded",
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_SPOOLED_DISK_OVER_LIMIT,
		.name = "hbcifs disk exceeds spool limit",
		.job = COORDINATOR_READ_FILE,
		.spool_limit = 4096,
		.send_image = true,
		.image_disk_bytes = 4097,
		.image_file_bytes = 1024,
		.image_container = "hbcifs",
		.status = FSIMG_EIO,
		.message = "invalid worker image information",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_SPOOLED_DISK_AT_LIMIT,
		.name = "hbcifs disk equals spool limit",
		.job = COORDINATOR_READ_FILE,
		.spool_limit = 4096,
		.send_image = true,
		.image_disk_bytes = 4096,
		.image_file_bytes = 1024,
		.image_container = "hbcifs",
		.status = FSIMG_OK,
		.message = "",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_ENCODED_PLAN_BELOW_DISK,
		.name = "encoded plan below disk",
		.job = COORDINATOR_CREATE_STAGING,
		.requested_compression = "gzip",
		.send_image = true,
		.image_disk_bytes = 4096,
		.image_file_bytes = 4096,
		.image_compression = "gzip",
		.plan_volume_bytes = 512,
		.plan_output_bytes = 4095,
		.done_image_bytes = 4096,
		.status = FSIMG_EIO,
		.message = "invalid worker image plan",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_ENCODED_PLAN_AT_DISK,
		.name = "encoded plan equals disk",
		.job = COORDINATOR_CREATE_STAGING,
		.requested_compression = "gzip",
		.send_image = true,
		.image_disk_bytes = 4096,
		.image_file_bytes = 4096,
		.image_compression = "gzip",
		.plan_volume_bytes = 512,
		.plan_output_bytes = 4096,
		.done_image_bytes = 4096,
		.status = FSIMG_EIO,
		.message = "invalid worker image plan",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_ENCODED_PLAN_ABOVE_DISK,
		.name = "encoded plan exceeds disk by one",
		.job = COORDINATOR_CREATE_STAGING,
		.requested_compression = "gzip",
		.send_image = true,
		.image_disk_bytes = 4096,
		.image_file_bytes = 4096,
		.image_compression = "gzip",
		.plan_volume_bytes = 512,
		.plan_output_bytes = 4097,
		.done_image_bytes = 4096,
		.status = FSIMG_OK,
		.message = "",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_COMPRESSION_MISMATCH,
		.name = "created image omits requested gzip",
		.job = COORDINATOR_CREATE_STAGING,
		.requested_compression = "gzip",
		.send_image = true,
		.image_disk_bytes = 4096,
		.image_file_bytes = 4096,
		.image_compression = "",
		.plan_volume_bytes = 512,
		.plan_output_bytes = 4097,
		.done_image_bytes = 4096,
		.status = FSIMG_EIO,
		.message = "invalid worker image information",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_STAGED_RAW_SHORT,
		.name = "staged raw length below plan",
		.job = COORDINATOR_CREATE_STAGING,
		.plan_volume_bytes = 4096,
		.plan_output_bytes = 4096,
		.done_image_bytes = 4095,
		.status = FSIMG_EIO,
		.message = "worker raw completion length differs",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_STAGED_RAW_LONG,
		.name = "staged raw length above plan",
		.job = COORDINATOR_CREATE_STAGING,
		.plan_volume_bytes = 4096,
		.plan_output_bytes = 4096,
		.done_image_bytes = 4097,
		.status = FSIMG_EIO,
		.message = "worker raw completion length differs",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_STAGED_RAW_EXACT,
		.name = "staged raw length equals plan",
		.job = COORDINATOR_CREATE_STAGING,
		.plan_volume_bytes = 4096,
		.plan_output_bytes = 4096,
		.done_image_bytes = 4096,
		.status = FSIMG_OK,
		.message = "",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_STAGED_ENCODED_OVER_PLAN,
		.name = "staged encoded length above plan",
		.job = COORDINATOR_CREATE_STAGING,
		.requested_compression = "gzip",
		.send_image = true,
		.image_disk_bytes = 4096,
		.image_file_bytes = 4096,
		.image_compression = "gzip",
		.plan_volume_bytes = 512,
		.plan_output_bytes = 4097,
		.done_image_bytes = 4098,
		.status = FSIMG_EIO,
		.message = "worker raw completion length differs",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_STAGED_ENCODED_ZERO,
		.name = "staged encoded length is zero",
		.job = COORDINATOR_CREATE_STAGING,
		.requested_compression = "gzip",
		.send_image = true,
		.image_disk_bytes = 4096,
		.image_file_bytes = 4096,
		.image_compression = "gzip",
		.plan_volume_bytes = 512,
		.plan_output_bytes = 4097,
		.done_image_bytes = 0,
		.status = FSIMG_EIO,
		.message = "worker raw completion length differs",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_STAGED_ENCODED_AT_PLAN,
		.name = "staged encoded length equals plan",
		.job = COORDINATOR_CREATE_STAGING,
		.requested_compression = "gzip",
		.send_image = true,
		.image_disk_bytes = 4096,
		.image_file_bytes = 4096,
		.image_compression = "gzip",
		.plan_volume_bytes = 512,
		.plan_output_bytes = 4097,
		.done_image_bytes = 4097,
		.status = FSIMG_OK,
		.message = "",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_STAGED_ENCODED_BELOW_PLAN,
		.name = "staged encoded length below plan",
		.job = COORDINATOR_CREATE_STAGING,
		.requested_compression = "gzip",
		.send_image = true,
		.image_disk_bytes = 4096,
		.image_file_bytes = 4096,
		.image_compression = "gzip",
		.plan_volume_bytes = 512,
		.plan_output_bytes = 4097,
		.done_image_bytes = 4096,
		.status = FSIMG_OK,
		.message = "",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_STREAM_OVER_PLAN,
		.name = "created stream exceeds plan",
		.job = COORDINATOR_CREATE_STREAM,
		.plan_volume_bytes = 4096,
		.plan_output_bytes = 4096,
		.raw_bytes = 4097,
		.status = FSIMG_EIO,
		.message = "worker image stream exceeds its plan",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_STREAM_SHORT,
		.name = "created raw stream falls short of plan",
		.job = COORDINATOR_CREATE_STREAM,
		.plan_volume_bytes = 4096,
		.plan_output_bytes = 4096,
		.raw_bytes = 4095,
		.status = FSIMG_EIO,
		.message = "worker or raw output was incomplete",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_STREAM_EXACT,
		.name = "created raw stream equals plan",
		.job = COORDINATOR_CREATE_STREAM,
		.plan_volume_bytes = 4096,
		.plan_output_bytes = 4096,
		.raw_bytes = 4096,
		.status = FSIMG_OK,
		.message = "",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_READ_FORBIDDEN_CONTAINER,
		.name = "read image names a forced-empty container",
		.job = COORDINATOR_READ_FILE,
		.requested_stack = "fat12/-/-",
		.send_image = true,
		.image_disk_bytes = 1048576,
		.image_file_bytes = 1024,
		.image_container = "dmg",
		.status = FSIMG_EIO,
		.message = "invalid worker image information",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_READ_FORBIDDEN_SCHEME,
		.name = "read image names a forced-empty scheme",
		.job = COORDINATOR_READ_FILE,
		.requested_stack = "fat12/-/-",
		.send_image = true,
		.image_disk_bytes = 1024,
		.image_file_bytes = 1024,
		.image_scheme = "mbr",
		.status = FSIMG_EIO,
		.message = "invalid worker image information",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_READ_MISSING_CONTAINER,
		.name = "read image omits forced vhd container",
		.job = COORDINATOR_READ_FILE,
		.requested_stack = "fat12/-/vhd",
		.send_image = true,
		.image_disk_bytes = 1024,
		.image_file_bytes = 1024,
		.status = FSIMG_EIO,
		.message = "invalid worker image information",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_READ_MISSING_COMPRESSION,
		.name = "read image omits requested gzip",
		.job = COORDINATOR_READ_FILE,
		.requested_compression = "gzip",
		.send_image = true,
		.image_disk_bytes = 1024,
		.image_file_bytes = 1024,
		.status = FSIMG_EIO,
		.message = "invalid worker image information",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_READ_BARE_STACK,
		.name = "read image honours forced-empty scheme and container",
		.job = COORDINATOR_READ_FILE,
		.requested_stack = "fat12/-/-",
		.send_image = true,
		.image_disk_bytes = 1024,
		.image_file_bytes = 1024,
		.status = FSIMG_OK,
		.message = "",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_READ_VHD_STACK,
		.name = "read image honours forced vhd container and footer",
		.job = COORDINATOR_READ_FILE,
		.requested_stack = "fat12/-/vhd",
		.send_image = true,
		.image_disk_bytes = 512,
		.image_file_bytes = 1024,
		.image_container = "vhd",
		.status = FSIMG_OK,
		.message = "",
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	},
	{
		.scenario = COORDINATOR_SUCCESS_BLOCKED_INPUT,
		.name = "worker success with blocked input relay",
		.job = COORDINATOR_READ_STREAM,
		.send_image = true,
		.image_disk_bytes = 1024,
		.image_file_bytes = 1024,
		.status = FSIMG_EIO,
		.message = "standard input could not be relayed",
		.minimum_ms = 2000,
		.maximum_ms = 8000,
		.deadline_ms = 8000,
	}
};

static unsigned coordinator_clock_calls;
static unsigned coordinator_clock_fail_from;
static uint64_t coordinator_clock_fault_started;

/* Create jobs in these tests copy one source directory to the image root. */
static const char *const image_root[] = { "/" };

#ifdef FSIMG_TEST_ASAN
/* A volatile release target keeps the deliberate runtime failure observable
 * under optimization. This is used only to test fatal-error classification.
 */
typedef void (*test_release_fn_t)(void *);
static test_release_fn_t volatile release_test_memory = free;
#endif

typedef struct worker_fixture {
	fsimg_ctx_t	*ctx;
	fsimg_worker_request_t request;
	fsimg_worker_result_t result;
	char		 path[64];
	char		 executable[4096];
	uint64_t	 name_bytes;
	uint64_t	 plan_losses;
	unsigned	 info;
	unsigned	 plans;
	unsigned	 images;
	unsigned	 partitions;
	bool		 ready;
	bool		 refuse_ready;
	bool		 dirty_event;
	bool		 print_info;
	int		 parent;
	int		 source;
	int		 output;
	int		 scratch;
	int		 input;
} worker_fixture_t;

/* The event borrows the fixture and the pipe that releases its consumer. */
typedef struct coordinator_event_context {
	worker_fixture_t *fixture;
	uint64_t	 ready_started;
	int		 drain_start;
} coordinator_event_context_t;

typedef struct listed_paths {
	worker_fixture_t *fixture;
	const char *const *expected;
	size_t count;
	size_t seen;
	size_t used;
	char path[128];
} listed_paths_t;

typedef struct output_source {
	unsigned	 emitted;
	unsigned	 records;
	bool		 fail;
	unsigned char	 buffer[4097];
} output_source_t;

typedef struct path_sequence_result {
	fsimg_frame_t	 frames[4];
	size_t		 count;
	fsimg_status_t	 status;
	int		 outcome;
	bool		 exited;
	bool		 eof;
} path_sequence_result_t;

typedef struct path_sequence_case {
	const char	*name;
	const char	*declared_selection;
	const char	*message;
	const char	*sent;
	const char	*following_selection;
	uint32_t	 sent_length;
	uint32_t	 following_length;
	uint32_t	 ack_length;
	fsimg_frame_type_t sent_type;
	fsimg_status_t	 expected_status;
	bool		 two_root_plan;
	bool		 expects_ready;
} path_sequence_case_t;

static void fixture_open(worker_fixture_t *);
static void fixture_open_format(worker_fixture_t *, const char *);
static void fixture_open_with_entries(worker_fixture_t *, const char *,
    const fsimg_entry_t *, size_t);
static void fixture_open_directories(worker_fixture_t *);
static void fixture_close(worker_fixture_t *);
static fsimg_status_t check_listed_path(void *, const fsimg_frame_t *,
    fsimg_worker_text_t *, fsimg_error_t *);
static void check_directory_listing(worker_fixture_t *, const char *const *,
    size_t);
static fsimg_status_t event(void *, const fsimg_frame_t *, fsimg_worker_text_t *,
    fsimg_error_t *);
static fsimg_status_t run(worker_fixture_t *, fsimg_error_t *);
static void test_operations(const char *);
static void test_prompt_completion(void);
static void test_startup(void);
static void test_inheritance(void);
static void test_forged(void);
static void test_verbose_names_protocol(void);
static bool names_worker_selected(struct stat *);
static int fake_names_worker(size_t);
static bool send_forged_names(const names_case_t *);
static void test_path_sequences(void);
static void run_path_sequence(const worker_fixture_t *, const unsigned char *,
    size_t, path_sequence_result_t *);
static void test_forged_publication(void);
static void test_text_output(void);
static void test_list_case_alias_members_once(void);
static void test_list_missing_member_names_error(void);
static void test_list_directory_member_precedes_subtree(void);
static void test_list_nested_directory_members_once(void);
static void test_list_directory_case_alias_members_once(void);
static void test_list_nested_directory_members_without_recursion_once(void);
static void test_list_reversed_nested_directory_members_keep_order(void);
static void test_list_directory_without_recursion_includes_target_and_children(void);
static void test_list_deep_directory_includes_ancestor(void);
static void test_list_root_and_no_members_have_no_root_entry(void);
static void test_list_and_extract_directory_counts_and_manifest_match(void);
static void test_incomplete_extraction(void);
static void test_skipped_extraction(void);
static void test_omitted_extraction(void);
static void test_mount_source(const char *);
static void test_multiple_roots(worker_fixture_t *);
static void test_manifest_roots(worker_fixture_t *);
static void test_manifest_special_files(worker_fixture_t *);
static int open_manifest(const worker_fixture_t *, const char *,
    const char *);
static void test_partitioned_create(void);
static int raw_intermediate(const worker_fixture_t *);
static void create_request(worker_fixture_t *, const char *);
static void test_container_create(void);
static void test_compressed_create(void);
static void test_transport(worker_scenario_t);
static int fake_worker(worker_scenario_t);
static int fake_output(void);
static fsimg_status_t produce_output(void *, fsimg_worker_text_t *, fsimg_error_t *);
static void test_output_ledger(void);
static uint64_t fill_pipe(int);
static void drain_after_delay(int, uint64_t);
static bool coordinator_worker_selected(struct stat *);
static int fake_coordinator_worker(size_t);
static int coordinator_marker(worker_fixture_t *, size_t, int);
static fsimg_status_t coordinator_event(void *, const fsimg_frame_t *,
    fsimg_worker_text_t *, fsimg_error_t *);
static fsimg_status_t coordinator_clock_event(void *, const fsimg_frame_t *,
    fsimg_worker_text_t *, fsimg_error_t *);
static uint64_t coordinator_real_milliseconds(void);
static void coordinator_drain(int, unsigned);
static void coordinator_run_case(worker_fixture_t *, const coordinator_case_t *,
    int, int[2]);
static unsigned coordinator_test_case(const coordinator_case_t *);
static unsigned coordinator_test_scenario(coordinator_scenario_t);
static unsigned coordinator_test_read_stack(void);
static unsigned test_worker_exit_with_blocked_input(void);
static unsigned test_monotonic_clock_failures(void);
static unsigned test_spooled_container_disk_limit(void);
static unsigned test_forged_creation_plan(void);
static unsigned test_forged_creation_compression(void);
static unsigned test_forged_creation_staged_length(void);
static unsigned test_forged_creation_stream_length(void);
static void test_coordinator_regressions(void);

/* Run the real worker with a preloaded job pipe, bypassing the coordinator's
 * path framing. The cancellation writer stays open until the child is reaped.
 */
static void
run_path_sequence(const worker_fixture_t *fixture, const unsigned char *bytes,
    size_t length, path_sequence_result_t *result)
{
	fsimg_frame_decoder_t decoder = { 0 };
	struct pollfd channel;
	unsigned char buffer[4096];
	size_t consumed, offset;
	ssize_t count;
	pid_t child, waited;
	int descriptors[FSIMG_CHILD_ROOTS + 2];
	int job[2], control[2], cancel[2], null_fd;

	bzero(result, sizeof(*result));
	CHECK(pipe(job) == 0);
	CHECK(pipe(control) == 0);
	CHECK(pipe(cancel) == 0);
	/* A nonblocking preload cannot hang even if the worker never reads. */
	CHECK(fcntl(job[1], F_SETFL, O_NONBLOCK) == 0);
	CHECK(write(job[1], bytes, length) == (ssize_t)length);
	CHECK(close(job[1]) == 0);
	CHECK(fcntl(control[0], F_SETFL, O_NONBLOCK) == 0);
	CHECK(fcntl(cancel[0], F_SETFL, O_NONBLOCK) == 0);
	null_fd = open("/dev/null", O_RDWR);
	CHECK(null_fd != -1);
	for (size_t i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]); i++)
		descriptors[i] = null_fd;
	descriptors[FSIMG_CHILD_JOB] = job[0];
	descriptors[FSIMG_CHILD_CONTROL] = control[1];
	descriptors[FSIMG_CHILD_CANCEL] = cancel[0];
	for (fsimg_capability_t c = 0; c < FSIMG_CAPABILITY_COUNT; c++) {
		int fd;

		fd = fsimg_job_capability_fd(&fixture->request, c);
		if (fd != -1)
			descriptors[FSIMG_CHILD_SLOT(c)] = fd;
	}
	CHECK(fixture->request.root_count <= 2);
	for (uint32_t i = 0; i < fixture->request.root_count; i++)
		descriptors[FSIMG_CHILD_ROOTS + i] = fixture->request.root_fds[i];
	CHECK(fsimg_host_spawn_worker(fixture->executable, descriptors,
	    FSIMG_CHILD_ROOTS + fixture->request.root_count, &child) == 0);
	CHECK(close(null_fd) == 0);
	CHECK(close(job[0]) == 0);
	CHECK(close(control[1]) == 0);
	CHECK(close(cancel[0]) == 0);

	channel = (struct pollfd){ .fd = control[0], .events = POLLIN };
	/* Both partial frames and a child that closes control but stays alive
	 * consume the same finite polling budget (eight seconds).
	 */
	for (unsigned attempt = 0; attempt < 400; attempt++) {
		if (!result->exited) {
			waited = waitpid(child, &result->outcome, WNOHANG);
			if (waited == child)
				result->exited = true;
			else if (waited == -1 && errno != EINTR) {
				result->status = FSIMG_EIO;
				break;
			}
		}
		if (result->eof && result->exited)
			break;
		if (poll(&channel, 1, 20) == -1) {
			if (errno == EINTR)
				continue;
			result->status = FSIMG_EIO;
			break;
		}
		if (channel.revents == 0)
			continue;
		count = read(control[0], buffer, sizeof(buffer));
		if (count == -1) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			result->status = FSIMG_EIO;
			break;
		}
		if (count == 0) {
			result->eof = true;
			result->status = fsimg_frame_finish(&decoder, NULL);
			channel.fd = -1;
			continue;
		}
		offset = 0;
		while (offset < (size_t)count) {
			result->status = fsimg_frame_feed(&decoder, buffer + offset,
			    (size_t)count - offset, &consumed, NULL);
			if (result->status != FSIMG_OK)
				break;
			offset += consumed;
			if (!decoder.complete)
				continue;
			if (result->count == sizeof(result->frames) /
			    sizeof(result->frames[0])) {
				result->status = FSIMG_EIO;
				break;
			}
			result->frames[result->count++] = decoder.frame;
			fsimg_frame_advance(&decoder);
		}
		if (result->status != FSIMG_OK)
			break;
	}
	if (!result->exited) {
		CHECK(kill(child, SIGKILL) == 0);
		do {
			waited = waitpid(child, &result->outcome, 0);
		} while (waited == -1 && errno == EINTR);
		CHECK(waited == child);
	}
	CHECK(close(control[0]) == 0);
	CHECK(close(cancel[1]) == 0);
}

/* The worker accepts only exactly the declared byte length in nonempty
 * SELECTION frames containing exactly the job's path count of terminated paths,
 * followed by the acknowledgement. A framing fault during setup fails before
 * READY; a content fault fails after the acknowledgement and before any parsing.
 */
static void
test_path_sequences(void)
{
	static const path_sequence_case_t cases[] = {
		/* A declared two-byte path followed by ACK completes an INFO job. */
		{ .name = "valid selection", .declared_selection = "/",
		    .sent = "/\0", .sent_length = sizeof("/\0") - 1,
		    .sent_type = FSIMG_FRAME_SELECTION,
		    .expected_status = FSIMG_OK, .expects_ready = true },
		/* The path and its terminator may arrive in separate frames. */
		{ .name = "fragmented valid selection", .declared_selection = "/",
		    .sent = "/", .sent_length = sizeof("/") - 1,
		    .sent_type = FSIMG_FRAME_SELECTION,
		    .following_selection = "\0",
		    .following_length = sizeof("\0") - 1,
		    .expected_status = FSIMG_OK, .expects_ready = true },
		{ .name = "acknowledgement payload", .declared_selection = "/",
		    .sent = "/\0", .sent_length = sizeof("/\0") - 1,
		    .sent_type = FSIMG_FRAME_SELECTION, .ack_length = 1,
		    .message = "worker did not receive parsing acknowledgement",
		    .expected_status = FSIMG_EIO, .expects_ready = true },
		/* Reject the excess path before validating its relative name. */
		{ .name = "too many paths", .declared_selection = "/aaa",
		    .sent = "/a\0b\0", .sent_length = sizeof("/a\0b\0") - 1,
		    .sent_type = FSIMG_FRAME_SELECTION,
		    .message = "invalid worker selection framing",
		    .expected_status = FSIMG_EINVAL, .expects_ready = true },
		/* Five declared bytes pass the decoder's minimum for two roots. */
		{ .name = "too few paths", .sent = "/abc\0",
		    .sent_length = sizeof("/abc\0") - 1,
		    .sent_type = FSIMG_FRAME_SELECTION,
		    .message = "invalid worker selection framing",
		    .expected_status = FSIMG_EINVAL,
		    .two_root_plan = true, .expects_ready = true },
		/* Both declared bytes arrive, but neither terminates the path. */
		{ .name = "missing terminator", .declared_selection = "/",
		    .sent = "/a", .sent_length = sizeof("/a") - 1,
		    .sent_type = FSIMG_FRAME_SELECTION,
		    .message = "invalid worker selection framing",
		    .expected_status = FSIMG_EINVAL, .expects_ready = true },
		/* After three declared bytes, an empty path frame occupies ACK's
		 * position; being empty, only its type tells it from an ACK.
		 */
		{ .name = "extra selection frame", .declared_selection = "/a",
		    .sent = "/a\0", .sent_length = sizeof("/a\0") - 1,
		    .sent_type = FSIMG_FRAME_SELECTION,
		    .following_selection = "",
		    .following_length = sizeof("") - 1,
		    .message = "worker did not receive parsing acknowledgement",
		    .expected_status = FSIMG_EIO, .expects_ready = true },
		/* The empty frame precedes all three valid declared bytes. */
		{ .name = "empty selection frame", .declared_selection = "/a",
		    .sent = "", .sent_length = sizeof("") - 1,
		    .sent_type = FSIMG_FRAME_SELECTION,
		    .following_selection = "/a\0",
		    .following_length = sizeof("/a\0") - 1,
		    .message = "invalid worker selection framing",
		    .expected_status = FSIMG_EINVAL },
		/* Three sent bytes exceed the declared two-byte path. */
		{ .name = "selection exceeds remaining bytes",
		    .declared_selection = "/", .sent = "/a\0",
		    .sent_length = sizeof("/a\0") - 1,
		    .sent_type = FSIMG_FRAME_SELECTION,
		    .message = "invalid worker selection framing",
		    .expected_status = FSIMG_EINVAL },
		/* Four bytes fit the five-byte total, but only three remain. */
		{ .name = "second selection exceeds remaining bytes",
		    .declared_selection = "/abc", .sent = "/a",
		    .sent_length = sizeof("/a") - 1,
		    .sent_type = FSIMG_FRAME_SELECTION,
		    .following_selection = "bc\0x",
		    .following_length = sizeof("bc\0x") - 1,
		    .message = "invalid worker selection framing",
		    .expected_status = FSIMG_EINVAL },
		/* A wrong-type frame carries all three valid declared bytes. */
		{ .name = "non-selection frame", .declared_selection = "/a",
		    .sent = "/a\0", .sent_length = sizeof("/a\0") - 1,
		    .sent_type = FSIMG_FRAME_NAME,
		    .message = "invalid worker selection framing",
		    .expected_status = FSIMG_EINVAL }
	};
	static const char *const root_paths[] = { "/", "/a" };
	worker_fixture_t fixture;
	path_sequence_result_t result;
	fsimg_error_t error;
	const fsimg_frame_t *frame;
	unsigned char bytes[FSIMG_JOB_BYTES + 4 * FSIMG_FRAME_HEADER + 16];
	size_t length;
	fsimg_status_t status;
	int roots[2];
	bool passed;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fixture_open(&fixture);
		fixture.request.sandbox = FSIMG_SANDBOX_OFF;
		fixture.request.scratch_fd = -1;
		fixture.request.members = &cases[i].declared_selection;
		fixture.request.member_count = 1;
		if (cases[i].two_root_plan) {
			roots[0] = roots[1] = fixture.source;
			fixture.request.kind = FSIMG_JOB_PLAN;
			fixture.request.input_fd = -1;
			fixture.request.input = (fsimg_input_t){ 0 };
			fixture.request.root_fds = roots;
			fixture.request.root_paths = root_paths;
			fixture.request.root_count = 2;
		}
		/* Valid placeholder paths declare the desired byte length; the
		 * independently framed bytes below replace them on the wire.
		 * This keeps private job-record offsets out of the test.
		 */
		CHECK(fsimg_job_encode(&fixture.request, bytes + FSIMG_FRAME_HEADER,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_frame_header(bytes, FSIMG_FRAME_JOB, FSIMG_JOB_BYTES,
		    NULL) == FSIMG_OK);
		length = FSIMG_FRAME_HEADER + FSIMG_JOB_BYTES;
		CHECK(FSIMG_FRAME_HEADER <= sizeof(bytes) - length);
		CHECK(fsimg_frame_header(bytes + length, cases[i].sent_type,
		    cases[i].sent_length, NULL) == FSIMG_OK);
		length += FSIMG_FRAME_HEADER;
		CHECK(cases[i].sent_length <= sizeof(bytes) - length);
		bcopy(cases[i].sent, bytes + length, cases[i].sent_length);
		length += cases[i].sent_length;
		if (cases[i].following_selection != NULL) {
			CHECK(FSIMG_FRAME_HEADER <= sizeof(bytes) - length);
			CHECK(fsimg_frame_header(bytes + length, FSIMG_FRAME_SELECTION,
			    cases[i].following_length, NULL) == FSIMG_OK);
			length += FSIMG_FRAME_HEADER;
			CHECK(cases[i].following_length <= sizeof(bytes) - length);
			bcopy(cases[i].following_selection, bytes + length,
			    cases[i].following_length);
			length += cases[i].following_length;
		}
		CHECK(FSIMG_FRAME_HEADER <= sizeof(bytes) - length);
		CHECK(fsimg_frame_header(bytes + length, FSIMG_FRAME_ACK,
		    cases[i].ack_length, NULL) == FSIMG_OK);
		length += FSIMG_FRAME_HEADER;
		CHECK(cases[i].ack_length <= sizeof(bytes) - length);
		bzero(bytes + length, cases[i].ack_length);
		length += cases[i].ack_length;

		run_path_sequence(&fixture, bytes, length, &result);
		fixture_close(&fixture);

		fsimg_error_clear(&error);
		frame = result.count == 0 ? NULL : &result.frames[result.count - 1];
		passed = result.status == FSIMG_OK && result.eof && result.exited &&
		    WIFEXITED(result.outcome) && WEXITSTATUS(result.outcome) ==
		    (cases[i].expected_status == FSIMG_OK ? 0 : 1);
		if (cases[i].expects_ready) {
			passed = passed && result.count != 0 &&
			    result.frames[0].type == FSIMG_FRAME_READY &&
			    result.frames[0].length == 8 &&
			    fsimg_load_le64(result.frames[0].payload) ==
			    FSIMG_CONFINEMENT_DISABLED;
		}
		if (cases[i].expected_status == FSIMG_OK) {
			/* The raw INFO job emits IMAGE and INFO between READY and DONE. */
			passed = passed && result.count == 4 &&
			    result.frames[1].type == FSIMG_FRAME_IMAGE &&
			    result.frames[2].type == FSIMG_FRAME_INFO &&
			    frame->type == FSIMG_FRAME_DONE &&
			    frame->length == FSIMG_WIRE_DONE_BYTES;
		} else {
			/* Exact frame order through EOF excludes all job output. */
			passed = passed &&
			    result.count == (cases[i].expects_ready ? 2u : 1u) &&
			    frame != NULL && frame->type == FSIMG_FRAME_ERROR &&
			    frame->length == FSIMG_WIRE_ERROR_BYTES;
			if (frame != NULL && frame->type == FSIMG_FRAME_ERROR &&
			    frame->length == FSIMG_WIRE_ERROR_BYTES) {
				status = fsimg_wire_error_decode(frame->payload,
				    frame->length, &error);
				passed = passed && status == cases[i].expected_status &&
				    error.status == cases[i].expected_status &&
				    strcmp(error.message, cases[i].message) == 0;
			}
		}
		if (!passed) {
			fprintf(stderr, "%s: frames=", cases[i].name);
			for (size_t j = 0; j < result.count; j++)
				fprintf(stderr, "%s%u", j == 0 ? "" : ",",
				    (unsigned)result.frames[j].type);
			fprintf(stderr, "; decoder status=%d, eof=%d, exited=%d, "
			    "outcome=%d; worker status=%d, message=%s\n",
			    result.status, result.eof, result.exited, result.outcome,
			    error.status, error.message);
		}
		CHECK(passed);
	}
}

static fsimg_status_t
produce_output(void *argument, fsimg_worker_text_t *text, fsimg_error_t *error)
{
	output_source_t *source = argument;

	if (source->emitted == source->records) {
		if (source->fail)
			return (fsimg_error_set(error, FSIMG_EIO, 0,
			    FSIMG_OP_CONTEXT, "test output producer failed"));
		return (FSIMG_DONE);
	}
	memset(source->buffer, 'A' + source->emitted, sizeof(source->buffer));
	source->emitted++;
	*text = (fsimg_worker_text_t){ .data = source->buffer,
	    .length = sizeof(source->buffer) };
	return (FSIMG_OK);
}

static void
test_output_ledger(void)
{
	worker_fixture_t fixture;
	output_source_t source = { .records = 1 };
	fsimg_output_budget_t budget;
	fsimg_error_t error;
	struct stat st;
	int fd;

	fixture_open(&fixture);
	fd = openat(fixture.parent, "output-only", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	CHECK(fd != -1);
	bzero(&budget, sizeof(budget));
	CHECK(fsimg_output_run(fixture.executable, produce_output, &source,
	    fd, 0, NULL, &budget, &error) == FSIMG_OK);
	CHECK(budget.text_bytes == sizeof(source.buffer));
	CHECK(budget.confirmed[0] == sizeof(source.buffer));
	CHECK(budget.stdout_possible);
	/* A new reporting phase fails before writing anything itself. The
	 * already confirmed prefix still makes the complete stream partial.
	 */
	source = (output_source_t){ .fail = true };
	CHECK(fsimg_output_run(fixture.executable, produce_output, &source,
	    fd, 0, NULL, &budget, &error) == FSIMG_EIO);
	CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
	CHECK(fstat(fd, &st) == 0 && st.st_size == (off_t)sizeof(source.buffer));
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "output-only", 0) == 0);
	fixture_close(&fixture);
}


/*
 * A confined plan of path, whose descendant path/alias a privileged caller
 * has bind- or nullfs-mounted from path/../lower holding the 8-byte file
 * payload (docs/build.md), scans across the mount as tar does: a mounted
 * descendant is authorized content. alias showing lower's inode proves the
 * mount is there, and the plan's one file of 8 bytes that it was crossed.
 */
static void
test_mount_source(const char *path)
{
	worker_fixture_t fixture;
	fsimg_host_file_t lower_identity, alias_identity;
	fsimg_error_t error;
	int source, alias, lower;

	fixture_open(&fixture);
	source = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(source != -1);
	alias = openat(source, "alias", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(alias != -1);
	lower = openat(source, "../lower", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(lower != -1);
	CHECK(fsimg_host_file(alias, &alias_identity) == 0);
	CHECK(fsimg_host_file(lower, &lower_identity) == 0);
	CHECK(alias_identity.inode == lower_identity.inode);
	CHECK(close(alias) == 0);
	CHECK(close(lower) == 0);
	fixture.request.input_fd = -1;
	fixture.request.input = (fsimg_input_t){ 0 };
	fixture.request.root_fds = &source;
	fixture.request.root_paths = image_root;
	fixture.request.root_count = 1;
	fixture.request.kind = FSIMG_JOB_PLAN;
	fixture.request.sandbox = FSIMG_SANDBOX_REQUIRED;
	CHECK(fsimg_stack_parse("fat12", &fixture.request.stack, NULL) == FSIMG_OK);
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.ready && fixture.result.confinement == FSIMG_CONFINEMENT_ACTIVE);
	CHECK(fixture.plans == 1);
	/* The directory alias and the file payload beneath the mount. */
	CHECK(fixture.result.entries == 2 && fixture.result.logical_bytes == 8);
	printf("mount crossed: alias inode %llu\n",
	    (unsigned long long)alias_identity.inode);
	CHECK(close(source) == 0);
	fixture_close(&fixture);
}

static uint64_t
fill_pipe(int fd)
{
	unsigned char bytes[4096] = { 0 };
	uint64_t filled;
	ssize_t count;
	int flags;

	flags = fcntl(fd, F_GETFL);
	CHECK(flags != -1);
	CHECK(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
	filled = 0;
	while ((count = write(fd, bytes, sizeof(bytes))) > 0)
		filled += (uint64_t)count;
	CHECK(count == -1 && errno == EAGAIN);
	CHECK(fcntl(fd, F_SETFL, flags) == 0);
	return (filled);
}

static void
drain_after_delay(int fd, uint64_t padding)
{
	unsigned char bytes[4096];
	size_t batch, offset;
	ssize_t count;

	poll(NULL, 0, 3000);
	while (padding != 0) {
		batch = padding > sizeof(bytes) ? sizeof(bytes) : (size_t)padding;
		count = read(fd, bytes, batch);
		CHECK(count > 0);
		for (ssize_t i = 0; i < count; i++)
			CHECK(bytes[i] == 0);
		padding -= (uint64_t)count;
	}
	offset = 0;
	while (offset < 7) {
		count = read(fd, bytes + offset, 7 - offset);
		CHECK(count > 0);
		offset += (size_t)count;
	}
	CHECK(bcmp(bytes, "payload", 7) == 0);
}

static void
fixture_open(worker_fixture_t *fixture)
{
	fixture_open_with_entries(fixture, "fat16", NULL, 0);
}

static void
fixture_open_format(worker_fixture_t *fixture, const char *format)
{
	fixture_open_with_entries(fixture, format, NULL, 0);
}

static void
fixture_open_with_entries(worker_fixture_t *fixture, const char *format,
    const fsimg_entry_t *entries, size_t count)
{
	const fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	fsimg_options_t options;
	fsimg_writer_t *writer;
	fsimg_io_t *output;
	const struct timespec source_times[2] = {
		{ .tv_sec = 315532801, .tv_nsec = 1 },
		{ .tv_sec = 315532801, .tv_nsec = 1 }
	};
	int fd;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->path, sizeof(fixture->path), "worker-fixture-XXXXXX");
	CHECK(mkdtemp(fixture->path) != NULL);
	fixture->parent = open(fixture->path, O_RDONLY | O_DIRECTORY);
	CHECK(fixture->parent != -1);
	CHECK(mkdirat(fixture->parent, "source", 0700) == 0);
	CHECK(mkdirat(fixture->parent, "output", 0700) == 0);
	CHECK(mkdirat(fixture->parent, "scratch", 0700) == 0);
	fixture->source = openat(fixture->parent, "source", O_RDONLY | O_DIRECTORY);
	fixture->output = openat(fixture->parent, "output", O_RDONLY | O_DIRECTORY);
	fixture->scratch = openat(fixture->parent, "scratch", O_RDONLY | O_DIRECTORY);
	CHECK(fixture->source != -1 && fixture->output != -1 && fixture->scratch != -1);
	fd = openat(fixture->source, "HELLO.TXT", O_CREAT | O_EXCL | O_WRONLY, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, "payload", 7) == 7);
	CHECK(futimens(fd, source_times) == 0);
	CHECK(close(fd) == 0);
	fsimg_options_init(&options);
	options.scratch_directory_fd = fixture->scratch;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_new(fixture->ctx, format, NULL, &writer, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add_tree(writer, fixture->source, "/", NULL) == FSIMG_OK);
	for (size_t i = 0; i < count; i++)
		CHECK(fsimg_writer_add(writer, &entries[i],
		    entries[i].kind == FSIMG_ENTRY_FILE ? &zero : NULL, NULL) == FSIMG_OK);
	fd = openat(fixture->parent, "image", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	CHECK(fd != -1);
	CHECK(fsimg_io_from_fd(fixture->ctx, fd, FSIMG_IO_WRITE, &output, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_emit(writer, output, NULL) == FSIMG_OK);
	CHECK(fsimg_io_close_output(output, NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	fsimg_writer_free(writer);
	fixture->input = openat(fixture->parent, "image", O_RDONLY);
	CHECK(fixture->input != -1);
	CHECK(fsimg_host_executable(fixture->executable, sizeof(fixture->executable)) == 0);
	fsimg_worker_request_init(&fixture->request);
	fixture->request.input_fd = fixture->input;
	CHECK(fsimg_input_prepare(fixture->request.input_fd, NULL, NULL,
	    &fixture->request.input, NULL) == FSIMG_OK);
	fixture->request.scratch_fd = fixture->scratch;
	fixture->request.limits.memory_bytes = UINT64_C(16) * 1024 * 1024;
	fixture->request.kind = FSIMG_JOB_INFO;
}

static void
fixture_close(worker_fixture_t *fixture)
{
	fsimg_ctx_free(fixture->ctx);
	CHECK(close(fixture->input) == 0);
	CHECK(unlinkat(fixture->parent, "image", 0) == 0);
	CHECK(unlinkat(fixture->source, "HELLO.TXT", 0) == 0);
	CHECK(close(fixture->source) == 0);
	CHECK(close(fixture->output) == 0);
	CHECK(close(fixture->scratch) == 0);
	CHECK(unlinkat(fixture->parent, "source", AT_REMOVEDIR) == 0);
	CHECK(unlinkat(fixture->parent, "output", AT_REMOVEDIR) == 0);
	CHECK(unlinkat(fixture->parent, "scratch", AT_REMOVEDIR) == 0);
	CHECK(close(fixture->parent) == 0);
	CHECK(rmdir(fixture->path) == 0);
}

static fsimg_status_t
event(void *argument, const fsimg_frame_t *frame,
	fsimg_worker_text_t *text, fsimg_error_t *error)
{
	worker_fixture_t *fixture = argument;

	if (fixture->dirty_event && error != NULL)
		error->status = FSIMG_EIO;
	if (frame->type == FSIMG_FRAME_READY) {
		fixture->ready = true;
		if (fixture->refuse_ready)
			return (FSIMG_ECANCELLED);
	} else {
		CHECK(fixture->ready);
		if (frame->type == FSIMG_FRAME_INFO)
			fixture->info++;
		if (frame->type == FSIMG_FRAME_INFO && fixture->print_info) {
			text->data = "printed information\n";
			text->length = strlen(text->data);
		}
		if (frame->type == FSIMG_FRAME_IMAGE)
			fixture->images++;
		if (frame->type == FSIMG_FRAME_PARTITION)
			fixture->partitions++;
		if (frame->type == FSIMG_FRAME_PLAN) {
			fixture->plans++;
			fixture->plan_losses = fsimg_load_le64(frame->payload + FSIMG_WIRE_PLAN_LOSSES);
		}
		if (frame->type == FSIMG_FRAME_NAME)
			fixture->name_bytes += frame->length;
	}
	return (FSIMG_OK);
}

static void
test_text_output(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	char bytes[32];
	int fd, flags, status_flags;

	fixture_open(&fixture);
	fixture.print_info = true;
	fd = openat(fixture.parent, "text", O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(fd != -1);
	flags = fcntl(fd, F_GETFL);
	CHECK(flags != -1);
	CHECK(fsimg_worker_run(fixture.executable, &fixture.request, event, &fixture,
	    fd, NULL, &fixture.result, &error) == FSIMG_OK);
	CHECK(pread(fd, bytes, sizeof(bytes), 0) == 20);
	CHECK(bcmp(bytes, "printed information\n", 20) == 0);
	/* Compare public status flags. macOS also exposes a kernel write-history
	 * bit through F_GETFL, which an ordinary sink write legitimately sets.
	 */
	status_flags = O_ACCMODE | O_APPEND | O_NONBLOCK | O_SYNC | O_DSYNC;
	CHECK((fcntl(fd, F_GETFL) & status_flags) == (flags & status_flags));
	CHECK(run(&fixture, &error) == FSIMG_EINVAL);
	CHECK(error.status == FSIMG_EINVAL);
	fixture.request.kind = FSIMG_JOB_CAT;
	fixture.request.members = (const char *const[]){ "/hello.txt" };
	fixture.request.member_count = 1;
	CHECK(fsimg_worker_run(fixture.executable, &fixture.request, event, &fixture,
	    fd, NULL, &fixture.result, &error) == FSIMG_EIO);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "text", 0) == 0);
	fixture_close(&fixture);
}

static fsimg_status_t
run(worker_fixture_t *fixture, fsimg_error_t *error)
{
	fixture->ready = false;
	return (fsimg_worker_run(fixture->executable, &fixture->request, event,
	    fixture, -1, NULL, &fixture->result, error));
}

static void
test_list_case_alias_members_once(void)
{
	const char *const members[] = { "/hello.txt", "/HELLO.TXT" };
	worker_fixture_t fixture;
	fsimg_error_t error;

	fixture_open(&fixture);
	fixture.request.kind = FSIMG_JOB_LIST;
	fixture.request.members = members;
	fixture.request.member_count = 2;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.result.entries == 1);
	CHECK(fixture.name_bytes == strlen("/HELLO.TXT"));
	fixture_close(&fixture);
}

static void
test_list_missing_member_names_error(void)
{
	const char *const members[] = { "/nothing-here" };
	worker_fixture_t fixture;
	fsimg_error_t error;

	fixture_open(&fixture);
	fixture.request.kind = FSIMG_JOB_LIST;
	fixture.request.members = members;
	fixture.request.member_count = 1;
	CHECK(run(&fixture, &error) == FSIMG_EINVAL);
	CHECK(error.status == FSIMG_EINVAL);
	CHECK(strcmp(error.path, members[0]) == 0);
	CHECK(fixture.result.entries == 0);
	CHECK(fixture.name_bytes == 0);
	fixture_close(&fixture);
}

/* Extra entries keep directory selection cases out of the shared fixture. */
static void
fixture_open_directories(worker_fixture_t *fixture)
{
	const fsimg_entry_t entries[] = {
		{ .path = "/DIR", .kind = FSIMG_ENTRY_DIRECTORY },
		{ .path = "/DIR/A.TXT", .kind = FSIMG_ENTRY_FILE, .size = 1 },
		{ .path = "/DIR/SUB", .kind = FSIMG_ENTRY_DIRECTORY },
		{ .path = "/DIR/SUB/B.TXT", .kind = FSIMG_ENTRY_FILE, .size = 1 }
	};

	fixture_open_with_entries(fixture, "fat16", entries,
	    sizeof(entries) / sizeof(entries[0]));
}

/* Compare complete NAME records, including their order and multiplicity. */
static fsimg_status_t
check_listed_path(void *argument, const fsimg_frame_t *frame,
    fsimg_worker_text_t *text, fsimg_error_t *error)
{
	listed_paths_t *paths = argument;
	fsimg_status_t status;

	status = event(paths->fixture, frame, text, error);
	if (status != FSIMG_OK)
		return (status);
	if (frame->type == FSIMG_FRAME_NAME) {
		CHECK(frame->length < sizeof(paths->path) - paths->used);
		bcopy(frame->payload, paths->path + paths->used, frame->length);
		paths->used += frame->length;
	}
	if (frame->type == FSIMG_FRAME_ENTRY_END) {
		paths->path[paths->used] = '\0';
		CHECK(paths->seen < paths->count);
		if (strcmp(paths->path, paths->expected[paths->seen]) != 0)
			fprintf(stderr, "listed path %zu: expected %s, received %s\n",
			    paths->seen, paths->expected[paths->seen], paths->path);
		CHECK(strcmp(paths->path, paths->expected[paths->seen]) == 0);
		paths->seen++;
		paths->used = 0;
	}
	return (FSIMG_OK);
}

static void
check_directory_listing(worker_fixture_t *fixture,
    const char *const *expected, size_t count)
{
	listed_paths_t paths = { .fixture = fixture, .expected = expected,
	    .count = count };
	fsimg_error_t error;

	fixture->request.kind = FSIMG_JOB_LIST;
	fixture->ready = false;
	CHECK(fsimg_worker_run(fixture->executable, &fixture->request,
	    check_listed_path, &paths, -1, NULL, &fixture->result, &error) == FSIMG_OK);
	CHECK(paths.seen == count && paths.used == 0);
	CHECK(fixture->result.entries == count);
}

static void
test_list_directory_member_precedes_subtree(void)
{
	const char *const members[] = { "/DIR" };
	const char *const expected[] = {
		"/DIR", "/DIR/A.TXT", "/DIR/SUB", "/DIR/SUB/B.TXT"
	};
	worker_fixture_t fixture;

	fixture_open_directories(&fixture);
	fixture.request.members = members;
	fixture.request.member_count = 1;
	fixture.request.flags = FSIMG_JOB_RECURSIVE;
	check_directory_listing(&fixture, expected, 4);
	fixture_close(&fixture);
}

static void
test_list_nested_directory_members_once(void)
{
	const char *const members[] = { "/DIR", "/DIR/SUB" };
	const char *const expected[] = {
		"/DIR", "/DIR/A.TXT", "/DIR/SUB", "/DIR/SUB/B.TXT"
	};
	worker_fixture_t fixture;

	fixture_open_directories(&fixture);
	fixture.request.members = members;
	fixture.request.member_count = 2;
	fixture.request.flags = FSIMG_JOB_RECURSIVE;
	check_directory_listing(&fixture, expected, 4);
	fixture_close(&fixture);
}

static void
test_list_directory_case_alias_members_once(void)
{
	const char *const members[] = { "/DIR", "/dir" };
	const char *const expected[] = {
		"/DIR", "/DIR/A.TXT", "/DIR/SUB", "/DIR/SUB/B.TXT"
	};
	worker_fixture_t fixture;

	fixture_open_directories(&fixture);
	fixture.request.members = members;
	fixture.request.member_count = sizeof(members) / sizeof(members[0]);
	fixture.request.flags = FSIMG_JOB_RECURSIVE;
	check_directory_listing(&fixture, expected,
	    sizeof(expected) / sizeof(expected[0]));
	fixture_close(&fixture);
}

static void
test_list_nested_directory_members_without_recursion_once(void)
{
	const char *const members[] = { "/DIR", "/DIR/SUB" };
	const char *const expected[] = {
		"/DIR", "/DIR/A.TXT", "/DIR/SUB", "/DIR/SUB/B.TXT"
	};
	worker_fixture_t fixture;

	fixture_open_directories(&fixture);
	fixture.request.members = members;
	fixture.request.member_count = sizeof(members) / sizeof(members[0]);
	fixture.request.flags = 0;
	check_directory_listing(&fixture, expected,
	    sizeof(expected) / sizeof(expected[0]));
	fixture_close(&fixture);
}

static void
test_list_reversed_nested_directory_members_keep_order(void)
{
	const char *const forward[] = { "/DIR", "/DIR/SUB" };
	const char *const reversed[] = { "/DIR/SUB", "/DIR" };
	const char *const expected[] = {
		"/DIR", "/DIR/A.TXT", "/DIR/SUB", "/DIR/SUB/B.TXT"
	};
	worker_fixture_t fixture;

	fixture_open_directories(&fixture);
	fixture.request.members = forward;
	fixture.request.member_count = sizeof(forward) / sizeof(forward[0]);
	fixture.request.flags = FSIMG_JOB_RECURSIVE;
	check_directory_listing(&fixture, expected,
	    sizeof(expected) / sizeof(expected[0]));
	fixture.request.members = reversed;
	fixture.request.member_count = sizeof(reversed) / sizeof(reversed[0]);
	check_directory_listing(&fixture, expected,
	    sizeof(expected) / sizeof(expected[0]));
	fixture_close(&fixture);
}

static void
test_list_directory_without_recursion_includes_target_and_children(void)
{
	const char *const members[] = { "/DIR" };
	const char *const expected[] = { "/DIR", "/DIR/A.TXT", "/DIR/SUB" };
	worker_fixture_t fixture;

	fixture_open_directories(&fixture);
	fixture.request.members = members;
	fixture.request.member_count = 1;
	fixture.request.flags = 0;
	check_directory_listing(&fixture, expected, 3);
	fixture_close(&fixture);
}

static void
test_list_deep_directory_includes_ancestor(void)
{
	const char *const members[] = { "/DIR/SUB" };
	const char *const expected[] = { "/DIR", "/DIR/SUB", "/DIR/SUB/B.TXT" };
	worker_fixture_t fixture;

	fixture_open_directories(&fixture);
	fixture.request.members = members;
	fixture.request.member_count = 1;
	fixture.request.flags = FSIMG_JOB_RECURSIVE;
	check_directory_listing(&fixture, expected, 3);
	fixture.request.flags = 0;
	check_directory_listing(&fixture, expected, 3);
	fixture_close(&fixture);
}

static void
test_list_root_and_no_members_have_no_root_entry(void)
{
	const char *const members[] = { "/" };
	const char *const direct_children[] = { "/DIR", "/HELLO.TXT" };
	const char *const expected[] = {
		"/DIR", "/DIR/A.TXT", "/DIR/SUB", "/DIR/SUB/B.TXT", "/HELLO.TXT"
	};
	worker_fixture_t fixture;

	fixture_open_directories(&fixture);
	fixture.request.flags = FSIMG_JOB_RECURSIVE;
	check_directory_listing(&fixture, expected, 5);
	fixture.request.flags = 0;
	check_directory_listing(&fixture, direct_children, 2);
	fixture.request.members = members;
	fixture.request.member_count = 1;
	fixture.request.flags = FSIMG_JOB_RECURSIVE;
	check_directory_listing(&fixture, expected, 5);
	fixture.request.flags = 0;
	check_directory_listing(&fixture, direct_children, 2);
	fixture_close(&fixture);
}

static void
test_list_and_extract_directory_counts_and_manifest_match(void)
{
	const char *const members[] = { "/DIR/SUB" };
	const char *const expected[] = {
		"/DIR", "/DIR/SUB", "/DIR/SUB/B.TXT"
	};
	worker_fixture_t fixture;
	fsimg_error_t error;
	uint64_t listed;
	fsimg_status_t status;
	char manifest[4096], *line, *next, *keyword;
	size_t seen;
	ssize_t bytes;
	int fd;

	fixture_open_directories(&fixture);
	fixture.request.members = members;
	fixture.request.member_count = 1;
	fixture.request.flags = FSIMG_JOB_RECURSIVE;
	/* The listing, extraction count and manifest agree. */
	check_directory_listing(&fixture, expected,
	    sizeof(expected) / sizeof(expected[0]));
	listed = fixture.result.entries;
	fd = openat(fixture.parent, "extracted.mtree",
	    O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
	CHECK(fd != -1);
	fixture.request.kind = FSIMG_JOB_EXTRACT;
	fixture.request.output_fd = fixture.output;
	fixture.request.manifest_fd = fd;
	status = run(&fixture, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "directory extraction: %d: %s\n", status, error.message);
	CHECK(status == FSIMG_OK);
	CHECK(fixture.result.entries == listed);
	CHECK(close(fd) == 0);
	fixture.request.manifest_fd = -1;
	fd = openat(fixture.parent, "extracted.mtree", O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1);
	bytes = pread(fd, manifest, sizeof(manifest) - 1, 0);
	CHECK(bytes > 0 && (size_t)bytes < sizeof(manifest) - 1);
	manifest[bytes] = '\0';
	seen = 0;
	for (line = strtok_r(manifest, "\n", &next); line != NULL;
	    line = strtok_r(NULL, "\n", &next)) {
		if (line[0] == '#' || strcmp(line, ". type=dir") == 0)
			continue;
		keyword = strchr(line, ' ');
		CHECK(keyword != NULL);
		*keyword = '\0';
		CHECK(line[0] == '.' && seen < sizeof(expected) / sizeof(expected[0]));
		CHECK(strcmp(line + 1, expected[seen]) == 0);
		seen++;
	}
	CHECK(seen == listed);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "extracted.mtree", 0) == 0);
	CHECK(unlinkat(fixture.output, "DIR/SUB/B.TXT", 0) == 0);
	CHECK(unlinkat(fixture.output, "DIR/SUB", AT_REMOVEDIR) == 0);
	CHECK(unlinkat(fixture.output, "DIR", AT_REMOVEDIR) == 0);
	fixture_close(&fixture);
}

static void
test_operations(const char *format)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	fsimg_status_t status;
	char bytes[8];
	int fd;

	fixture_open_format(&fixture, format);
	status = run(&fixture, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "worker info: %d: %s\n", status, error.message);
	CHECK(status == FSIMG_OK && fixture.info == 1);
	CHECK(error.status == FSIMG_OK);
	fixture.request.kind = FSIMG_JOB_LIST;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.result.entries == 1 && fixture.name_bytes == strlen("/HELLO.TXT"));
	fixture.request.kind = FSIMG_JOB_VERIFY;
	fixture.request.flags = FSIMG_JOB_PAYLOAD;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.result.logical_bytes == 7);
	fixture.request.flags = 0;
	fixture.request.kind = FSIMG_JOB_EXTRACT;
	fixture.request.output_fd = fixture.output;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.result.entries == 1 && fixture.result.logical_bytes == 7);
	fd = openat(fixture.output, "HELLO.TXT", O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1);
	CHECK(read(fd, bytes, sizeof(bytes)) == 7);
	CHECK(bcmp(bytes, "payload", 7) == 0);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.output, "HELLO.TXT", 0) == 0);

	fixture.request.kind = FSIMG_JOB_CAT;
	fixture.request.output_fd = -1;
	fixture.request.members = (const char *const[]){ "/hello.txt" };
	fixture.request.member_count = 1;
	fd = openat(fixture.parent, "raw", O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(fd != -1);
	fixture.ready = false;
	CHECK(fsimg_worker_run(fixture.executable, &fixture.request, event, &fixture,
	    fd, NULL, &fixture.result, &error) == FSIMG_OK);
	CHECK(fixture.result.raw_bytes == 7);
	CHECK(pread(fd, bytes, sizeof(bytes), 0) == 7);
	CHECK(bcmp(bytes, "payload", 7) == 0);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "raw", 0) == 0);

	fixture.request.kind = FSIMG_JOB_CREATE;
	fixture.request.input_fd = -1;
	fixture.request.input = (fsimg_input_t){ 0 };
	fixture.request.root_fds = &fixture.source;
	fixture.request.root_paths = image_root;
	fixture.request.root_count = 1;
	fixture.request.output_fd = fixture.output;
	CHECK(fsimg_stack_parse(strcmp(format, "exfat") == 0 ? "exfat" : "fat12",
	    &fixture.request.stack, NULL) == FSIMG_OK);
	if (strcmp(format, "exfat") == 0)
		fixture.request.write.label = "";
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.plans == 1 && fixture.result.image_bytes != 0);
	CHECK(fixture.plan_losses != 0 && fixture.result.metadata_losses == fixture.plan_losses);
	if (strcmp(format, "exfat") == 0) {
		fsimg_io_t *image;
		fsimg_reader_t *reader;
		fsimg_volume_info_t info;

		fd = openat(fixture.output, "image", O_RDONLY | O_NOFOLLOW);
		CHECK(fd != -1);
		CHECK(fsimg_io_from_fd(fixture.ctx, fd, FSIMG_IO_READ, &image, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_open(fixture.ctx, image, NULL, &reader, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_get_info(reader, &info, NULL) == FSIMG_OK);
		CHECK(info.label[0] == '\0' && info.time_basis == FSIMG_TIME_PER_TIMESTAMP);
		fsimg_reader_free(reader);
		fsimg_io_free(image);
		CHECK(close(fd) == 0);
	}
	CHECK(unlinkat(fixture.output, "image", 0) == 0);
	fixture.request.kind = FSIMG_JOB_PLAN;
	fixture.request.output_fd = -1;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.plans == 2);
	CHECK(fixture.result.metadata_losses == fixture.plan_losses);
	test_multiple_roots(&fixture);
	test_manifest_roots(&fixture);
	test_manifest_special_files(&fixture);
	fixture_close(&fixture);
}

/*
 * A create job of a partitioned disk reports the planned disk's IMAGE and
 * PARTITION records before its PLAN, carries its layer settings across
 * the job record, and stages the whole disk, whose length the parent
 * accepts as the image's.
 */
static void
test_partitioned_create(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	struct stat st;
	unsigned char bytes[8];
	int fd;

	fixture_open(&fixture);
	fixture.request.kind = FSIMG_JOB_CREATE;
	fixture.request.input_fd = -1;
	fixture.request.input = (fsimg_input_t){ 0 };
	fixture.request.root_fds = &fixture.source;
	fixture.request.root_paths = image_root;
	fixture.request.root_count = 1;
	fixture.request.output_fd = fixture.output;
	fixture.request.write.flags |= FSIMG_WRITE_REPRODUCIBLE;
	CHECK(fsimg_stack_parse("fat12/mbr", &fixture.request.stack, NULL) ==
	    FSIMG_OK);
	fixture.request.layers.mbr_id = 0x0badcafe;
	fixture.request.layers.mbr_id_set = true;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.images == 1 && fixture.partitions == 1 && fixture.plans == 1);
	fd = openat(fixture.output, "image", O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1 && fstat(fd, &st) == 0);
	CHECK((uint64_t)st.st_size == fixture.result.image_bytes);
	CHECK(pread(fd, bytes, 4, 440) == 4 && bytes[0] == 0xfe && bytes[3] == 0x0b);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.output, "image", 0) == 0);

	/* A refused setting fails the job in the worker, naming the key. */
	snprintf(fixture.request.layers.mbr_type,
	    sizeof(fixture.request.layers.mbr_type), "0xee");
	bzero(&error, sizeof(error));
	CHECK(run(&fixture, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "mbr:type") == 0);
	CHECK(faccessat(fixture.output, "image", F_OK, 0) == -1);
	fixture_close(&fixture);
}

/* An unlinked, empty read/write file in scratch, as the coordinator makes
 * the raw intermediate.
 */
static int
raw_intermediate(const worker_fixture_t *fixture)
{
	int fd;

	fd = openat(fixture->scratch, "raw", O_RDWR | O_CREAT | O_EXCL |
	    O_NOFOLLOW | O_CLOEXEC, 0600);
	CHECK(fd != -1 && unlinkat(fixture->scratch, "raw", 0) == 0);
	return (fd);
}

/* A reproducible creation of the source tree as stack into staging. */
static void
create_request(worker_fixture_t *fixture, const char *stack)
{
	fixture->request.kind = FSIMG_JOB_CREATE;
	fixture->request.flags = 0;
	fixture->request.input_fd = -1;
	fixture->request.input = (fsimg_input_t){ 0 };
	fixture->request.root_fds = &fixture->source;
	fixture->request.root_paths = image_root;
	fixture->request.root_count = 1;
	fixture->request.output_fd = fixture->output;
	fixture->request.write.flags |= FSIMG_WRITE_REPRODUCIBLE;
	CHECK(fsimg_stack_parse(stack, &fixture->request.stack, NULL) ==
	    FSIMG_OK);
}

/*
 * A creation with a container writes the container through the raw
 * intermediate the coordinator hands over: the IMAGE record names it, and
 * DONE reports the staged file's length, which the plan bounds. Without
 * the raw intermediate the worker refuses its authority. A stream writes
 * the same bytes to the data channel, which the coordinator relays to
 * stdout and counts. A fault the staged file acquires afterwards fails the
 * verification job, which names the container in error.path; that is
 * what cW reports before publishing nothing.
 */
static void
test_container_create(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	struct stat st;
	unsigned char *staged, *streamed, byte;
	int fd, stream;

	fixture_open(&fixture);
	create_request(&fixture, "fat12/gpt/dmg");
	fixture.request.raw_fd = raw_intermediate(&fixture);
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(close(fixture.request.raw_fd) == 0);
	CHECK(fixture.images == 1 && fixture.partitions == 1 && fixture.plans == 1);
	fd = openat(fixture.output, "image", O_RDWR | O_NOFOLLOW);
	CHECK(fd != -1 && fstat(fd, &st) == 0);
	CHECK(st.st_size > 0 && (uint64_t)st.st_size == fixture.result.image_bytes);
	staged = malloc((size_t)st.st_size);
	CHECK(staged != NULL &&
	    pread(fd, staged, (size_t)st.st_size, 0) == st.st_size);
	CHECK(bcmp(staged + st.st_size - 512, "koly", 4) == 0);

	/* The same creation streamed: nothing staged, the bytes relayed. */
	stream = openat(fixture.parent, "stream", O_RDWR | O_CREAT | O_EXCL, 0600);
	CHECK(stream != -1);
	fixture.request.flags = FSIMG_JOB_STDOUT;
	fixture.request.output_fd = -1;
	fixture.request.raw_fd = raw_intermediate(&fixture);
	fixture.ready = false;
	CHECK(fsimg_worker_run(fixture.executable, &fixture.request, event,
	    &fixture, stream, NULL, &fixture.result, &error) == FSIMG_OK);
	CHECK(close(fixture.request.raw_fd) == 0);
	CHECK(fixture.result.image_bytes == 0 &&
	    fixture.result.raw_bytes == (uint64_t)st.st_size);
	streamed = malloc((size_t)st.st_size);
	CHECK(streamed != NULL &&
	    pread(stream, streamed, (size_t)st.st_size, 0) == st.st_size);
	CHECK(bcmp(staged, streamed, (size_t)st.st_size) == 0);
	CHECK(close(stream) == 0 && unlinkat(fixture.parent, "stream", 0) == 0);
	free(streamed);
	free(staged);

	/* A creation needing a raw intermediate refuses to run without one. */
	fixture.request.flags = 0;
	fixture.request.output_fd = fixture.output;
	fixture.request.raw_fd = -1;
	CHECK(run(&fixture, &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "filesystem authority") != NULL);

	/* A data fork byte flipped after the creation fails verification. */
	CHECK(pread(fd, &byte, 1, 64) == 1);
	byte ^= 0xff;
	CHECK(pwrite(fd, &byte, 1, 64) == 1);
	CHECK(close(fd) == 0);
	fixture.request.kind = FSIMG_JOB_VERIFY;
	fixture.request.flags = FSIMG_JOB_PAYLOAD;
	fixture.request.root_fds = NULL;
	fixture.request.root_paths = NULL;
	fixture.request.root_count = 0;
	fixture.request.output_fd = -1;
	fixture.request.input_fd = openat(fixture.output, "image", O_RDONLY);
	CHECK(fsimg_input_prepare(fixture.request.input_fd, NULL, NULL,
	    &fixture.request.input, NULL) == FSIMG_OK);
	CHECK(fixture.request.input_fd != -1);
	bzero(&error, sizeof(error));
	CHECK(run(&fixture, &error) == FSIMG_ECORRUPT);
	CHECK(error.operation == FSIMG_OP_IMAGE && strcmp(error.path, "dmg") == 0);
	CHECK(close(fixture.request.input_fd) == 0);
	fixture.request.input_fd = -1;
	fixture.request.input = (fsimg_input_t){ 0 };
	CHECK(unlinkat(fixture.output, "image", 0) == 0);
	fixture_close(&fixture);
}

/*
 * A gzip creation writes one member through the raw intermediate and sends
 * an IMAGE record for it, as for a container. A CRC32 flipped in the staged
 * member afterwards fails the verification job, which decodes the member
 * into its spool and names gzip in error.path; that is what cW reports
 * before publishing nothing.
 */
static void
test_compressed_create(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	struct stat st;
	unsigned char header[2], byte;
	int fd;

	fixture_open(&fixture);
	create_request(&fixture, "fat12");
	snprintf(fixture.request.compression, sizeof(fixture.request.compression),
	    "gzip");
	fixture.request.raw_fd = raw_intermediate(&fixture);
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(close(fixture.request.raw_fd) == 0);
	CHECK(fixture.images == 1 && fixture.plans == 1);
	fd = openat(fixture.output, "image", O_RDWR | O_NOFOLLOW);
	CHECK(fd != -1 && fstat(fd, &st) == 0);
	CHECK((uint64_t)st.st_size == fixture.result.image_bytes);
	CHECK(pread(fd, header, 2, 0) == 2 && header[0] == 0x1f &&
	    header[1] == 0x8b);
	CHECK(pread(fd, &byte, 1, st.st_size - 8) == 1);
	byte ^= 0xff;
	CHECK(pwrite(fd, &byte, 1, st.st_size - 8) == 1);
	CHECK(close(fd) == 0);
	/* The request cW sends: every slot and the compression forced. */
	fixture.request.kind = FSIMG_JOB_VERIFY;
	fixture.request.flags = 0;
	for (unsigned i = FSIMG_LAYER_SCHEME; i < FSIMG_STACK_SLOTS; i++) {
		if (fixture.request.stack.slots[i].state == FSIMG_SLOT_UNSPECIFIED)
			fixture.request.stack.slots[i].state = FSIMG_SLOT_EMPTY;
	}
	fixture.request.root_fds = NULL;
	fixture.request.root_paths = NULL;
	fixture.request.root_count = 0;
	fixture.request.output_fd = fixture.request.raw_fd = -1;
	fixture.request.spool_fd = raw_intermediate(&fixture);
	fixture.request.input_fd = openat(fixture.output, "image", O_RDONLY);
	CHECK(fsimg_input_prepare(fixture.request.input_fd, NULL, NULL,
	    &fixture.request.input, NULL) == FSIMG_OK);
	CHECK(fixture.request.input_fd != -1);
	bzero(&error, sizeof(error));
	CHECK(run(&fixture, &error) == FSIMG_ECORRUPT);
	CHECK(error.operation == FSIMG_OP_IMAGE && strcmp(error.path, "gzip") == 0);
	CHECK(close(fixture.request.input_fd) == 0 &&
	    close(fixture.request.spool_fd) == 0);
	fixture.request.input_fd = fixture.request.spool_fd = -1;
	fixture.request.input = (fsimg_input_t){ 0 };
	CHECK(unlinkat(fixture.output, "image", 0) == 0);
	fixture_close(&fixture);
}

/* A plan job with several roots: the source directory at the image root and
 * again at "/a/b" beneath a synthesized "/a", and one of its files as a
 * regular-file root. The worker validates every root path and refuses an
 * image path two roots supply.
 */
static void
test_multiple_roots(worker_fixture_t *fixture)
{
	static const char *const paths[] = { "/", "/a/b", "/c.txt" };
	static const char *const invalid[] = { "/", "/a/../b", "/c.txt" };
	static const char *const colliding[] = { "/", "/HELLO.TXT", "/c.txt" };
	fsimg_error_t error;
	uint64_t single;
	int roots[3];

	single = fixture->result.entries;
	roots[0] = fixture->source;
	roots[1] = fixture->source;
	roots[2] = openat(fixture->source, "HELLO.TXT", O_RDONLY | O_CLOEXEC);
	CHECK(roots[2] != -1);
	fixture->request.kind = FSIMG_JOB_PLAN;
	fixture->request.root_fds = roots;
	fixture->request.root_paths = paths;
	fixture->request.root_count = 3;
	CHECK(run(fixture, &error) == FSIMG_OK);
	/* "/a", "/a/b", "/a/b/HELLO.TXT" and "/c.txt" beside the single root's. */
	CHECK(fixture->result.entries == single + 4);

	fixture->request.root_paths = invalid;
	CHECK(run(fixture, &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "image path component") != NULL);
	fixture->request.root_paths = colliding;
	CHECK(run(fixture, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "/HELLO.TXT") == 0);
	CHECK(close(roots[2]) == 0);
	fixture->request.root_fds = &fixture->source;
	fixture->request.root_paths = image_root;
	fixture->request.root_count = 1;
}

/* A manifest root is the manifest's root descriptor, with the path
 * FSIMG_JOB_MANIFEST, and its content directory's, with the path
 * FSIMG_JOB_CONTENTS, when it has one: the worker parses the manifest,
 * reads a host file's bytes beneath the directory, and merges its entries
 * with a host root's. Without the directory the manifest's generated files
 * are imported and an entry needing host bytes is refused, naming its line
 * and path, although the next root is a directory holding that file; such
 * a manifest may be the last root. A content directory path anywhere but
 * right after a manifest's is refused before any root is read.
 */
static void
test_manifest_roots(worker_fixture_t *fixture)
{
	static const char manifest[] =
	    ". type=dir\n"
	    "./HELLO.TXT type=file\n"
	    "./generated type=file fsimg.content=seeded fsimg.seed=1 size=40\n";
	static const char generated[] =
	    "./generated type=file fsimg.content=seeded fsimg.seed=1 size=40\n";
	static const char *const paths[] = { FSIMG_JOB_MANIFEST,
	    FSIMG_JOB_CONTENTS, "/copy" };
	static const char *const without_contents[] = { FSIMG_JOB_MANIFEST,
	    "/copy" };
	static const char *const last[] = { FSIMG_JOB_MANIFEST, "/copy",
	    FSIMG_JOB_MANIFEST };
	static const char *const leading[] = { FSIMG_JOB_CONTENTS, "/copy",
	    "/other" };
	static const char *const orphaned[] = { "/copy", FSIMG_JOB_CONTENTS,
	    "/other" };
	static const char *const repeated[] = { FSIMG_JOB_MANIFEST,
	    FSIMG_JOB_CONTENTS, FSIMG_JOB_CONTENTS };
	fsimg_error_t error;
	int roots[3];

	roots[0] = open_manifest(fixture, "manifest", manifest);
	roots[1] = fixture->source;
	roots[2] = fixture->source;
	fixture->request.kind = FSIMG_JOB_PLAN;
	fixture->request.root_fds = roots;
	fixture->request.root_paths = paths;
	fixture->request.root_count = 3;
	CHECK(run(fixture, &error) == FSIMG_OK);
	/* The manifest's two files, and "/copy" holding the source's file. */
	CHECK(fixture->result.entries == 4);

	fixture->request.root_paths = without_contents;
	fixture->request.root_count = 2;
	CHECK(run(fixture, &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "manifest line 2: ") == error.message);
	CHECK(strstr(error.message, "no content directory") != NULL);
	CHECK(strcmp(error.path, "/HELLO.TXT") == 0);
	CHECK(close(roots[0]) == 0);
	CHECK(unlinkat(fixture->parent, "manifest", 0) == 0);
	roots[0] = open_manifest(fixture, "manifest", generated);
	CHECK(run(fixture, &error) == FSIMG_OK);
	/* The generated file, and "/copy" holding the source's file. */
	CHECK(fixture->result.entries == 3);
	/* A manifest as the last root: the next path is never read. */
	roots[2] = open_manifest(fixture, "second",
	    "./other type=file fsimg.content=zero size=3\n");
	fixture->request.root_count = 3;
	fixture->request.root_paths = last;
	CHECK(run(fixture, &error) == FSIMG_OK);
	CHECK(fixture->result.entries == 4);
	CHECK(close(roots[2]) == 0);
	CHECK(unlinkat(fixture->parent, "second", 0) == 0);
	roots[2] = fixture->source;

	fixture->request.root_paths = leading;
	CHECK(run(fixture, &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "manifest root") != NULL);
	fixture->request.root_paths = orphaned;
	CHECK(run(fixture, &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "manifest root") != NULL);
	fixture->request.root_paths = repeated;
	CHECK(run(fixture, &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "manifest root") != NULL);
	CHECK(close(roots[0]) == 0);
	CHECK(unlinkat(fixture->parent, "manifest", 0) == 0);
	fixture->request.root_fds = &fixture->source;
	fixture->request.root_paths = image_root;
	fixture->request.root_count = 1;
}

/* A manifest file entry whose host file is a FIFO or a socket is refused
 * as unsupported, not a regular file, by the confined worker and without
 * blocking: every host opens the name no-follow and nonblocking and checks
 * the descriptor, and a socket, which no host opens, fails the open with
 * the same refusal. Where the host has a sandbox the job requires it, so
 * an unconfined run cannot pass for a confined one; a kernel that cannot
 * confine creation refuses the job instead, which leaves the refusals
 * untested there, as test-sandbox reports.
 */
static void
test_manifest_special_files(worker_fixture_t *fixture)
{
	static const char *const paths[] = { FSIMG_JOB_MANIFEST,
	    FSIMG_JOB_CONTENTS };
	static const char *const manifests[] = {
	    ". type=dir\n./pipe type=file\n",
	    ". type=dir\n./socket type=file\n"
	};
	fsimg_error_t error;
	fsimg_sandbox_policy_t sandbox;
	int roots[2], listener;

	CHECK(mkdirat(fixture->parent, "special-contents", 0700) == 0);
	roots[1] = openat(fixture->parent, "special-contents",
	    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(roots[1] != -1);
	CHECK(mkfifoat(roots[1], "pipe", 0600) == 0);
	listener = bind_socket(roots[1], "socket");
	fixture->request.kind = FSIMG_JOB_PLAN;
	fixture->request.root_fds = roots;
	fixture->request.root_paths = paths;
	fixture->request.root_count = 2;
	sandbox = fixture->request.sandbox;
#if defined(__linux__) || defined(__FreeBSD__)
	fixture->request.sandbox = FSIMG_SANDBOX_REQUIRED;
#endif

	for (size_t i = 0; i < sizeof(manifests) / sizeof(manifests[0]); i++) {
		roots[0] = open_manifest(fixture, "special-manifest", manifests[i]);
		CHECK(run(fixture, &error) == FSIMG_EUNSUPPORTED);
		if (fixture->request.sandbox == FSIMG_SANDBOX_REQUIRED &&
		    fixture->result.confinement != FSIMG_CONFINEMENT_ACTIVE)
			fprintf(stderr, "manifest special-file refusal untested: "
			    "creation cannot be confined here\n");
		else
			CHECK(strstr(error.message, "not name a regular file") !=
			    NULL);
		CHECK(close(roots[0]) == 0);
		CHECK(unlinkat(fixture->parent, "special-manifest", 0) == 0);
	}
	fixture->request.sandbox = sandbox;

	CHECK(close(listener) == 0);
	CHECK(unlinkat(roots[1], "socket", 0) == 0);
	CHECK(unlinkat(roots[1], "pipe", 0) == 0);
	CHECK(close(roots[1]) == 0);
	CHECK(unlinkat(fixture->parent, "special-contents", AT_REMOVEDIR) == 0);
	fixture->request.root_fds = &fixture->source;
	fixture->request.root_paths = image_root;
	fixture->request.root_count = 1;
}

/* Write text to a new file name beneath the fixture's directory and
 * return a read-only descriptor for it, as the coordinator opens a
 * manifest root.
 */
static int
open_manifest(const worker_fixture_t *fixture, const char *name,
    const char *text)
{
	size_t length;
	int fd;

	length = strlen(text);
	fd = openat(fixture->parent, name, O_CREAT | O_EXCL | O_WRONLY |
	    O_CLOEXEC, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, text, length) == (ssize_t)length);
	CHECK(close(fd) == 0);
	fd = openat(fixture->parent, name, O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	return (fd);
}

/* Children close their channels just before they exit, and no descriptor
 * reports the exit itself. The supervisor must reap them promptly rather
 * than sleep out its cancellation-poll interval (100 ms) after the last
 * frame, which would add that interval to every operation. The fastest of
 * ten info operations shows it whatever the host's load; a sanitizer
 * runtime's process startup alone exceeds the bound, so those builds skip.
 */
static void
test_prompt_completion(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	struct timespec start, end;
	long long elapsed_ms, fastest_ms = -1;

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
	puts("SKIP: worker completion latency is not measurable under a sanitizer runtime");
	return;
#endif
#endif
	fixture_open_format(&fixture, "fat16");
	for (int i = 0; i < 10; i++) {
		CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
		CHECK(run(&fixture, &error) == FSIMG_OK);
		CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
		elapsed_ms = (long long)(end.tv_sec - start.tv_sec) * 1000 +
		    (end.tv_nsec - start.tv_nsec) / 1000000;
		if (fastest_ms == -1 || elapsed_ms < fastest_ms)
			fastest_ms = elapsed_ms;
	}
	if (fastest_ms >= 50)
		fprintf(stderr, "fastest worker operation took %lld ms\n", fastest_ms);
	CHECK(fastest_ms < 50);
	fixture_close(&fixture);
}

static void
test_startup(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	fsimg_status_t status;

	fixture_open(&fixture);
	fixture.refuse_ready = true;
	fixture.dirty_event = true;
	bzero(&error, sizeof(error));
	CHECK(run(&fixture, &error) == FSIMG_ECANCELLED);
	CHECK(error.status == FSIMG_ECANCELLED);
	CHECK(fixture.ready && fixture.info == 0);
	fixture.refuse_ready = false;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(error.status == FSIMG_OK);
	fixture.info = 0;
	fixture.dirty_event = false;
	fixture.request.sandbox = FSIMG_SANDBOX_REQUIRED;
	status = run(&fixture, &error);
#if defined(__APPLE__)
	CHECK(status == FSIMG_EUNSUPPORTED);
	CHECK(fixture.info == 0);
#else
	CHECK(status == FSIMG_OK || status == FSIMG_EUNSUPPORTED);
#endif
	fixture.request.sandbox = FSIMG_SANDBOX_OFF;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.result.confinement == FSIMG_CONFINEMENT_DISABLED);
	fixture.request.kind = FSIMG_JOB_EXTRACT;
	fixture.request.output_fd = fixture.output;
	fixture.info = 0;
	for (uint64_t limit = 1; limit <= 2; limit++) {
		fixture.request.limits.max_open_files = limit;
		CHECK(run(&fixture, &error) == FSIMG_ELIMIT);
		CHECK(!fixture.ready && fixture.info == 0);
		CHECK(fixture.result.confinement == FSIMG_CONFINEMENT_UNKNOWN);
	}
	fixture_close(&fixture);
}

static void
test_inheritance(void)
{
	worker_fixture_t fixture;
	struct rlimit original, lowered;
	int high;

	fixture_open(&fixture);
	high = fcntl(fixture.input, F_DUPFD, 200);
	CHECK(high >= 200);
	CHECK(getrlimit(RLIMIT_NOFILE, &original) == 0);
	lowered = original;
	lowered.rlim_cur = 64;
	CHECK(setrlimit(RLIMIT_NOFILE, &lowered) == 0);
	CHECK(run(&fixture, NULL) == FSIMG_OK);
	CHECK(setrlimit(RLIMIT_NOFILE, &original) == 0);
	CHECK(close(high) == 0);
	fixture_close(&fixture);
}

/* This test-only executable can act as a compromised worker. An executable
 * fixture marks the attack and its size selects the scenario, without
 * adding test flags or hooks to production worker code. It never parses
 * image bytes.
 */
static int
fake_worker(worker_scenario_t mode)
{
	fsimg_frame_t frame;
	unsigned char raw[FSIMG_FRAME_BYTES], header[FSIMG_FRAME_HEADER];
	uint64_t remaining;
	struct timespec delay = { .tv_nsec = 200000000 };
	fsimg_error_t error;
	int output;
#ifdef FSIMG_TEST_ASAN
	unsigned char *allocation;
#endif

	if (mode == SCENARIO_STARTUP_NONREADER) {
		for (;;)
			pause();
	}

	CHECK(fsimg_frame_read(FSIMG_CHILD_JOB, &frame, NULL) == FSIMG_OK);
	if (mode == SCENARIO_STARTUP_ERROR || mode == SCENARIO_FORGED_PUBLISHED || mode == SCENARIO_FORGED_UNCERTAIN) {
		bzero(&error, sizeof(error));
		error.status = mode == SCENARIO_STARTUP_ERROR ? FSIMG_EINVAL : FSIMG_EIO;
		if (mode != SCENARIO_STARTUP_ERROR)
			error.output_state = mode == SCENARIO_FORGED_PUBLISHED ? FSIMG_OUTPUT_PUBLISHED : FSIMG_OUTPUT_UNCERTAIN;
		strcpy(error.message, "startup test rejection");
		fsimg_wire_error_encode(raw, &error);
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_ERROR,
		    raw, FSIMG_WIRE_ERROR_BYTES, NULL) == FSIMG_OK);
		return (1);
	}
	CHECK(fsimg_frame_read(FSIMG_CHILD_JOB, &frame, NULL) == FSIMG_OK);
	bzero(raw, sizeof(raw));
#ifdef FSIMG_TEST_ASAN
	if (mode == SCENARIO_ASAN_FAILURE) {
		allocation = malloc(1);
		CHECK(allocation != NULL);
		release_test_memory(allocation);
		return (((volatile unsigned char *)allocation)[0]);
	}
#endif
	if (mode == SCENARIO_PREMATURE_RAW) {
		CHECK(write(FSIMG_CHILD_DATA, "ATTACK", 6) == 6);
		nanosleep(&delay, NULL);
	}
	if (mode == SCENARIO_DONE_BEFORE_READY) {
		fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_DONE, raw, FSIMG_WIRE_DONE_BYTES, NULL);
		return (0);
	}
	if (mode == SCENARIO_TRUNCATED_HEADER) {
		CHECK(fsimg_frame_header(header, FSIMG_FRAME_READY, 8, NULL) == FSIMG_OK);
		CHECK(write(FSIMG_CHILD_CONTROL, header, 3) == 3);
		return (0);
	}
	fsimg_store_le64(raw, 2);
	CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_READY, raw, 8, NULL) == FSIMG_OK);
	if (mode == SCENARIO_ACK_ERROR) {
		CHECK(close(FSIMG_CHILD_JOB) == 0);
		bzero(&error, sizeof(error));
		error.status = FSIMG_ECORRUPT;
		strcpy(error.message, "acknowledgement test rejection");
		fsimg_wire_error_encode(raw, &error);
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_ERROR,
		    raw, FSIMG_WIRE_ERROR_BYTES, NULL) == FSIMG_OK);
		return (1);
	}
	CHECK(fsimg_frame_read(FSIMG_CHILD_JOB, &frame, NULL) == FSIMG_OK);
	if (mode == SCENARIO_DONE_BEFORE_INFO) {
		bzero(raw, FSIMG_WIRE_DONE_BYTES);
		fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_DONE, raw, FSIMG_WIRE_DONE_BYTES, NULL);
		return (0);
	}
	if (mode == SCENARIO_NAME_WITHOUT_ENTRY) {
		fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_NAME, "X", 1, NULL);
		return (0);
	}
	/* As many valid metadata loss examples as a worker may send, or one
	 * more, then an otherwise correct job.
	 */
	if (mode == SCENARIO_LIMIT_LOSS || mode == SCENARIO_EXCESS_LOSS) {
		for (unsigned i = 0; i < FSIMG_WIRE_LOSS_EVENTS +
		    (mode == SCENARIO_EXCESS_LOSS ? 1 : 0); i++) {
			bzero(raw, FSIMG_WIRE_LOSS_BYTES + 1);
			fsimg_store_le64(raw + FSIMG_WIRE_LOSS_FIELDS,
			    FSIMG_META_MODIFIED);
			fsimg_store_le64(raw + FSIMG_WIRE_LOSS_LENGTH, 1);
			raw[FSIMG_WIRE_LOSS_PATH] = 'X';
			if (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_LOSS,
			    raw, FSIMG_WIRE_LOSS_BYTES + 1, NULL) != FSIMG_OK)
				return (0);
		}
	}
	/* The IMAGE record describes the input, whose size is the scenario. */
	bzero(raw, FSIMG_WIRE_IMAGE_BYTES);
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_DISK, mode == SCENARIO_IMAGE_WRONG_DISK ?
	    (uint64_t)mode + 1 : (uint64_t)mode);
	fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FILE, (uint64_t)mode);
	if (mode == SCENARIO_PARTITION_OUTSIDE || mode == SCENARIO_PARTITION_UNSELECTED_LIST) {
		strcpy((char *)raw + FSIMG_WIRE_IMAGE_SCHEME, "mbr");
		fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_SECTOR, 512);
		fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_SLOTS, 4);
	}
	/* A fixed VHD's disk is the file less its footer, never the file. */
	if (mode == SCENARIO_VHD_WHOLE_FILE)
		strcpy((char *)raw + FSIMG_WIRE_IMAGE_CONTAINER, "vhd");
	if (mode == SCENARIO_PARTITION_UNSELECTED_LIST) {
		fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FLAGS, FSIMG_WIRE_IMAGE_UNSELECTED);
		strcpy((char *)raw + FSIMG_WIRE_IMAGE_REASON, "no single filesystem");
	}
	if (mode != SCENARIO_INFO_WITHOUT_IMAGE)
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_IMAGE, raw,
		    FSIMG_WIRE_IMAGE_BYTES, NULL) == FSIMG_OK);
	if (mode == SCENARIO_PARTITION_OUTSIDE) {
		bzero(raw, FSIMG_WIRE_PARTITION_BYTES);
		fsimg_store_le64(raw + FSIMG_WIRE_PARTITION_INDEX, 1);
		fsimg_store_le64(raw + FSIMG_WIRE_PARTITION_OFFSET, 1);
		fsimg_store_le64(raw + FSIMG_WIRE_PARTITION_LENGTH, (uint64_t)mode);
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_PARTITION, raw,
		    FSIMG_WIRE_PARTITION_BYTES, NULL) == FSIMG_OK);
	}
	bzero(raw, FSIMG_WIRE_INFO_BYTES);
	fsimg_store_le64(raw + 56, mode == SCENARIO_INVALID_TIME_BASIS ? UINT64_MAX : FSIMG_TIME_WALL);
	fsimg_store_le64(raw + 64, FSIMG_VALIDATION_HEADER);
	CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_INFO, raw, FSIMG_WIRE_INFO_BYTES, NULL) == FSIMG_OK);
	if (mode == SCENARIO_PARTIAL_EXTRACTION) {
		output = openat(FSIMG_CHILD_OUTPUT, "partial", O_WRONLY | O_CREAT | O_EXCL, 0600);
		CHECK(output != -1);
		CHECK(write(output, "prefix", 6) == 6);
		CHECK(close(output) == 0);
		return (1);
	}
	if (mode >= SCENARIO_VERIFY_INVALID_SCOPE && mode <= SCENARIO_VERIFY_MISSING_PAYLOAD) {
		bzero(raw, 72);
		fsimg_store_le64(raw, mode == SCENARIO_VERIFY_INVALID_SCOPE ? UINT64_MAX : mode == SCENARIO_VERIFY_HEADER_SCOPE ?
		    FSIMG_VALIDATION_HEADER : mode == SCENARIO_VERIFY_UNREQUESTED_PAYLOAD ? FSIMG_VALIDATION_PAYLOAD :
		    FSIMG_VALIDATION_STRUCTURAL);
		fsimg_store_le64(raw + 8, 1);
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_VERIFY,
		    raw, 72, NULL) == FSIMG_OK);
	}
	if (mode == SCENARIO_MISSING_DONE)
		return (0);
	if (mode == SCENARIO_STALLED_PARSER) {
		signal(SIGTERM, SIG_IGN);
		for (;;)
			pause();
	}
	if (mode == SCENARIO_LONG_LIST_NAME) {
		bzero(raw, FSIMG_WIRE_ENTRY_BYTES);
		fsimg_store_le64(raw, FSIMG_ENTRY_FILE);
		fsimg_store_le64(raw + 16, 65535);
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_ENTRY, raw, FSIMG_WIRE_ENTRY_BYTES, NULL) == FSIMG_OK);
		memset(raw, 'N', sizeof(raw));
		raw[0] = '/';
		remaining = 65535;
		while (remaining != 0) {
			uint32_t batch = remaining > sizeof(raw) ? sizeof(raw) : (uint32_t)remaining;

			CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_NAME, raw, batch, NULL) == FSIMG_OK);
			remaining -= batch;
		}
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_ENTRY_END, NULL, 0, NULL) == FSIMG_OK);
	}
	bzero(raw, FSIMG_WIRE_DONE_BYTES);
	fsimg_store_le64(raw, mode == SCENARIO_LONG_LIST_NAME ? 1 : 0);
	if (mode == SCENARIO_SKIPPED_EXTRACTION)
		fsimg_store_le64(raw + FSIMG_WIRE_DONE_SKIPPED, 1);
	if (mode == SCENARIO_OMITTED_EXTRACTION)
		fsimg_store_le64(raw + FSIMG_WIRE_DONE_OMITTED, 1);
	CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_DONE, raw, FSIMG_WIRE_DONE_BYTES, NULL) == FSIMG_OK);
	if (mode == SCENARIO_DUPLICATE_DONE)
		fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_DONE, raw, FSIMG_WIRE_DONE_BYTES, NULL);
	if (mode == SCENARIO_RUNTIME_FAILURE)
		return (FSIMG_WORKER_RUNTIME_EXIT);
	return (mode == SCENARIO_BAD_EXIT ? 1 : 0);
}

static void
test_incomplete_extraction(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	int fd;

	fixture_open(&fixture);
	fd = openat(fixture.parent, "fake", O_CREAT | O_EXCL | O_RDWR, 0700);
	CHECK(fd != -1);
	CHECK(ftruncate(fd, SCENARIO_PARTIAL_EXTRACTION) == 0);
	CHECK(fchmod(fd, 0700) == 0);
	CHECK(close(fd) == 0);
	fd = openat(fixture.parent, "fake", O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	fixture.request.input_fd = fd;
	CHECK(fsimg_input_prepare(fixture.request.input_fd, NULL, NULL,
	    &fixture.request.input, NULL) == FSIMG_OK);
	fixture.request.output_fd = fixture.output;
	fixture.request.kind = FSIMG_JOB_EXTRACT;
	fixture.request.sandbox = FSIMG_SANDBOX_OFF;
	CHECK(run(&fixture, &error) == FSIMG_EIO);
	CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
	CHECK(unlinkat(fixture.output, "partial", 0) == 0);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "fake", 0) == 0);
	fixture_close(&fixture);
}

/* A worker may report skipped entries only for an extraction that asked
 * for skipping; otherwise the count could only be a forged warning.
 */
static void
test_skipped_extraction(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	fsimg_status_t status;
	int fd;

	fixture_open(&fixture);
	fd = openat(fixture.parent, "fake", O_CREAT | O_EXCL | O_RDWR, 0700);
	CHECK(fd != -1);
	CHECK(ftruncate(fd, SCENARIO_SKIPPED_EXTRACTION) == 0);
	CHECK(fchmod(fd, 0700) == 0);
	CHECK(close(fd) == 0);
	fd = openat(fixture.parent, "fake", O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	fixture.request.input_fd = fd;
	CHECK(fsimg_input_prepare(fixture.request.input_fd, NULL, NULL,
	    &fixture.request.input, NULL) == FSIMG_OK);
	fixture.request.output_fd = fixture.output;
	fixture.request.kind = FSIMG_JOB_EXTRACT;
	fixture.request.sandbox = FSIMG_SANDBOX_OFF;
	CHECK(run(&fixture, &error) == FSIMG_EIO);
	fixture.request.flags = FSIMG_JOB_SKIP_UNSUPPORTED;
	status = run(&fixture, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "skipped extraction: %d: %s\n", status, error.message);
	CHECK(status == FSIMG_OK && fixture.result.skipped_entries == 1);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "fake", 0) == 0);
	fixture_close(&fixture);
}

/* Likewise omitted entries: an extraction that did not ask to skip fails
 * at the first one, so a count there could only be forged.
 */
static void
test_omitted_extraction(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	fsimg_status_t status;
	int fd;

	fixture_open(&fixture);
	fd = openat(fixture.parent, "fake", O_CREAT | O_EXCL | O_RDWR, 0700);
	CHECK(fd != -1);
	CHECK(ftruncate(fd, SCENARIO_OMITTED_EXTRACTION) == 0);
	CHECK(fchmod(fd, 0700) == 0);
	CHECK(close(fd) == 0);
	fd = openat(fixture.parent, "fake", O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	fixture.request.input_fd = fd;
	CHECK(fsimg_input_prepare(fixture.request.input_fd, NULL, NULL,
	    &fixture.request.input, NULL) == FSIMG_OK);
	fixture.request.output_fd = fixture.output;
	fixture.request.kind = FSIMG_JOB_EXTRACT;
	fixture.request.sandbox = FSIMG_SANDBOX_OFF;
	CHECK(run(&fixture, &error) == FSIMG_EIO);
	fixture.request.flags = FSIMG_JOB_SKIP_UNSUPPORTED;
	status = run(&fixture, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "omitted extraction: %d: %s\n", status, error.message);
	CHECK(status == FSIMG_OK && fixture.result.omitted_entries == 1);
	CHECK(fixture.result.skipped_entries == 0);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "fake", 0) == 0);
	fixture_close(&fixture);
}

static void
test_forged_publication(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	int fd;

	fixture_open(&fixture);
	for (worker_scenario_t mode = SCENARIO_FORGED_PUBLISHED; mode <= SCENARIO_ACK_ERROR; mode++) {
		fd = openat(fixture.parent, "fake", O_CREAT | O_EXCL | O_RDWR, 0700);
		CHECK(fd != -1);
		CHECK(ftruncate(fd, mode) == 0);
		CHECK(fchmod(fd, 0700) == 0);
		CHECK(close(fd) == 0);
		fd = openat(fixture.parent, "fake", O_RDONLY | O_CLOEXEC);
		CHECK(fd != -1);
		fixture.request.input_fd = fd;
		CHECK(fsimg_input_prepare(fixture.request.input_fd, NULL, NULL,
		    &fixture.request.input, NULL) == FSIMG_OK);
		fixture.request.sandbox = FSIMG_SANDBOX_OFF;
		CHECK(run(&fixture, &error) == (mode == SCENARIO_ACK_ERROR ? FSIMG_ECORRUPT : FSIMG_EIO));
		CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
		CHECK(close(fd) == 0);
		CHECK(unlinkat(fixture.parent, "fake", 0) == 0);
	}
	fixture_close(&fixture);
}

static void
test_forged(void)
{
	const worker_scenario_t cases[] = {
		SCENARIO_DONE_BEFORE_READY, SCENARIO_DONE_BEFORE_INFO,
		SCENARIO_BAD_EXIT, SCENARIO_DUPLICATE_DONE, SCENARIO_NAME_WITHOUT_ENTRY,
		SCENARIO_TRUNCATED_HEADER, SCENARIO_MISSING_DONE,
		SCENARIO_STALLED_PARSER, SCENARIO_LONG_LIST_NAME,
		SCENARIO_INVALID_TIME_BASIS, SCENARIO_VERIFY_INVALID_SCOPE,
		SCENARIO_VERIFY_HEADER_SCOPE, SCENARIO_VERIFY_UNREQUESTED_PAYLOAD,
		SCENARIO_VERIFY_MISSING_PAYLOAD, SCENARIO_RUNTIME_FAILURE,
		SCENARIO_INFO_WITHOUT_IMAGE, SCENARIO_IMAGE_WRONG_DISK,
		SCENARIO_PARTITION_OUTSIDE, SCENARIO_PARTITION_UNSELECTED_LIST,
		SCENARIO_VHD_WHOLE_FILE, SCENARIO_EXCESS_LOSS, SCENARIO_LIMIT_LOSS
#ifdef FSIMG_TEST_ASAN
		, SCENARIO_ASAN_FAILURE
#endif
	};
	worker_fixture_t fixture;
	fsimg_error_t error;
	fsimg_status_t status, expected;
	worker_scenario_t mode;
	int fd;

	for (size_t scenario = 0; scenario < sizeof(cases) / sizeof(cases[0]); scenario++) {
		mode = cases[scenario];
		fixture_open(&fixture);
		fd = openat(fixture.parent, "fake", O_CREAT | O_EXCL | O_RDWR, 0700);
		CHECK(fd != -1);
		CHECK(ftruncate(fd, mode) == 0);
		CHECK(fchmod(fd, 0700) == 0);
		CHECK(close(fd) == 0);
		fd = openat(fixture.parent, "fake", O_RDONLY | O_CLOEXEC);
		CHECK(fd != -1);
		fixture.request.input_fd = fd;
		CHECK(fsimg_input_prepare(fixture.request.input_fd, NULL, NULL,
		    &fixture.request.input, NULL) == FSIMG_OK);
		fixture.request.sandbox = FSIMG_SANDBOX_OFF;
		fixture.request.kind = FSIMG_JOB_INFO;
		fixture.request.seconds = 5;
		expected = FSIMG_EIO;
		if (mode == SCENARIO_LONG_LIST_NAME) {
			fixture.request.kind = FSIMG_JOB_LIST;
			expected = FSIMG_OK;
		}
		if (mode == SCENARIO_LIMIT_LOSS)
			expected = FSIMG_OK;
		/* Only i may report a disk without a selected volume. */
		if (mode == SCENARIO_PARTITION_UNSELECTED_LIST)
			fixture.request.kind = FSIMG_JOB_LIST;
		if (mode >= SCENARIO_VERIFY_INVALID_SCOPE && mode <= SCENARIO_VERIFY_MISSING_PAYLOAD)
			fixture.request.kind = FSIMG_JOB_VERIFY;
		if (mode == SCENARIO_VERIFY_MISSING_PAYLOAD)
			fixture.request.flags = FSIMG_JOB_PAYLOAD;
		if (mode == SCENARIO_STALLED_PARSER) {
			fixture.request.seconds = 1;
			expected = FSIMG_ECANCELLED;
		}
		if (mode == SCENARIO_RUNTIME_FAILURE || mode == SCENARIO_ASAN_FAILURE)
			expected = FSIMG_ESTATE;

		status = run(&fixture, &error);
		if (status != expected)
			fprintf(stderr, "forged worker scenario %d: expected %d, received %d: %s\n",
			    mode, expected, status, error.message);
		CHECK(status == expected);
		if (mode == SCENARIO_LONG_LIST_NAME)
			CHECK(fixture.name_bytes == 65535);
		if (mode == SCENARIO_EXCESS_LOSS)
			CHECK(strstr(error.message, "metadata loss examples") != NULL);
		CHECK(close(fd) == 0);
		CHECK(unlinkat(fixture.parent, "fake", 0) == 0);
		fixture_close(&fixture);
	}
}

/* A separate process group bounds regressions that deliberately stop draining
 * a pipe. Killing that group also reaps the fixture's worker on a broken tree.
 */
static void
test_transport(worker_scenario_t mode)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	fsimg_writer_t *writer;
	fsimg_entry_t entry;
	fsimg_source_t zero;
	fsimg_io_t *image;
	fsimg_status_t status;
	struct stat st;
	char *selection;
	const char *selected_member[1];
	pid_t child, waited, consumer;
	uint64_t padding;
	int fd, output, channels[2], outcome;
	bool finished;

	fixture_open(&fixture);
	fd = -1;
	channels[0] = channels[1] = -1;
	selection = NULL;
	selected_member[0] = "/";
	fixture.request.members = selected_member;
	fixture.request.member_count = 1;
	padding = 0;
	if (mode == SCENARIO_STARTUP_NONREADER || mode == SCENARIO_PREMATURE_RAW || mode == SCENARIO_STARTUP_ERROR) {
		fd = openat(fixture.parent, "fake", O_CREAT | O_EXCL | O_RDWR, 0700);
		CHECK(fd != -1 && ftruncate(fd, mode) == 0 && fchmod(fd, 0700) == 0);
		CHECK(close(fd) == 0);
		fd = openat(fixture.parent, "fake", O_RDONLY | O_CLOEXEC);
		CHECK(fd != -1);
		fixture.request.input_fd = fd;
		CHECK(fsimg_input_prepare(fixture.request.input_fd, NULL, NULL,
		    &fixture.request.input, NULL) == FSIMG_OK);
		fixture.request.sandbox = FSIMG_SANDBOX_OFF;
	}
	if (mode == SCENARIO_STARTUP_NONREADER || mode == SCENARIO_STARTUP_ERROR) {
		selection = malloc(65536);
		CHECK(selection != NULL);
		memset(selection, 'N', 65535);
		selection[0] = '/';
		selection[65535] = '\0';
		selected_member[0] = selection;
	}
	if (mode == SCENARIO_BLOCKED_RAW) {
		CHECK(fsimg_writer_new(fixture.ctx, "fat16", NULL, &writer, NULL) == FSIMG_OK);
		bzero(&entry, sizeof(entry));
		bzero(&zero, sizeof(zero));
		entry.path = "/BIG";
		entry.kind = FSIMG_ENTRY_FILE;
		entry.size = 1024 * 1024;
		zero.kind = FSIMG_SOURCE_ZERO;
		CHECK(fsimg_writer_add(writer, &entry, &zero, NULL) == FSIMG_OK);
		fd = openat(fixture.parent, "large", O_RDWR | O_CREAT | O_EXCL, 0600);
		CHECK(fd != -1);
		CHECK(fsimg_io_from_fd(fixture.ctx, fd, FSIMG_IO_WRITE, &image, NULL) == FSIMG_OK);
		CHECK(fsimg_writer_emit(writer, image, NULL) == FSIMG_OK);
		CHECK(fsimg_io_close_output(image, NULL) == FSIMG_OK);
		CHECK(close(fd) == 0);
		fsimg_writer_free(writer);
		fd = openat(fixture.parent, "large", O_RDONLY);
		CHECK(fd != -1);
		fixture.request.input_fd = fd;
		CHECK(fsimg_input_prepare(fixture.request.input_fd, NULL, NULL,
		    &fixture.request.input, NULL) == FSIMG_OK);
		selected_member[0] = "/BIG";
		CHECK(pipe(channels) == 0);
		output = channels[1];
	} else if (mode == SCENARIO_BROKEN_CONSUMER) {
		CHECK(pipe(channels) == 0);
		CHECK(close(channels[0]) == 0);
		channels[0] = -1;
		output = channels[1];
		selected_member[0] = "/HELLO.TXT";
	} else if (mode == SCENARIO_BLOCKED_WARNING || mode == SCENARIO_SLOW_CONSUMER) {
		CHECK(pipe(channels) == 0);
		output = channels[1];
		padding = fill_pipe(output);
		selected_member[0] = mode == SCENARIO_BLOCKED_WARNING ? "/" : "/HELLO.TXT";
		fixture.request.sandbox = FSIMG_SANDBOX_OFF;
	} else {
		output = openat(fixture.parent, "raw", O_CREAT | O_EXCL | O_RDWR, 0600);
		CHECK(output != -1);
		if (mode == SCENARIO_STALLED_RELAY) {
			CHECK(fchmod(output, 0700) == 0);
			selected_member[0] = "/HELLO.TXT";
		}
	}
	fixture.request.kind = mode == SCENARIO_STARTUP_NONREADER || mode == SCENARIO_STARTUP_ERROR || mode == SCENARIO_BLOCKED_WARNING ?
	    FSIMG_JOB_INFO : FSIMG_JOB_CAT;
	fixture.request.seconds = mode == SCENARIO_STALLED_RELAY || mode == SCENARIO_SLOW_CONSUMER ? 0 : 1;
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		CHECK(setpgid(0, 0) == 0);
		consumer = -1;
		if (mode == SCENARIO_BLOCKED_WARNING)
			CHECK(dup2(output, STDERR_FILENO) == STDERR_FILENO);
		if (mode == SCENARIO_SLOW_CONSUMER) {
			consumer = fork();
			CHECK(consumer != -1);
			if (consumer == 0) {
				drain_after_delay(channels[0], padding);
				_exit(0);
			}
		}
		for (unsigned attempt = 0; attempt < (mode == SCENARIO_STARTUP_ERROR ? 8u : 1u); attempt++) {
			status = fsimg_worker_run(fixture.executable, &fixture.request, event,
			    &fixture, output, NULL, &fixture.result, &error);
			if (mode == SCENARIO_STARTUP_ERROR) {
				CHECK(status == FSIMG_EINVAL);
				CHECK(strcmp(error.message, "startup test rejection") == 0);
			}
		}
		if (mode == SCENARIO_PREMATURE_RAW) {
			CHECK(status == FSIMG_EIO);
			CHECK(fstat(output, &st) == 0 && st.st_size == 0);
		} else if (mode == SCENARIO_BROKEN_CONSUMER) {
			CHECK(status == FSIMG_EIO && error.os_error == EPIPE);
			CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		} else if (mode == SCENARIO_STALLED_RELAY) {
			CHECK(status == FSIMG_EIO);
		} else if (mode == SCENARIO_SLOW_CONSUMER) {
			CHECK(status == FSIMG_OK && fixture.result.raw_bytes == 7);
			CHECK(waitpid(consumer, &outcome, 0) == consumer);
			CHECK(WIFEXITED(outcome) && WEXITSTATUS(outcome) == 0);
		} else if (mode != SCENARIO_STARTUP_ERROR) {
			CHECK(status == FSIMG_ECANCELLED);
			if (mode == SCENARIO_BLOCKED_RAW)
				CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
			if (mode == SCENARIO_BLOCKED_WARNING)
				CHECK(!fixture.ready && fixture.info == 0);
		}
		_exit(0);
	}
	CHECK(setpgid(child, child) == 0 || errno == EACCES);
	finished = false;
	for (unsigned i = 0; i < 400; i++) {
		waited = waitpid(child, &outcome, WNOHANG);
		CHECK(waited != -1);
		if (waited == child) {
			finished = true;
			break;
		}
		poll(NULL, 0, 20);
	}
	if (!finished) {
		CHECK(kill(-child, SIGKILL) == 0);
		CHECK(waitpid(child, &outcome, 0) == child);
	}
	/* Cleanup precedes assertions so a failing regression leaves no fixtures. */
	CHECK(close(output) == 0);
	if (channels[0] != -1)
		CHECK(close(channels[0]) == 0);
	else if (mode != SCENARIO_BLOCKED_RAW && mode != SCENARIO_BROKEN_CONSUMER)
		CHECK(unlinkat(fixture.parent, "raw", 0) == 0);
	if (fd != -1)
		CHECK(close(fd) == 0);
	if (mode != SCENARIO_BROKEN_CONSUMER && mode != SCENARIO_STALLED_RELAY && mode != SCENARIO_BLOCKED_WARNING && mode != SCENARIO_SLOW_CONSUMER)
		CHECK(unlinkat(fixture.parent, mode == SCENARIO_BLOCKED_RAW ? "large" : "fake", 0) == 0);
	free(selection);
	fixture_close(&fixture);
	CHECK(finished && WIFEXITED(outcome) && WEXITSTATUS(outcome) == 0);
}

/* Consume complete relay records, acknowledge their counts, then deliberately
 * keep the process alive after its valid final record and status EOF.
 */
static int
fake_output(void)
{
	unsigned char header[16], buffer[4096], report[32];
	uint64_t confirmed[2] = { 0, 0 }, stream, remaining;
	size_t offset, batch;
	ssize_t count;

	for (;;) {
		offset = 0;
		while (offset < sizeof(header)) {
			count = read(3, header + offset, sizeof(header) - offset);
			CHECK(count >= 0);
			if (count == 0) {
				CHECK(offset == 0);
				goto complete;
			}
			offset += (size_t)count;
		}
		stream = fsimg_load_le64(header);
		remaining = fsimg_load_le64(header + 8);
		CHECK(stream == 1 || stream == 2);
		while (remaining != 0) {
			batch = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
			count = read(3, buffer, batch);
			CHECK(count > 0);
			confirmed[stream - 1] += (uint64_t)count;
			remaining -= (uint64_t)count;
		}
		bzero(report, sizeof(report));
		fsimg_store_le64(report, confirmed[0]);
		fsimg_store_le64(report + 8, confirmed[1]);
		fsimg_store_le64(report + 24, 1);
		CHECK(write(4, report, sizeof(report)) == sizeof(report));
	}
complete:
	bzero(report, sizeof(report));
	fsimg_store_le64(report, confirmed[0]);
	fsimg_store_le64(report + 8, confirmed[1]);
	fsimg_store_le64(report + 24, 2);
	CHECK(write(4, report, sizeof(report)) == sizeof(report));
	CHECK(close(4) == 0);
	signal(SIGTERM, SIG_IGN);
	for (;;)
		pause();
}

int
main(int argc, char **argv)
{
	const struct rlimit no_core = { 0, 0 };
	struct stat st;

	if (argc == 2 && strcmp(argv[1], "--internal-output") == 0) {
		if (fstat(STDOUT_FILENO, &st) == 0 && (st.st_mode & 0100) != 0)
			return (fake_output());
		return (fsimg_output_main());
	}
	if (argc == 2 && strcmp(argv[1], "--internal-worker") == 0) {
		CHECK(setrlimit(RLIMIT_CORE, &no_core) == 0);
		CHECK(fcntl(200, F_GETFD) == -1 && errno == EBADF);
		if (names_worker_selected(&st))
			return (fake_names_worker((size_t)st.st_size));
		if (coordinator_worker_selected(&st))
			return (fake_coordinator_worker((size_t)st.st_size));
		if (fstat(FSIMG_CHILD_INPUT, &st) == 0 && (st.st_mode & 0100) != 0)
			return (fake_worker((worker_scenario_t)st.st_size));
		return (fsimg_worker_main());
	}
	if (argc == 2 && strcmp(argv[1], "--internal-input") == 0)
		return (fsimg_input_main());
	if (argc == 2 && strcmp(argv[1], "--output-prefix") == 0) {
		signal(SIGPIPE, SIG_IGN);
		test_output_ledger();
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--mount-source") == 0) {
		signal(SIGPIPE, SIG_IGN);
		test_mount_source(argv[2]);
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--verbose-names") == 0) {
		signal(SIGPIPE, SIG_IGN);
		test_verbose_names_protocol();
		return (0);
	}
	CHECK(argc == 1);

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer)
	puts("SKIP: leak scanning inside fresh children requires tracing unavailable under confinement");
#endif
#endif
	signal(SIGPIPE, SIG_IGN);
	if (getenv("FSIMG_TEST_TRANSPORT") != NULL) {
		test_transport((worker_scenario_t)atoi(getenv("FSIMG_TEST_TRANSPORT")));
		return (0);
	}
	test_operations("fat16");
	test_operations("exfat");
	test_list_case_alias_members_once();
	test_list_missing_member_names_error();
	test_list_directory_member_precedes_subtree();
	test_list_nested_directory_members_once();
	test_list_directory_case_alias_members_once();
	test_list_nested_directory_members_without_recursion_once();
	test_list_reversed_nested_directory_members_keep_order();
	test_list_directory_without_recursion_includes_target_and_children();
	test_list_deep_directory_includes_ancestor();
	test_list_root_and_no_members_have_no_root_entry();
	test_list_and_extract_directory_counts_and_manifest_match();
	test_partitioned_create();
	test_container_create();
	test_compressed_create();
	test_prompt_completion();
	test_text_output();
	test_startup();
	test_inheritance();
	test_verbose_names_protocol();
	test_forged();
	test_path_sequences();
	test_forged_publication();
	test_incomplete_extraction();
	test_skipped_extraction();
	test_omitted_extraction();
	test_output_ledger();
	{
		test_transport(SCENARIO_STARTUP_NONREADER);
		test_transport(SCENARIO_PREMATURE_RAW);
		test_transport(SCENARIO_BLOCKED_RAW);
		test_transport(SCENARIO_BROKEN_CONSUMER);
		test_transport(SCENARIO_STARTUP_ERROR);
		test_transport(SCENARIO_STALLED_RELAY);
		test_transport(SCENARIO_BLOCKED_WARNING);
		test_transport(SCENARIO_SLOW_CONSUMER);
	}
	test_coordinator_regressions();
	return (0);
}

/* Mode 0710 distinguishes these table indices from other worker markers. */
static bool
coordinator_worker_selected(struct stat *st)
{
	const int descriptors[] = { FSIMG_CHILD_INPUT, FSIMG_CHILD_SPOOL,
	    FSIMG_CHILD_SLOT(FSIMG_CAPABILITY_SETTING) };

	for (size_t i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]); i++) {
		if (fstat(descriptors[i], st) == 0 && S_ISREG(st->st_mode) &&
		    (st->st_mode & 0777) == 0710 &&
		    st->st_size >= 0 && (uint64_t)st->st_size <
		    sizeof(coordinator_cases) / sizeof(coordinator_cases[0]))
			return (true);
	}
	return (false);
}

/* Valid envelopes isolate the coordinator's job-specific validation. */
static int
fake_coordinator_worker(size_t case_index)
{
	const coordinator_case_t *test;
	fsimg_frame_t frame;
	unsigned char raw[FSIMG_FRAME_BYTES];

	CHECK(case_index < sizeof(coordinator_cases) / sizeof(coordinator_cases[0]));
	test = &coordinator_cases[case_index];
	CHECK(fsimg_frame_read(FSIMG_CHILD_JOB, &frame, NULL) == FSIMG_OK);
	CHECK(frame.type == FSIMG_FRAME_JOB);
	CHECK(fsimg_frame_read(FSIMG_CHILD_JOB, &frame, NULL) == FSIMG_OK);
	CHECK(frame.type == FSIMG_FRAME_SELECTION);
	if (test->stall_ignoring_sigterm)
		CHECK(signal(SIGTERM, SIG_IGN) != SIG_ERR);
	bzero(raw, sizeof(raw));
	fsimg_store_le64(raw, FSIMG_CONFINEMENT_DISABLED);
	CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_READY, raw,
	    8, NULL) == FSIMG_OK);
	CHECK(fsimg_frame_read(FSIMG_CHILD_JOB, &frame, NULL) == FSIMG_OK);
	CHECK(frame.type == FSIMG_FRAME_ACK);

	if (test->stall_ignoring_sigterm) {
		for (;;)
			pause();
	}
	if (test->exit_before_done) {
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_PROGRESS,
		    raw, 24, NULL) == FSIMG_OK);
		return (1);
	}
	if (test->send_image) {
		bzero(raw, FSIMG_WIRE_IMAGE_BYTES);
		fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_DISK, test->image_disk_bytes);
		fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FILE, test->image_file_bytes);
		if (test->image_scheme != NULL) {
			strcpy((char *)raw + FSIMG_WIRE_IMAGE_SCHEME, test->image_scheme);
			fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_SECTOR, 512);
			fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_SLOTS, 4);
			fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_SELECTED, 1);
		}
		if (test->image_container != NULL)
			strcpy((char *)raw + FSIMG_WIRE_IMAGE_CONTAINER,
			    test->image_container);
		if (test->image_compression != NULL)
			strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION,
			    test->image_compression);
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_IMAGE,
		    raw, FSIMG_WIRE_IMAGE_BYTES, NULL) == FSIMG_OK);
	}
	switch (test->job) {
	case COORDINATOR_READ_FILE:
	case COORDINATOR_READ_STREAM:
		bzero(raw, FSIMG_WIRE_INFO_BYTES);
		fsimg_store_le64(raw + 56, FSIMG_TIME_WALL);
		fsimg_store_le64(raw + 64, FSIMG_VALIDATION_HEADER);
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_INFO,
		    raw, FSIMG_WIRE_INFO_BYTES, NULL) == FSIMG_OK);
		break;
	case COORDINATOR_CREATE_STAGING:
	case COORDINATOR_CREATE_STREAM:
		bzero(raw, FSIMG_WIRE_PLAN_BYTES);
		fsimg_store_le64(raw + FSIMG_WIRE_PLAN_VOLUME, test->plan_volume_bytes);
		fsimg_store_le64(raw + FSIMG_WIRE_PLAN_OUTPUT, test->plan_output_bytes);
		fsimg_store_le64(raw + FSIMG_WIRE_PLAN_SECTOR, 512);
		fsimg_store_le64(raw + FSIMG_WIRE_PLAN_CLUSTER, 512);
		strcpy((char *)raw + FSIMG_WIRE_PLAN_FORMAT, "fat12");
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_PLAN,
		    raw, FSIMG_WIRE_PLAN_BYTES, NULL) == FSIMG_OK);
		break;
	}
	if (test->raw_bytes != 0) {
		CHECK(test->raw_bytes <= sizeof(raw));
		bzero(raw, (size_t)test->raw_bytes);
		CHECK(write(FSIMG_CHILD_DATA, raw, (size_t)test->raw_bytes) ==
		    (ssize_t)test->raw_bytes);
	}
	bzero(raw, FSIMG_WIRE_DONE_BYTES);
	fsimg_store_le64(raw + FSIMG_WIRE_DONE_IMAGE, test->done_image_bytes);
	CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_DONE,
	    raw, FSIMG_WIRE_DONE_BYTES, NULL) == FSIMG_OK);
	return (0);
}

/* Unlink markers immediately so a killed case cannot leave a forged input. */
static int
coordinator_marker(worker_fixture_t *fixture, size_t case_index,
    int flags)
{
	int fd;

	fd = openat(fixture->parent, "coordinator", O_CREAT | O_EXCL | O_RDWR,
	    0710);
	CHECK(fd != -1);
	CHECK(ftruncate(fd, (off_t)case_index) == 0);
	CHECK(fchmod(fd, 0710) == 0);
	CHECK(close(fd) == 0);
	fd = openat(fixture->parent, "coordinator", flags | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(unlinkat(fixture->parent, "coordinator", 0) == 0);
	return (fd);
}

static fsimg_status_t
coordinator_event(void *argument, const fsimg_frame_t *frame,
    fsimg_worker_text_t *text, fsimg_error_t *error)
{
	coordinator_event_context_t *context = argument;
	fsimg_status_t status;

	status = event(context->fixture, frame, text, error);
	if (status == FSIMG_OK && frame->type == FSIMG_FRAME_READY)
		context->ready_started = coordinator_real_milliseconds();
	if (status == FSIMG_OK && frame->type == FSIMG_FRAME_PROGRESS) {
		/* Startup must not consume the blocked-output interval. */
		if (context->drain_start != -1)
			CHECK(write(context->drain_start, "P", 1) == 1);
		*text = (fsimg_worker_text_t){ .data = "payload", .length = 7 };
	}
	return (status);
}

/* READY proves the parser has installed SIG_IGN before cleanup is tested. */
static fsimg_status_t
coordinator_clock_event(void *argument, const fsimg_frame_t *frame,
    fsimg_worker_text_t *text, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = coordinator_event(argument, frame, text, error);
	if (status == FSIMG_OK && frame->type == FSIMG_FRAME_READY) {
		coordinator_clock_calls = 0;
		coordinator_clock_fail_from = 1;
	}
	return (status);
}

/* Drain until EOF; a rejected stream is allowed to forward no bytes. */
static void
coordinator_drain(int fd, unsigned delay)
{
	unsigned char buffer[4096];
	ssize_t count;

	CHECK(poll(NULL, 0, (int)delay) == 0);
	for (;;) {
		count = read(fd, buffer, sizeof(buffer));
		if (count == -1 && errno == EINTR)
			continue;
		CHECK(count >= 0);
		if (count == 0)
			return;
	}
}

/* The case process owns stdin replacement, fault arming and its consumer. */
static void
coordinator_run_case(worker_fixture_t *fixture, const coordinator_case_t *test,
    int input, int output[2])
{
	coordinator_event_context_t context = { .fixture = fixture,
	    .drain_start = -1 };
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	uint64_t started, elapsed;
	pid_t consumer, remaining;
	int saved_input, outcome, child_errno, drain_start[2] = { -1, -1 };
	char marker;
	bool valid, message_matches;

	saved_input = -1;
	consumer = -1;
	if (input != -1) {
		saved_input = dup(STDIN_FILENO);
		CHECK(saved_input != -1);
		CHECK(dup2(input, STDIN_FILENO) == STDIN_FILENO);
		if (output[1] != -1)
			CHECK(pipe(drain_start) == 0);
	}
	if (output[1] != -1) {
		consumer = fork();
		CHECK(consumer != -1);
		if (consumer == 0) {
			CHECK(close(output[1]) == 0);
			if (input != -1) {
				CHECK(close(drain_start[1]) == 0);
				CHECK(read(drain_start[0], &marker, 1) == 1);
				CHECK(close(drain_start[0]) == 0);
			}
			coordinator_drain(output[0], input != -1 ? 3000 : 0);
			_exit(0);
		}
		CHECK(close(output[0]) == 0);
	}
	if (drain_start[0] != -1) {
		CHECK(close(drain_start[0]) == 0);
		context.drain_start = drain_start[1];
	}

	started = coordinator_real_milliseconds();
	coordinator_clock_calls = 0;
	coordinator_clock_fault_started = 0;
	switch (test->clock_fault) {
	case COORDINATOR_CLOCK_NONE:
	case COORDINATOR_CLOCK_AT_READY:
		break;
	case COORDINATOR_CLOCK_FROM_FIRST:
		coordinator_clock_fail_from = 1;
		break;
	case COORDINATOR_CLOCK_FROM_SECOND:
		coordinator_clock_fail_from = 2;
		break;
	}
	status = fsimg_worker_run(fixture->executable, &fixture->request,
	    test->clock_fault == COORDINATOR_CLOCK_AT_READY ?
	    coordinator_clock_event : coordinator_event, &context, output[1],
	    NULL, &fixture->result, &error);
	if (test->clock_fault == COORDINATOR_CLOCK_AT_READY ||
	    test->scenario == COORDINATOR_SUCCESS_BLOCKED_INPUT)
		started = context.ready_started;
	else if (test->clock_fault == COORDINATOR_CLOCK_FROM_SECOND)
		started = coordinator_clock_fault_started;
	elapsed = coordinator_real_milliseconds() - started;
	coordinator_clock_fail_from = 0;

	if (context.drain_start != -1)
		CHECK(close(context.drain_start) == 0);
	if (saved_input != -1) {
		CHECK(dup2(saved_input, STDIN_FILENO) == STDIN_FILENO);
		CHECK(close(saved_input) == 0);
	}
	if (consumer != -1) {
		CHECK(close(output[1]) == 0);
		CHECK(waitpid(consumer, &outcome, 0) == consumer);
		CHECK(WIFEXITED(outcome) && WEXITSTATUS(outcome) == 0);
	}
	/* No parser, input relay or output relay may remain unreaped. */
	remaining = waitpid(-1, &outcome, WNOHANG);
	child_errno = errno;
	message_matches = test->message_contains ?
	    strstr(error.message, test->message) != NULL :
	    strcmp(error.message, test->message) == 0;
	valid = status == test->status && error.status == test->status &&
	    message_matches && elapsed >= test->minimum_ms &&
	    elapsed < test->maximum_ms && remaining == -1 && child_errno == ECHILD;
	if (test->clock_fault == COORDINATOR_CLOCK_AT_READY)
		valid = valid && fixture->ready;
	if (test->clock_fault == COORDINATOR_CLOCK_FROM_SECOND)
		valid = valid && coordinator_clock_fault_started != 0;
	if (test->scenario == COORDINATOR_SUCCESS_BLOCKED_INPUT)
		valid = valid && fixture->ready && fixture->images == 1 &&
		    fixture->info == 1;
	if (status == FSIMG_OK) {
		valid = valid && fixture->ready &&
		    fixture->images == (test->send_image ? 1u : 0u);
		switch (test->job) {
		case COORDINATOR_READ_FILE:
		case COORDINATOR_READ_STREAM:
			valid = valid && fixture->info == 1;
			break;
		case COORDINATOR_CREATE_STAGING:
		case COORDINATOR_CREATE_STREAM:
			valid = valid && fixture->plans == 1 &&
			    fixture->result.image_bytes == test->done_image_bytes &&
			    fixture->result.raw_bytes == test->raw_bytes;
			break;
		}
	}
	if (!valid)
		fprintf(stderr, "%s: status %d, message \"%s\", elapsed %llu ms; "
		    "expected status %d, message \"%s\", %llu <= elapsed < %llu ms; "
		    "remaining child %ld, errno %d\n", test->name, status,
		    error.message, (unsigned long long)elapsed, test->status,
		    test->message, (unsigned long long)test->minimum_ms,
		    (unsigned long long)test->maximum_ms, (long)remaining, child_errno);
	CHECK(valid);
}

/* Every case has a process-group deadline, including failed cleanup paths.
 * The parent owns the fixture and removes it after killing any descendants.
 */
static unsigned
coordinator_test_case(const coordinator_case_t *test)
{
	worker_fixture_t fixture;
	uint64_t started, deadline;
	size_t case_index;
	pid_t child, waited;
	int marker, fd, input[2] = { -1, -1 }, output[2] = { -1, -1 }, outcome;
	bool finished;

	case_index = (size_t)(test - coordinator_cases);
	CHECK(case_index < sizeof(coordinator_cases) / sizeof(coordinator_cases[0]));
	fixture_open(&fixture);
	fixture.request.sandbox = FSIMG_SANDBOX_OFF;
	fixture.request.seconds = 5;
	if (test->spool_limit != 0)
		fixture.request.limits.spool_bytes = test->spool_limit;
	marker = coordinator_marker(&fixture, case_index, O_RDWR);
	switch (test->job) {
	case COORDINATOR_READ_STREAM:
		fixture.request.input_fd = -1;
		fixture.request.input = (fsimg_input_t){ .kind = FSIMG_INPUT_STREAM };
		fixture.request.spool_fd = marker;
		fixture.request.seconds = 0;
		CHECK(pipe(input) == 0);
		if (test->exit_before_done) {
			CHECK(pipe(output) == 0);
			CHECK(fill_pipe(output[1]) != 0);
		}
		break;
	case COORDINATOR_READ_FILE:
		fixture.request.spool_fd = marker;
		if (test->send_image) {
			/* Keep the input length independent of the marker index. */
			fd = openat(fixture.parent, "image", O_WRONLY);
			CHECK(fd != -1);
			CHECK(ftruncate(fd, (off_t)test->image_file_bytes) == 0);
			CHECK(close(fd) == 0);
			CHECK(fsimg_input_prepare(fixture.input, NULL, NULL,
			    &fixture.request.input, NULL) == FSIMG_OK);
		}
		break;
	case COORDINATOR_CREATE_STAGING:
	case COORDINATOR_CREATE_STREAM:
		create_request(&fixture, "fat12");
		fixture.request.setting_fds[0] = marker;
		if (test->requested_compression != NULL ||
		    test->job == COORDINATOR_CREATE_STREAM)
			fixture.request.raw_fd = raw_intermediate(&fixture);
		if (test->job == COORDINATOR_CREATE_STREAM) {
			fixture.request.flags |= FSIMG_JOB_STDOUT;
			fixture.request.output_fd = -1;
			/* Let the plan, rather than the output limit, reject excess. */
			fixture.request.limits.max_output_bytes = 8192;
			CHECK(pipe(output) == 0);
		}
		break;
	}
	if (test->requested_stack != NULL)
		CHECK(fsimg_stack_parse(test->requested_stack, &fixture.request.stack,
		    NULL) == FSIMG_OK);
	if (test->requested_compression != NULL)
		strcpy(fixture.request.compression, test->requested_compression);

	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		CHECK(setpgid(0, 0) == 0);
		coordinator_run_case(&fixture, test, input[0], output);
		_exit(0);
	}
	CHECK(setpgid(child, child) == 0 || errno == EACCES);
	if (output[1] != -1) {
		CHECK(close(output[1]) == 0);
		output[1] = -1;
	}
	started = coordinator_real_milliseconds();
	deadline = started + test->deadline_ms;
	finished = false;
	while (coordinator_real_milliseconds() < deadline) {
		waited = waitpid(child, &outcome, WNOHANG);
		CHECK(waited != -1);
		if (waited == child) {
			finished = true;
			break;
		}
		poll(NULL, 0, 20);
	}
	CHECK(kill(-child, SIGKILL) == 0 || errno == ESRCH);
	if (!finished)
		CHECK(waitpid(child, &outcome, 0) == child);

	CHECK(close(marker) == 0);
	if (fixture.request.spool_fd != -1 && fixture.request.spool_fd != marker)
		CHECK(close(fixture.request.spool_fd) == 0);
	if (fixture.request.raw_fd != -1)
		CHECK(close(fixture.request.raw_fd) == 0);
	for (unsigned i = 0; i < 2; i++) {
		if (input[i] != -1)
			CHECK(close(input[i]) == 0);
		if (output[i] != -1)
			CHECK(close(output[i]) == 0);
	}
	fixture_close(&fixture);
	if (!finished || !WIFEXITED(outcome) || WEXITSTATUS(outcome) != 0)
		fprintf(stderr, "%s: case process status %d, message \"%s\", "
		    "elapsed %llu ms\n", test->name, outcome,
		    finished ? "case assertion failed" : "process-group deadline exceeded",
		    (unsigned long long)(coordinator_real_milliseconds() - started));
	return (!finished || !WIFEXITED(outcome) || WEXITSTATUS(outcome) != 0);
}

/* Exact scenario names select tests without assigning meaning to enum order. */
static unsigned
coordinator_test_scenario(coordinator_scenario_t scenario)
{
	for (size_t i = 0; i < sizeof(coordinator_cases) / sizeof(coordinator_cases[0]); i++) {
		if (coordinator_cases[i].scenario == scenario)
			return (coordinator_test_case(&coordinator_cases[i]));
	}
	CHECK(false);
	return (1);
}

/* Forced read slots and compression constrain the worker's IMAGE record. */
static unsigned
coordinator_test_read_stack(void)
{
	unsigned failures = 0;

	failures += coordinator_test_scenario(COORDINATOR_READ_FORBIDDEN_CONTAINER);
	failures += coordinator_test_scenario(COORDINATOR_READ_FORBIDDEN_SCHEME);
	failures += coordinator_test_scenario(COORDINATOR_READ_MISSING_CONTAINER);
	failures += coordinator_test_scenario(COORDINATOR_READ_MISSING_COMPRESSION);
	failures += coordinator_test_scenario(COORDINATOR_READ_BARE_STACK);
	failures += coordinator_test_scenario(COORDINATOR_READ_VHD_STACK);
	return (failures);
}

/* Parser failure must survive backpressure while stdin remains open. */
static unsigned
test_worker_exit_with_blocked_input(void)
{
	return (coordinator_test_scenario(COORDINATOR_EXIT_BLOCKED_INPUT));
}

/* Failed clock readings cannot create an unbounded wait or cleanup grace. */
static unsigned
test_monotonic_clock_failures(void)
{
	unsigned failures = 0;

	failures += coordinator_test_scenario(COORDINATOR_CLOCK_FIRST_FAILURE);
	failures += coordinator_test_scenario(COORDINATOR_CLOCK_LATER_FAILURE);
	failures += coordinator_test_scenario(COORDINATOR_CLOCK_STOP_FAILURE);
	return (failures);
}

/* A container decoded into scratch fits the spool at its exact boundary. */
static unsigned
test_spooled_container_disk_limit(void)
{
	unsigned failures = 0;

	failures += coordinator_test_scenario(COORDINATOR_SPOOLED_DISK_AT_LIMIT);
	failures += coordinator_test_scenario(COORDINATOR_SPOOLED_DISK_OVER_LIMIT);
	return (failures);
}

/* An encoding's plan allows strictly more bytes than its underlying disk. */
static unsigned
test_forged_creation_plan(void)
{
	unsigned failures = 0;

	failures += coordinator_test_scenario(COORDINATOR_ENCODED_PLAN_BELOW_DISK);
	failures += coordinator_test_scenario(COORDINATOR_ENCODED_PLAN_AT_DISK);
	failures += coordinator_test_scenario(COORDINATOR_ENCODED_PLAN_ABOVE_DISK);
	return (failures);
}

/* A creation's IMAGE compression must match its job's requested encoding. */
static unsigned
test_forged_creation_compression(void)
{
	unsigned failures = 0;

	failures += coordinator_test_scenario(COORDINATOR_COMPRESSION_MISMATCH);
	failures += coordinator_test_scenario(COORDINATOR_STAGED_ENCODED_BELOW_PLAN);
	return (failures);
}

/* Staged raw lengths are exact; encoded lengths are positive and bounded. */
static unsigned
test_forged_creation_staged_length(void)
{
	unsigned failures = 0;

	failures += coordinator_test_scenario(COORDINATOR_STAGED_RAW_SHORT);
	failures += coordinator_test_scenario(COORDINATOR_STAGED_RAW_LONG);
	failures += coordinator_test_scenario(COORDINATOR_STAGED_RAW_EXACT);
	failures += coordinator_test_scenario(COORDINATOR_STAGED_ENCODED_OVER_PLAN);
	failures += coordinator_test_scenario(COORDINATOR_STAGED_ENCODED_ZERO);
	failures += coordinator_test_scenario(COORDINATOR_STAGED_ENCODED_AT_PLAN);
	failures += coordinator_test_scenario(COORDINATOR_STAGED_ENCODED_BELOW_PLAN);
	return (failures);
}

/* The coordinator counts streamed raw bytes independently of DONE. */
static unsigned
test_forged_creation_stream_length(void)
{
	unsigned failures = 0;

	failures += coordinator_test_scenario(COORDINATOR_STREAM_OVER_PLAN);
	failures += coordinator_test_scenario(COORDINATOR_STREAM_SHORT);
	failures += coordinator_test_scenario(COORDINATOR_STREAM_EXACT);
	return (failures);
}

/* Collect independent failures so one regression cannot hide another. */
static void
test_coordinator_regressions(void)
{
	unsigned failures = 0;

	failures += coordinator_test_read_stack();
	failures += coordinator_test_scenario(COORDINATOR_SUCCESS_BLOCKED_INPUT);
	failures += test_forged_creation_plan();
	failures += test_forged_creation_compression();
	failures += test_forged_creation_staged_length();
	failures += test_forged_creation_stream_length();
	failures += test_worker_exit_with_blocked_input();
	failures += test_monotonic_clock_failures();
	failures += test_spooled_container_disk_limit();
	CHECK(failures == 0);
}

/* Everything after this point must stay in the real-clock section: only
 * the clock hook and its real-clock helper belong here. The command-line
 * macro also renamed the system header's declaration, so redeclare the
 * real clock_gettime after undefining it to bypass faults for supervision.
 */
#undef clock_gettime

int clock_gettime(clockid_t, struct timespec *);

static uint64_t
coordinator_real_milliseconds(void)
{
	struct timespec time;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
	return ((uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000);
}

/* Fault arming belongs to the case process; re-executed children start clear. */
int
fsimg_test_clock_gettime(clockid_t clock, struct timespec *time)
{
	if (clock == CLOCK_MONOTONIC && coordinator_clock_fail_from != 0 &&
	    ++coordinator_clock_calls >= coordinator_clock_fail_from) {
		/* Measure cleanup from fault activation rather than test setup. */
		if (coordinator_clock_fault_started == 0)
			coordinator_clock_fault_started = coordinator_real_milliseconds();
		errno = EINVAL;
		return (-1);
	}
	return (clock_gettime(clock, time));
}

/* A mode distinct from the other forged-worker markers selects this table. */
static bool
names_worker_selected(struct stat *st)
{
	const int descriptors[] = { FSIMG_CHILD_SPOOL,
	    FSIMG_CHILD_SLOT(FSIMG_CAPABILITY_SETTING) };

	for (size_t i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]); i++) {
		if (fstat(descriptors[i], st) == 0 && S_ISREG(st->st_mode) &&
		    (st->st_mode & 0777) == 0720 && st->st_size >= 0 &&
		    (uint64_t)st->st_size < sizeof(names_cases) / sizeof(names_cases[0]))
			return (true);
	}
	return (false);
}

/* All envelopes and names are valid; only their position/count is forged.
 * A rejected frame may close the channel before the rest of the script sends.
 */
static bool
send_forged_names(const names_case_t *test)
{
	static const fsimg_entry_kind_t kinds[] = { FSIMG_ENTRY_FILE,
	    FSIMG_ENTRY_DIRECTORY, FSIMG_ENTRY_SYMLINK, FSIMG_ENTRY_FIFO };
	unsigned char raw[FSIMG_WIRE_ENTRY_BYTES];
	char name[] = "/a";

	CHECK(test->names <= sizeof(kinds) / sizeof(kinds[0]));
	for (size_t i = 0; i < test->names; i++) {
		bzero(raw, sizeof(raw));
		fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_KIND, kinds[i]);
		fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_NAME_LENGTH, 2);
		if (kinds[i] == FSIMG_ENTRY_SYMLINK)
			fsimg_store_le64(raw + FSIMG_WIRE_ENTRY_LENGTH, 1);
		if (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_ENTRY,
		    raw, sizeof(raw), NULL) != FSIMG_OK)
			return (false);
		name[1] = (char)('a' + i);
		if (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_NAME,
		    name, 2, NULL) != FSIMG_OK)
			return (false);
		if (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_ENTRY_END,
		    NULL, 0, NULL) != FSIMG_OK)
			return (false);
	}
	return (true);
}

static int
fake_names_worker(size_t index)
{
	const names_case_t *test = &names_cases[index];
	fsimg_frame_t frame;
	unsigned char raw[FSIMG_FRAME_BYTES];
	int fd;

	CHECK(fsimg_frame_read(FSIMG_CHILD_JOB, &frame, NULL) == FSIMG_OK);
	CHECK(frame.type == FSIMG_FRAME_JOB);
	CHECK(fsimg_frame_read(FSIMG_CHILD_JOB, &frame, NULL) == FSIMG_OK);
	CHECK(frame.type == FSIMG_FRAME_SELECTION);
	bzero(raw, sizeof(raw));
	fsimg_store_le64(raw, FSIMG_CONFINEMENT_DISABLED);
	CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_READY, raw, 8,
	    NULL) == FSIMG_OK);
	CHECK(fsimg_frame_read(FSIMG_CHILD_JOB, &frame, NULL) == FSIMG_OK);
	CHECK(frame.type == FSIMG_FRAME_ACK);

	if (test->position == NAMES_CHUNK_WITHOUT_ENTRY) {
		fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_NAME, "/a", 2, NULL);
		return (0);
	}
	if (test->position == NAMES_BEFORE_HEADER && !send_forged_names(test))
		return (0);
	if (test->kind == FSIMG_JOB_EXTRACT || test->encoded) {
		bzero(raw, FSIMG_WIRE_IMAGE_BYTES);
		fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_DISK, 512);
		fsimg_store_le64(raw + FSIMG_WIRE_IMAGE_FILE, 512);
		if (test->encoded)
			strcpy((char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION, "gzip");
		if (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_IMAGE, raw,
		    FSIMG_WIRE_IMAGE_BYTES, NULL) != FSIMG_OK)
			return (0);
	}
	if (test->position == NAMES_AFTER_IMAGE && !send_forged_names(test))
		return (0);
	if (test->kind == FSIMG_JOB_EXTRACT) {
		bzero(raw, FSIMG_WIRE_INFO_BYTES);
		fsimg_store_le64(raw + 56, FSIMG_TIME_WALL);
		fsimg_store_le64(raw + 64, FSIMG_VALIDATION_HEADER);
		if (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_INFO, raw,
		    FSIMG_WIRE_INFO_BYTES, NULL) != FSIMG_OK)
			return (0);
		if (test->position == NAMES_AFTER_INFO && !send_forged_names(test))
			return (0);
	} else {
		bzero(raw, FSIMG_WIRE_PLAN_BYTES);
		fsimg_store_le64(raw + FSIMG_WIRE_PLAN_VOLUME, 512);
		fsimg_store_le64(raw + FSIMG_WIRE_PLAN_OUTPUT, test->encoded ? 1024 : 512);
		fsimg_store_le64(raw + FSIMG_WIRE_PLAN_SECTOR, 512);
		fsimg_store_le64(raw + FSIMG_WIRE_PLAN_CLUSTER, 512);
		fsimg_store_le64(raw + FSIMG_WIRE_PLAN_ENTRIES, test->plan_entries);
		strcpy((char *)raw + FSIMG_WIRE_PLAN_FORMAT, "fat12");
		if (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_PLAN, raw,
		    FSIMG_WIRE_PLAN_BYTES, NULL) != FSIMG_OK)
			return (0);
		if (test->position == NAMES_AFTER_PLAN && !send_forged_names(test))
			return (0);
	}
	/* Only the successful creation writes staging. The coordinator owns
	 * publication; no rejected script creates even a staged image.
	 */
	if (test->message == NULL && test->kind == FSIMG_JOB_CREATE) {
		fd = openat(FSIMG_CHILD_OUTPUT, "image", O_CREAT | O_EXCL | O_WRONLY, 0600);
		CHECK(fd != -1);
		CHECK(ftruncate(fd, 512) == 0);
		CHECK(close(fd) == 0);
	}
	bzero(raw, FSIMG_WIRE_DONE_BYTES);
	fsimg_store_le64(raw + FSIMG_WIRE_DONE_ENTRIES, test->done_entries);
	fsimg_store_le64(raw + FSIMG_WIRE_DONE_SKIPPED, test->done_skipped);
	if (test->kind == FSIMG_JOB_CREATE)
		fsimg_store_le64(raw + FSIMG_WIRE_DONE_IMAGE, 512);
	fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_DONE, raw,
	    FSIMG_WIRE_DONE_BYTES, NULL);
	return (0);
}

static void
test_verbose_names_protocol(void)
{
	worker_fixture_t fixture;
	const names_case_t *test;
	fsimg_error_t error;
	fsimg_status_t status;
	int marker, fd;

	for (size_t i = 0; i < sizeof(names_cases) / sizeof(names_cases[0]); i++) {
		test = &names_cases[i];
		fixture_open(&fixture);
		marker = coordinator_marker(&fixture, i, O_RDWR);
		CHECK(fchmod(marker, 0720) == 0);
		if (test->kind == FSIMG_JOB_EXTRACT) {
			fd = openat(fixture.parent, "image", O_WRONLY);
			CHECK(fd != -1);
			CHECK(ftruncate(fd, 512) == 0);
			CHECK(close(fd) == 0);
			CHECK(fsimg_input_prepare(fixture.input, NULL, NULL,
			    &fixture.request.input, NULL) == FSIMG_OK);
			fixture.request.spool_fd = marker;
			fixture.request.output_fd = fixture.output;
		} else {
			create_request(&fixture, "fat12");
			fixture.request.setting_fds[0] = marker;
		}
		fixture.request.kind = test->kind;
		fixture.request.flags = test->flags;
		fixture.request.sandbox = FSIMG_SANDBOX_OFF;
		fixture.request.seconds = 5;
		if (test->encoded) {
			strcpy(fixture.request.compression, "gzip");
			fixture.request.raw_fd = raw_intermediate(&fixture);
		}
		if (test->kind == FSIMG_JOB_PLAN)
			fixture.request.output_fd = -1;

		status = run(&fixture, &error);
		if (status != (test->message == NULL ? FSIMG_OK : FSIMG_EIO) ||
		    (test->message != NULL && strcmp(error.message, test->message) != 0))
			fprintf(stderr, "%s: status %d, message %s\n",
			    test->name, status, error.message);
		CHECK(status == (test->message == NULL ? FSIMG_OK : FSIMG_EIO));
		if (test->message != NULL) {
			CHECK(strcmp(error.message, test->message) == 0);
			CHECK(error.output_state != FSIMG_OUTPUT_PUBLISHED);
			CHECK(error.output_state != FSIMG_OUTPUT_UNCERTAIN);
			CHECK(faccessat(fixture.output, "image", F_OK, 0) == -1 && errno == ENOENT);
		} else {
			CHECK(fixture.name_bytes == 2 * test->names);
			CHECK(fixture.result.entries == test->done_entries);
			CHECK(fixture.result.skipped_entries == test->done_skipped);
			if (test->kind == FSIMG_JOB_CREATE)
				CHECK(unlinkat(fixture.output, "image", 0) == 0);
		}
		if (fixture.request.raw_fd != -1)
			CHECK(close(fixture.request.raw_fd) == 0);
		CHECK(close(marker) == 0);
		fixture_close(&fixture);
	}
}
