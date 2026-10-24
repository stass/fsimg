/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "internal.h"
#include "unicode.h"
#include "writer.h"
#include "test.h"

/* The writer's metadata-loss contract: the one policy operation every
 * omission and driver encoding loss passes through, what a plan and the
 * context count and report, and the two timestamp helpers drivers share.
 * Expected DOS packings are written out from the field layout (year since
 * 1980 << 25, month << 21, day << 16, hour << 11, minute << 5, second / 2)
 * and the Unix times of their calendar dates, not computed by the walk
 * under test.
 */

#define LOSS_CALLS_MAX	8

typedef struct loss_fixture {
	fsimg_ctx_t	*ctx;
	unsigned	 calls;
	uint32_t	 masks[LOSS_CALLS_MAX];
	char		 paths[LOSS_CALLS_MAX][64];
	/* What re-entering the context from the callback returned. */
	fsimg_status_t	 reentry;
} loss_fixture_t;

static void record_loss(void *, const char *, uint32_t);
static void fixture_open(loss_fixture_t *);
static uint64_t context_losses(loss_fixture_t *);
static fsimg_writer_t *new_writer(loss_fixture_t *, const char *, uint32_t);
static void add_file(fsimg_writer_t *, const fsimg_entry_t *);
static void test_operation_counts_each_field_once(void);
static void test_operation_refusal_has_no_effects(void);
static void test_operation_rejects_absent_fields(void);
static void test_omission_and_encoding_losses_share_one_total(void);
static void test_late_refusal_keeps_earlier_observations(void);
static void test_summary_refusal_reports_nothing(void);
static void test_qnx4_owner_pair_is_one_loss(void);
static uint64_t fat_access_losses(int64_t);
static void test_fat_access_time_keeps_only_the_date(void);
static void check_selected(bool, const fsimg_timestamp_t *, uint32_t, bool,
    uint32_t, bool);
static void test_whole_second_selection(void);
static void check_dos(int64_t, uint32_t, unsigned char);
static void test_dos_calendar(void);

static void
record_loss(void *argument, const char *path, uint32_t fields)
{
	loss_fixture_t *fixture = argument;
	fsimg_stats_t stats;

	CHECK(fixture->calls < LOSS_CALLS_MAX);
	CHECK(strlen(path) < sizeof(fixture->paths[0]));
	fixture->masks[fixture->calls] = fields;
	strcpy(fixture->paths[fixture->calls], path);
	fixture->calls++;
	fixture->reentry = fsimg_ctx_get_stats(fixture->ctx, &stats, NULL);
}

static void
fixture_open(loss_fixture_t *fixture)
{
	fsimg_options_t options;

	bzero(fixture, sizeof(*fixture));
	fsimg_options_init(&options);
	options.memory_bytes = UINT64_C(32) * 1024 * 1024;
	options.scratch_directory_fd = -1;
	options.loss = record_loss;
	options.loss_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
}

static uint64_t
context_losses(loss_fixture_t *fixture)
{
	fsimg_stats_t stats;

	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	return (stats.metadata_losses);
}

/* A writer for format whose flags are the initialized ones less clear. */
static fsimg_writer_t *
new_writer(loss_fixture_t *fixture, const char *format, uint32_t clear)
{
	fsimg_write_options_t options;
	fsimg_writer_t *writer;

	fsimg_write_options_init(&options);
	options.flags &= ~clear;
	CHECK(fsimg_writer_new(fixture->ctx, format, &options, &writer,
	    NULL) == FSIMG_OK);
	return (writer);
}

static void
add_file(fsimg_writer_t *writer, const fsimg_entry_t *entry)
{
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };

	CHECK(fsimg_writer_add(writer, entry, &zero, NULL) == FSIMG_OK);
}

/* An accepted mask is one callback carrying the whole mask under the
 * rendered path, its backslash doubled, and counts one loss per field in
 * both the writer and the context, which the callback cannot re-enter. A
 * zero mask does nothing.
 */
static void
test_operation_counts_each_field_once(void)
{
	const uint32_t lost = FSIMG_META_OWNER | FSIMG_META_MODE |
	    FSIMG_META_CHANGED;
	fsimg_entry_t entry = { .path = "/dir/a\\b", .kind = FSIMG_ENTRY_FILE };
	loss_fixture_t fixture;
	fsimg_writer_t *writer;

	fixture_open(&fixture);
	writer = new_writer(&fixture, "qnx6", 0);
	entry.metadata.valid = lost | FSIMG_META_MODIFIED;

	CHECK(fsimg_writer_loss(writer, &entry, 0, "unused", NULL) == FSIMG_OK);
	CHECK(fixture.calls == 0 && writer->metadata_losses == 0);

	CHECK(fsimg_writer_loss(writer, &entry, lost, "unused", NULL) == FSIMG_OK);
	CHECK(writer->metadata_losses == 3 && context_losses(&fixture) == 3);
	CHECK(fixture.calls == 1 && fixture.masks[0] == lost);
	CHECK(strcmp(fixture.paths[0], "/dir/a\\\\b") == 0);
	CHECK(fixture.reentry == FSIMG_ESTATE);
	fsimg_writer_free(writer);

	/* Context statistics accumulate across writers; each writer's own
	 * total starts from zero.
	 */
	writer = new_writer(&fixture, "qnx6", 0);
	CHECK(fsimg_writer_loss(writer, &entry, FSIMG_META_MODIFIED, "unused",
	    NULL) == FSIMG_OK);
	CHECK(writer->metadata_losses == 1 && context_losses(&fixture) == 4);
	fsimg_writer_free(writer);
	fsimg_ctx_free(fixture.ctx);
}

/* A required field, or any field when the caller forbids losses, refuses
 * with the explanation and the entry's path, and counts and reports
 * nothing of the mask.
 */
static void
test_operation_refusal_has_no_effects(void)
{
	fsimg_entry_t entry = { .path = "/strict", .kind = FSIMG_ENTRY_FILE };
	loss_fixture_t fixture;
	fsimg_writer_t *writer;
	fsimg_error_t error;

	fixture_open(&fixture);
	entry.metadata.valid = FSIMG_META_OWNER | FSIMG_META_MODE;
	entry.metadata.required = FSIMG_META_MODE;
	writer = new_writer(&fixture, "qnx6", 0);
	CHECK(fsimg_writer_loss(writer, &entry, entry.metadata.valid,
	    "explained refusal", &error) == FSIMG_EUNSUPPORTED);
	CHECK(error.operation == FSIMG_OP_WRITER);
	CHECK(strcmp(error.message, "explained refusal") == 0);
	CHECK(strcmp(error.path, "/strict") == 0);
	CHECK(writer->metadata_losses == 0 && fixture.calls == 0);
	fsimg_writer_free(writer);

	entry.metadata.required = 0;
	writer = new_writer(&fixture, "qnx6", FSIMG_WRITE_ALLOW_LOSS);
	CHECK(fsimg_writer_loss(writer, &entry, FSIMG_META_OWNER,
	    "explained refusal", &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "/strict") == 0);
	CHECK(writer->metadata_losses == 0 && fixture.calls == 0);
	CHECK(context_losses(&fixture) == 0);
	fsimg_writer_free(writer);
	fsimg_ctx_free(fixture.ctx);
}

/* An absent or unknown field cannot be lost: submitting one is an internal
 * state error, never silently dropped bits.
 */
static void
test_operation_rejects_absent_fields(void)
{
	fsimg_entry_t entry = { .path = "/absent", .kind = FSIMG_ENTRY_FILE };
	loss_fixture_t fixture;
	fsimg_writer_t *writer;

	fixture_open(&fixture);
	writer = new_writer(&fixture, "qnx6", 0);
	entry.metadata.valid = FSIMG_META_OWNER;
	CHECK(fsimg_writer_loss(writer, &entry,
	    FSIMG_META_OWNER | FSIMG_META_MODE, "unused", NULL) == FSIMG_ESTATE);
	CHECK(fsimg_writer_loss(writer, &entry, 0x80000000u, "unused",
	    NULL) == FSIMG_ESTATE);
	CHECK(writer->metadata_losses == 0 && fixture.calls == 0);
	fsimg_writer_free(writer);
	fsimg_ctx_free(fixture.ctx);
}

/* FAT omits the owner, which the common check reports first as one
 * callback, and its driver loses a UTC modification time, which needs
 * wall-clock basis. Both enter the plan's one total; retrieving the plan
 * and its information again reports nothing more.
 */
static void
test_omission_and_encoding_losses_share_one_total(void)
{
	fsimg_entry_t entry = { .path = "/MIXED", .kind = FSIMG_ENTRY_FILE };
	loss_fixture_t fixture;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan, *again;
	fsimg_plan_info_t info;

	fixture_open(&fixture);
	entry.metadata.valid = FSIMG_META_OWNER | FSIMG_META_MODIFIED;
	entry.metadata.modified = (fsimg_timestamp_t){ .seconds = 1700000000,
	    .precision_ns = 1000000000, .basis = FSIMG_TIME_UTC };
	writer = new_writer(&fixture, "fat12", 0);
	add_file(writer, &entry);

	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(info.metadata_losses == 2 && context_losses(&fixture) == 2);
	CHECK(fixture.calls == 2);
	CHECK(fixture.masks[0] == FSIMG_META_OWNER);
	CHECK(fixture.masks[1] == FSIMG_META_MODIFIED);
	CHECK(strcmp(fixture.paths[0], "/MIXED") == 0 &&
	    strcmp(fixture.paths[1], "/MIXED") == 0);

	CHECK(fsimg_writer_plan(writer, &again, NULL) == FSIMG_OK && again == plan);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(info.metadata_losses == 2 && context_losses(&fixture) == 2);
	CHECK(fixture.calls == 2);
	fsimg_writer_free(writer);
	fsimg_ctx_free(fixture.ctx);
}

/* /A's allowed omission and time loss are observed before the driver
 * refuses /B's required time. The failed plan publishes nothing, but the
 * context keeps what it observed, and the failed writer cannot plan again
 * to report it twice.
 */
static void
test_late_refusal_keeps_earlier_observations(void)
{
	const fsimg_timestamp_t utc = { .seconds = 1700000000,
	    .precision_ns = 1000000000, .basis = FSIMG_TIME_UTC };
	fsimg_entry_t first = { .path = "/A", .kind = FSIMG_ENTRY_FILE };
	fsimg_entry_t second = { .path = "/B", .kind = FSIMG_ENTRY_FILE };
	loss_fixture_t fixture;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_error_t error;

	fixture_open(&fixture);
	first.metadata.valid = FSIMG_META_MODE | FSIMG_META_MODIFIED;
	first.metadata.modified = utc;
	second.metadata.valid = FSIMG_META_MODIFIED;
	second.metadata.required = FSIMG_META_MODIFIED;
	second.metadata.modified = utc;
	writer = new_writer(&fixture, "fat12", 0);
	add_file(writer, &first);
	add_file(writer, &second);

	CHECK(fsimg_writer_plan(writer, &plan, &error) == FSIMG_EUNSUPPORTED);
	CHECK(plan == NULL && strcmp(error.path, "/B") == 0);
	CHECK(fixture.calls == 2 && context_losses(&fixture) == 2);
	CHECK(strcmp(fixture.paths[0], "/A") == 0 &&
	    strcmp(fixture.paths[1], "/A") == 0);

	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_ESTATE);
	CHECK(fixture.calls == 2 && context_losses(&fixture) == 2);
	fsimg_writer_free(writer);
	fsimg_ctx_free(fixture.ctx);
}

/* When the catalog summary already proves a refusal, the pass that names
 * the failing entry reports none of the allowed omissions before it.
 */
static void
test_summary_refusal_reports_nothing(void)
{
	fsimg_entry_t first = { .path = "/A", .kind = FSIMG_ENTRY_FILE };
	fsimg_entry_t second = { .path = "/B", .kind = FSIMG_ENTRY_FILE };
	loss_fixture_t fixture;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_error_t error;

	fixture_open(&fixture);
	first.metadata.valid = FSIMG_META_OWNER;
	second.metadata.valid = FSIMG_META_OWNER;
	second.metadata.required = FSIMG_META_OWNER;
	writer = new_writer(&fixture, "fat12", 0);
	add_file(writer, &first);
	add_file(writer, &second);

	CHECK(fsimg_writer_plan(writer, &plan, &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "/B") == 0);
	CHECK(fixture.calls == 0 && context_losses(&fixture) == 0);
	fsimg_writer_free(writer);
	fsimg_ctx_free(fixture.ctx);
}

/* Owner and group both above QNX4's 65534 are one owner loss. */
static void
test_qnx4_owner_pair_is_one_loss(void)
{
	fsimg_entry_t entry = { .path = "/owner", .kind = FSIMG_ENTRY_FILE };
	loss_fixture_t fixture;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;

	fixture_open(&fixture);
	entry.metadata.valid = FSIMG_META_OWNER;
	entry.metadata.uid = 70000;
	entry.metadata.gid = 70001;
	writer = new_writer(&fixture, "qnx4", 0);
	add_file(writer, &entry);
	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(info.metadata_losses == 1);
	CHECK(fixture.calls == 1 && fixture.masks[0] == FSIMG_META_OWNER);
	fsimg_writer_free(writer);
	fsimg_ctx_free(fixture.ctx);
}

/* The losses of planning one FAT file whose only time is a wall-clock
 * access time at seconds.
 */
static uint64_t
fat_access_losses(int64_t seconds)
{
	fsimg_entry_t entry = { .path = "/ACCESS", .kind = FSIMG_ENTRY_FILE };
	loss_fixture_t fixture;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;

	fixture_open(&fixture);
	entry.metadata.valid = FSIMG_META_ACCESSED;
	entry.metadata.accessed = (fsimg_timestamp_t){ .seconds = seconds,
	    .precision_ns = 1000000000, .basis = FSIMG_TIME_WALL };
	writer = new_writer(&fixture, "fat12", 0);
	add_file(writer, &entry);
	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	fsimg_ctx_free(fixture.ctx);
	return (info.metadata_losses);
}

/* FAT stores only an access date, so only a midnight access time is
 * preserved; one or two seconds past it are each one loss.
 */
static void
test_fat_access_time_keeps_only_the_date(void)
{
	CHECK(fat_access_losses(1700006400) == 0);	/* 2023-11-15 00:00:00 */
	CHECK(fat_access_losses(1700006401) == 1);
	CHECK(fat_access_losses(1700006402) == 1);
	CHECK(fat_access_losses(1700092799) == 1);	/* 2023-11-15 23:59:59 */
}

static void
check_selected(bool present, const fsimg_timestamp_t *time, uint32_t maximum,
    bool reproducible, uint32_t stored, bool exact)
{
	uint32_t out = 0xdeadbeefu;

	CHECK(fsimg_time_select_u32(present, time, maximum, 1000, reproducible,
	    &out) == exact);
	CHECK(out == stored);
}

/* Whole seconds in range are kept, truncated, and preserved only for a
 * UTC whole second; anything outside the range takes the fallback, 1000
 * here, rather than an endpoint; reproducible mode stores the fallback.
 */
static void
test_whole_second_selection(void)
{
	const uint32_t qnx4 = 2147483647u;
	fsimg_timestamp_t time = { .basis = FSIMG_TIME_UTC };

	check_selected(false, NULL, qnx4, false, 1000, true);
	check_selected(false, NULL, qnx4, true, 1000, true);

	time.seconds = 0;
	check_selected(true, &time, qnx4, false, 0, true);
	time.seconds = -1;
	check_selected(true, &time, qnx4, false, 1000, false);
	time.seconds = 2147483647;
	check_selected(true, &time, qnx4, false, 2147483647u, true);
	time.seconds = 2147483648;
	check_selected(true, &time, qnx4, false, 1000, false);
	check_selected(true, &time, UINT32_MAX, false, 2147483648u, true);
	time.seconds = 4294967295;
	check_selected(true, &time, UINT32_MAX, false, UINT32_MAX, true);
	time.seconds = 4294967296;
	check_selected(true, &time, UINT32_MAX, false, 1000, false);

	time.seconds = 5;
	time.nanoseconds = 999999999;
	check_selected(true, &time, qnx4, false, 5, false);
	time.nanoseconds = 0;
	time.basis = FSIMG_TIME_WALL;
	check_selected(true, &time, qnx4, false, 5, false);
	time.basis = FSIMG_TIME_OFFSET;
	time.offset_minutes = 60;
	check_selected(true, &time, qnx4, false, 5, false);

	time = (fsimg_timestamp_t){ .seconds = 1000, .basis = FSIMG_TIME_UTC };
	check_selected(true, &time, qnx4, true, 1000, true);
	time.seconds = 1001;
	check_selected(true, &time, qnx4, true, 1000, false);
	time = (fsimg_timestamp_t){ .seconds = 1000, .basis = FSIMG_TIME_WALL };
	check_selected(true, &time, qnx4, true, 1000, false);
	time.seconds = -1;
	time.basis = FSIMG_TIME_UTC;
	check_selected(true, &time, qnx4, true, 1000, false);
}

static void
check_dos(int64_t seconds, uint32_t packed, unsigned char odd)
{
	uint32_t out_packed = 0;
	unsigned char out_odd = 9;

	CHECK(fsimg_dos_time_encode(seconds, &out_packed, &out_odd));
	CHECK(out_packed == packed && out_odd == odd);
}

/* The DOS calendar runs from 1980-01-01 00:00:00 through 2107-12-31
 * 23:59:59. 2000 is a leap year and 2100 is not. A rejected value leaves
 * both outputs as they were.
 */
static void
test_dos_calendar(void)
{
	uint32_t packed = 0xdeadbeefu;
	unsigned char odd = 7;

	check_dos(315532800, 0x00210000u, 0);		/* 1980-01-01 00:00:00 */
	check_dos(951782400, 0x285d0000u, 0);		/* 2000-02-29 00:00:00 */
	check_dos(1709210097, 0x585d645cu, 1);		/* 2024-02-29 12:34:57 */
	check_dos(4107542399, 0xf05cbf7du, 1);		/* 2100-02-28 23:59:59 */
	check_dos(4107542400, 0xf0610000u, 0);		/* 2100-03-01 00:00:00 */
	check_dos(4354819199, 0xff9fbf7du, 1);		/* 2107-12-31 23:59:59 */

	CHECK(!fsimg_dos_time_encode(315532799, &packed, &odd));
	CHECK(!fsimg_dos_time_encode(4354819200, &packed, &odd));
	CHECK(!fsimg_dos_time_encode(INT64_MIN, &packed, &odd));
	CHECK(packed == 0xdeadbeefu && odd == 7);
}

int
main(void)
{
	test_operation_counts_each_field_once();
	test_operation_refusal_has_no_effects();
	test_operation_rejects_absent_fields();
	test_omission_and_encoding_losses_share_one_total();
	test_late_refusal_keeps_earlier_observations();
	test_summary_refusal_reports_nothing();
	test_qnx4_owner_pair_is_one_loss();
	test_fat_access_time_keeps_only_the_date();
	test_whole_second_selection();
	test_dos_calendar();
	return (0);
}
