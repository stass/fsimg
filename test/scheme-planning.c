/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "host.h"
#include "internal.h"
#include "layer.h"
#include "test.h"

/*
 * The scheme planning boundary (specs/design-scheme-planning.md): common
 * disk planning reaches a scheme only through its descriptor's operations.
 *
 * A synthetic descriptor, "tri", stands in for a third scheme. It is
 * compiled test data, planned through fsimg_disk_plan_scheme on the plan of
 * a stack without a scheme, and never registered. It borrows the MBR
 * record representation but has its own sector size, slot count,
 * footprint, disk key, defaults, final limit and report, so each place
 * where common code consulted GPT or MBR instead of the descriptor shows
 * here as a wrong default, offset, key or report. Its encoder writes
 * deterministic test bytes ('L' leading, 'T' trailing); they are not a
 * disk format any tool reads.
 *
 * The GPT and MBR cases drive the same plan functions through the
 * registered descriptors, checking the resolved records against values
 * spelled out here rather than against the planner's own helpers. Their
 * emitted bytes are decoded independently in test/layers-write.c.
 */

#define MIB		(UINT64_C(1024) * 1024)
#define EPOCH		INT64_C(1700000000)	/* 0x6553f100 */
#define TRI_SECTOR	4096u
#define TRI_SLOTS	3u
#define TRI_DEFAULT_TYPE 0x42

/* The synthetic scheme's configuration and what its operations saw. */
typedef struct tri_state {
	uint64_t	 requested_disk_bytes;
	uint64_t	 leading, trailing;	/* Footprint in sectors */
	uint64_t	 cap_bytes;		/* validate_plan's final limit */
	uint32_t	 fail_slot;		/* plan_partition refuses it */
	unsigned	 plan_disk_calls, partition_calls, validate_calls,
			 report_calls, encode_calls;
	uint64_t	 validated_bytes;
	bool		 saw_layers, saw_part;
} tri_state_t;

static tri_state_t tri;

/* A disk in memory for emission. */
typedef struct memory {
	unsigned char	*bytes;
	uint64_t	 length;
} memory_t;

static fsimg_status_t tri_plan_disk(const fsimg_scheme_plan_args_t *,
    fsimg_scheme_table_t *, fsimg_scheme_plan_facts_t *, fsimg_error_t *);
static fsimg_status_t tri_plan_partition(const fsimg_scheme_partition_args_t *,
    fsimg_partition_record_t *, fsimg_error_t *);
static fsimg_status_t tri_validate_plan(const fsimg_scheme_table_t *,
    uint64_t, fsimg_error_t *);
static void tri_report_plan(const fsimg_scheme_table_t *, uint32_t,
    fsimg_image_info_t *, fsimg_partition_info_t *);
static void tri_footprint(uint32_t, uint64_t *, uint64_t *);
static fsimg_status_t tri_encode(fsimg_ctx_t *, const fsimg_scheme_table_t *,
    uint64_t, fsimg_scheme_tables_t *, fsimg_error_t *);
static fsimg_status_t tri_open(const fsimg_scheme_input_t *,
    fsimg_scheme_table_t *, fsimg_error_t *);
static fsimg_status_t tri_verify(const fsimg_scheme_input_t *,
    const fsimg_scheme_table_t *, fsimg_error_t *);
static void tri_reset(void);
static fsimg_ctx_t *context_new(void);
static uint64_t memory_in_use(fsimg_ctx_t *);
static fsimg_status_t tri_plan(fsimg_ctx_t *, const char *,
    const fsimg_layer_options_t *, uint32_t, fsimg_disk_plan_t *,
    fsimg_error_t *);
static fsimg_status_t scheme_plan(fsimg_ctx_t *, const char *,
    const fsimg_layer_options_t *, uint32_t, uint32_t, fsimg_disk_plan_t *,
    fsimg_error_t *);
static fsimg_status_t memory_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t memory_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t memory_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t memory_resize(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t fill_volume(void *, fsimg_io_t *, fsimg_error_t *);
static bool bytes_are(const unsigned char *, uint64_t, unsigned char);
static void guid_text(const char *, unsigned char *);

static void test_registry_write_sets(void);
static void test_capabilities_follow_write_set(void);
static void test_read_only_scheme_refused_before_callbacks(void);
static void test_descriptor_supplies_disk_defaults(void);
static void test_partition_sources_and_keys(void);
static void test_placement_follows_footprint(void);
static void test_final_validation_after_rounding(void);
static void test_fixed_disk_uses_descriptor_key(void);
static void test_large_footprints_are_checked(void);
static void test_report_is_descriptor_owned(void);
static void test_emission_uses_descriptor_encoder(void);
static void test_cleanup_at_every_boundary(void);
static void test_unpartitioned_plan_has_no_planner(void);
static void test_records_retain_no_request_pointers(void);
static void test_gpt_sector_and_count(void);
static void test_gpt_names(void);
static void test_gpt_identities(void);
static void test_mbr_metadata(void);
static void test_mbr_limits(void);

static const fsimg_scheme_ops_t tri_scheme = {
	.probe_bytes = 512,
	.open = tri_open,
	.verify = tri_verify,
	.slots = TRI_SLOTS,
	.max_disk_sectors = UINT32_MAX,
	.plan_disk = tri_plan_disk,
	.plan_partition = tri_plan_partition,
	.validate_plan = tri_validate_plan,
	.report_plan = tri_report_plan,
	.footprint = tri_footprint,
	.encode = tri_encode
};

static const fsimg_layer_t tri_layer = {
	.name = "tri",
	.kind = FSIMG_LAYER_SCHEME,
	.scheme = &tri_scheme
};

/* The same reader without a write set: a read-only scheme. */
static const fsimg_scheme_ops_t read_only_scheme = {
	.probe_bytes = 512,
	.open = tri_open,
	.verify = tri_verify
};

static const fsimg_layer_t read_only_layer = {
	.name = "readonly",
	.kind = FSIMG_LAYER_SCHEME,
	.scheme = &read_only_scheme
};

/* Its write set with validate_plan missing: a descriptor defect. */
static const fsimg_scheme_ops_t partial_scheme = {
	.slots = TRI_SLOTS,
	.max_disk_sectors = UINT32_MAX,
	.plan_disk = tri_plan_disk,
	.plan_partition = tri_plan_partition,
	.report_plan = tri_report_plan,
	.footprint = tri_footprint,
	.encode = tri_encode
};

static fsimg_status_t
tri_plan_disk(const fsimg_scheme_plan_args_t *args,
    fsimg_scheme_table_t *table, fsimg_scheme_plan_facts_t *facts,
    fsimg_error_t *error FSIMG_UNUSED)
{
	tri.plan_disk_calls++;
	table->kind = FSIMG_SCHEME_MBR;
	table->sector_bytes = TRI_SECTOR;
	table->mbr_disk_id = (uint32_t)args->epoch ^ 0x7e57u;
	facts->requested_disk_bytes = tri.requested_disk_bytes;
	facts->disk_key = "tri:disk";
	return (FSIMG_OK);
}

/* The default type is the descriptor's own, offset by the slot; a part's
 * type text, when given, is its first byte.
 */
static fsimg_status_t
tri_plan_partition(const fsimg_scheme_partition_args_t *args,
    fsimg_partition_record_t *record, fsimg_error_t *error)
{
	tri.partition_calls++;
	tri.saw_layers |= args->layers != NULL;
	tri.saw_part |= args->part != NULL;
	if (args->index == tri.fail_slot)
		return (fsimg_partition_setting_error(args, "tri:type",
		    "tri refuses this partition", error));
	record->mbr_type = (uint8_t)(TRI_DEFAULT_TYPE + args->index);
	if (args->part != NULL && args->part->type != NULL &&
	    args->part->type[0] != '\0')
		record->mbr_type = (uint8_t)args->part->type[0];
	record->used = true;
	return (FSIMG_OK);
}

static fsimg_status_t
tri_validate_plan(const fsimg_scheme_table_t *table FSIMG_UNUSED,
    uint64_t disk_bytes, fsimg_error_t *error)
{
	tri.validate_calls++;
	tri.validated_bytes = disk_bytes;
	if (disk_bytes > tri.cap_bytes)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "a tri disk exceeds its limit"));
	return (FSIMG_OK);
}

static void
tri_report_plan(const fsimg_scheme_table_t *table, uint32_t slot,
    fsimg_image_info_t *image, fsimg_partition_info_t *partition)
{
	tri.report_calls++;
	image->sector_bytes = table->sector_bytes;
	image->partition_count = TRI_SLOTS;
	image->selected = slot;
	image->mbr_disk_id = table->mbr_disk_id;
	fsimg_partition_report(FSIMG_SCHEME_MBR, &table->records[slot - 1],
	    partition);
}

static void
tri_footprint(uint32_t sector FSIMG_UNUSED, uint64_t *leading,
    uint64_t *trailing)
{
	*leading = tri.leading;
	*trailing = tri.trailing;
}

/* Deterministic test bytes over the footprint: 'L' at the start of the
 * disk and 'T' at its end.
 */
static fsimg_status_t
tri_encode(fsimg_ctx_t *ctx, const fsimg_scheme_table_t *table,
    uint64_t disk_bytes, fsimg_scheme_tables_t *out, fsimg_error_t *error)
{
	size_t leading = (size_t)(tri.leading * table->sector_bytes);
	size_t trailing = (size_t)(tri.trailing * table->sector_bytes);

	tri.encode_calls++;
	bzero(out, sizeof(*out));
	out->regions[0].bytes = fsimg_alloc(ctx, leading, error);
	if (out->regions[0].bytes == NULL)
		return (error->status);
	memset(out->regions[0].bytes, 'L', leading);
	out->regions[0].length = leading;
	out->count = 1;
	if (trailing == 0)
		return (FSIMG_OK);
	out->regions[1].bytes = fsimg_alloc(ctx, trailing, error);
	if (out->regions[1].bytes == NULL)
		return (error->status);
	memset(out->regions[1].bytes, 'T', trailing);
	out->regions[1].offset = disk_bytes - trailing;
	out->regions[1].length = trailing;
	out->count = 2;
	return (FSIMG_OK);
}

static fsimg_status_t
tri_open(const fsimg_scheme_input_t *input FSIMG_UNUSED,
    fsimg_scheme_table_t *table FSIMG_UNUSED, fsimg_error_t *error)
{
	return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
	    "tri is never read"));
}

static fsimg_status_t
tri_verify(const fsimg_scheme_input_t *input FSIMG_UNUSED,
    const fsimg_scheme_table_t *table FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

/* Three sectors before the partitions and two after, no fixed length, and
 * a limit no automatic disk here reaches.
 */
static void
tri_reset(void)
{
	bzero(&tri, sizeof(tri));
	tri.leading = 3;
	tri.trailing = 2;
	tri.cap_bytes = UINT64_MAX;
}

static fsimg_ctx_t *
context_new(void)
{
	fsimg_ctx_t *ctx;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	return (ctx);
}

static uint64_t
memory_in_use(fsimg_ctx_t *ctx)
{
	fsimg_stats_t stats;

	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	return (stats.memory_bytes);
}

/*
 * A plan of the stack text (NULL for an empty stack) and its settings
 * (NULL for the defaults) with tri as its scheme: init plans the stack's
 * container and compression, then the scheme stage runs for tri as init
 * runs it for a registered scheme.
 */
static fsimg_status_t
tri_plan(fsimg_ctx_t *ctx, const char *text,
    const fsimg_layer_options_t *given, uint32_t count,
    fsimg_disk_plan_t *plan, fsimg_error_t *error)
{
	fsimg_layer_options_t layers;
	fsimg_disk_request_t request;
	fsimg_session_t session;
	fsimg_stack_t stack;
	fsimg_status_t status;

	bzero(&stack, sizeof(stack));
	if (text != NULL)
		CHECK(fsimg_stack_parse(text, &stack, NULL) == FSIMG_OK);
	fsimg_layer_options_init(&layers);
	if (given != NULL)
		layers = *given;
	fsimg_session_init(&session, ctx, "test", 0);
	request = (fsimg_disk_request_t){ .session = &session,
	    .stack = &stack, .layers = &layers, .compression = "",
	    .epoch = EPOCH, .flags = FSIMG_WRITE_REPRODUCIBLE,
	    .partitions = count };
	status = fsimg_disk_plan_init(ctx, &request, plan, error);
	if (status != FSIMG_OK)
		return (status);
	CHECK(plan->scheme == NULL);
	return (fsimg_disk_plan_scheme(ctx, plan, &tri_layer, &layers, count,
	    error));
}

/* A plan of a registered scheme, by name, with the given settings. */
static fsimg_status_t
scheme_plan(fsimg_ctx_t *ctx, const char *text,
    const fsimg_layer_options_t *layers, uint32_t flags, uint32_t count,
    fsimg_disk_plan_t *plan, fsimg_error_t *error)
{
	fsimg_disk_request_t request;
	fsimg_session_t session;
	fsimg_stack_t stack;

	CHECK(fsimg_stack_parse(text, &stack, NULL) == FSIMG_OK);
	fsimg_session_init(&session, ctx, "test", 0);
	request = (fsimg_disk_request_t){ .session = &session,
	    .stack = &stack, .layers = layers, .compression = "",
	    .epoch = EPOCH, .flags = flags, .partitions = count };
	return (fsimg_disk_plan_init(ctx, &request, plan, error));
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *done, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_t *memory = argument;

	*done = offset >= memory->length ? 0 :
	    (size_t)(memory->length - offset < length ?
	    memory->length - offset : length);
	bcopy(memory->bytes + offset, buffer, *done);
	return (FSIMG_OK);
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer,
    size_t length, size_t *done, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_t *memory = argument;

	CHECK(offset + length <= memory->length);
	bcopy(buffer, memory->bytes + offset, length);
	*done = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((memory_t *)argument)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_resize(void *argument FSIMG_UNUSED, uint64_t length FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

/* The volume: 'V' over its slice. */
static fsimg_status_t
fill_volume(void *argument FSIMG_UNUSED, fsimg_io_t *volume,
    fsimg_error_t *error)
{
	unsigned char bytes[4096];
	uint64_t length;
	size_t done;
	fsimg_status_t status;

	memset(bytes, 'V', sizeof(bytes));
	status = fsimg_io_get_size(volume, &length, error);
	for (uint64_t at = 0; status == FSIMG_OK && at < length;
	    at += sizeof(bytes))
		status = fsimg_io_write_exact(volume, at, bytes,
		    length - at < sizeof(bytes) ? (size_t)(length - at) :
		    sizeof(bytes), &done, error);
	return (status);
}

static bool
bytes_are(const unsigned char *bytes, uint64_t length, unsigned char value)
{
	for (uint64_t i = 0; i < length; i++) {
		if (bytes[i] != value)
			return (false);
	}
	return (true);
}

/* RFC 4122 text into textual byte order, by this file's own parser. */
static void
guid_text(const char *text, unsigned char *guid)
{
	unsigned value;

	for (unsigned i = 0; i < 16; i++) {
		if (*text == '-')
			text++;
		CHECK(sscanf(text, "%2x", &value) == 1);
		guid[i] = (unsigned char)value;
		text += 2;
	}
}

/* Every registered scheme has a complete write set or none; GPT and MBR
 * have one. A partial set is a defect the check rejects.
 */
static void
test_registry_write_sets(void)
{
	const fsimg_layer_t *layer;
	unsigned schemes = 0;

	for (size_t i = 0; (layer = fsimg_layer_at(i)) != NULL; i++) {
		if (layer->scheme == NULL)
			continue;
		schemes++;
		CHECK(fsimg_scheme_write_set_valid(layer->scheme));
		CHECK(fsimg_scheme_writable(layer->scheme));
	}
	CHECK(schemes == 2);
	CHECK(fsimg_layer_find("tri") == NULL);
	CHECK(fsimg_scheme_write_set_valid(&tri_scheme));
	CHECK(fsimg_scheme_write_set_valid(&read_only_scheme));
	CHECK(!fsimg_scheme_write_set_valid(&partial_scheme));
}

/* Write capability is the write set's presence: GPT and MBR read, write
 * and verify; the read-only descriptor reads and verifies only.
 */
static void
test_capabilities_follow_write_set(void)
{
	const uint32_t all = FSIMG_CAP_READ | FSIMG_CAP_WRITE | FSIMG_CAP_VERIFY;
	fsimg_layer_info_t info;
	size_t cursor;
	unsigned seen = 0;

	cursor = 0;
	while (fsimg_layer_next(&cursor, &info, NULL) == FSIMG_OK) {
		if (info.kind != FSIMG_LAYER_SCHEME)
			continue;
		seen++;
		CHECK(info.capabilities == all);
	}
	CHECK(seen == 2);
	CHECK(fsimg_layer_capabilities(&tri_layer) == all);
	CHECK(fsimg_layer_capabilities(&read_only_layer) ==
	    (FSIMG_CAP_READ | FSIMG_CAP_VERIFY));
}

/* Creating with a read-only scheme fails naming it, before any planner
 * callback runs and with no record allocated.
 */
static void
test_read_only_scheme_refused_before_callbacks(void)
{
	fsimg_layer_options_t layers;
	fsimg_disk_plan_t plan;
	fsimg_error_t error;
	fsimg_ctx_t *ctx = context_new();

	bzero(&plan, sizeof(plan));
	fsimg_layer_options_init(&layers);
	CHECK(fsimg_disk_plan_scheme(ctx, &plan, &read_only_layer, &layers, 1,
	    &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "readonly") == 0);
	CHECK(plan.scheme == NULL && plan.table.records == NULL);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

/*
 * The descriptor alone supplies the disk: its sector, identity and key,
 * and its slot bound, which refuses 0 and 4 partitions with the limit.
 * Records start zeroed with their slot numbers.
 */
static void
test_descriptor_supplies_disk_defaults(void)
{
	static const uint32_t refused[] = { 0, TRI_SLOTS + 1 };
	fsimg_disk_plan_t plan;
	fsimg_error_t error;
	fsimg_ctx_t *ctx = context_new();

	tri_reset();
	CHECK(tri_plan(ctx, NULL, NULL, TRI_SLOTS, &plan, &error) == FSIMG_OK);
	CHECK(tri.plan_disk_calls == 1);
	CHECK(plan.table.sector_bytes == TRI_SECTOR);
	CHECK(plan.table.mbr_disk_id == ((uint32_t)EPOCH ^ 0x7e57u));
	CHECK(strcmp(plan.disk_key, "tri:disk") == 0 && !plan.fixed);
	CHECK(plan.table.count == TRI_SLOTS);
	for (uint32_t i = 0; i < TRI_SLOTS; i++)
		CHECK(plan.table.records[i].index == i + 1 &&
		    !plan.table.records[i].used &&
		    plan.table.records[i].mbr_type == 0);
	fsimg_disk_plan_free(&plan);

	for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		tri_reset();
		CHECK(tri_plan(ctx, NULL, NULL, refused[i], &plan, &error) ==
		    FSIMG_EINVAL);
		CHECK(error.limit == TRI_SLOTS && error.observed == refused[i]);
		CHECK(strstr(error.message, "tri") != NULL);
		CHECK(tri.plan_disk_calls == 0 && plan.table.records == NULL);
		fsimg_disk_plan_free(&plan);
	}
	fsimg_ctx_free(ctx);
}

/*
 * A partition comes from exactly one source: the layer options of a
 * create or wrap, or a compose part. Both or neither is an internal
 * error that reaches no callback. A failure names the setting for the
 * first and the part for the second.
 */
static void
test_partition_sources_and_keys(void)
{
	const fsimg_partition_request_t part = { .type = "Q" };
	fsimg_layer_options_t layers;
	fsimg_disk_plan_t plan;
	fsimg_error_t error;
	fsimg_ctx_t *ctx = context_new();

	fsimg_layer_options_init(&layers);
	tri_reset();
	CHECK(tri_plan(ctx, NULL, NULL, 2, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, &part, NULL,
	    &error) == FSIMG_ESTATE);
	CHECK(fsimg_disk_plan_partition(&plan, 1, NULL, NULL, NULL,
	    &error) == FSIMG_ESTATE);
	CHECK(fsimg_disk_plan_partition(&plan, 0, &layers, NULL, NULL,
	    &error) == FSIMG_ESTATE);
	CHECK(fsimg_disk_plan_partition(&plan, 3, &layers, NULL, NULL,
	    &error) == FSIMG_ESTATE);
	CHECK(tri.partition_calls == 0);

	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, NULL,
	    &error) == FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 2, NULL, &part, NULL,
	    &error) == FSIMG_OK);
	CHECK(tri.saw_layers && tri.saw_part);
	CHECK(plan.table.records[0].mbr_type == TRI_DEFAULT_TYPE + 1);
	CHECK(plan.table.records[1].mbr_type == 'Q');
	CHECK(plan.table.records[1].index == 2 && plan.table.records[1].used);

	tri.fail_slot = 2;
	CHECK(fsimg_disk_plan_partition(&plan, 2, NULL, &part, NULL,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "partition 2") == 0);
	CHECK(!plan.table.records[1].used);
	tri.fail_slot = 1;
	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, NULL,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "tri:type") == 0);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

/*
 * Placement by the descriptor's footprint: three 4096-byte sectors lead,
 * so the first volume starts at 1 MiB; an odd volume length rounds up to
 * whole 4096-byte sectors; the second starts at the next 1 MiB boundary;
 * two sectors trail. A leading footprint just past 1 MiB moves the first
 * start to 2 MiB, and a zero trailing footprint ends the disk at the last
 * volume. A zero-length volume is refused naming its slot.
 */
static void
test_placement_follows_footprint(void)
{
	const uint64_t lengths[2] = { 5000, MIB };
	const uint64_t empty[2] = { 4096, 0 };
	fsimg_layer_options_t layers;
	fsimg_disk_plan_t plan;
	fsimg_error_t error;
	fsimg_ctx_t *ctx = context_new();

	fsimg_layer_options_init(&layers);
	tri_reset();
	CHECK(tri_plan(ctx, NULL, NULL, 2, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_OK);
	CHECK(plan.table.records[0].offset == MIB &&
	    plan.table.records[0].length == 8192);
	CHECK(plan.table.records[1].offset == 2 * MIB &&
	    plan.table.records[1].length == MIB);
	CHECK(plan.disk_bytes == 3 * MIB + 2 * TRI_SECTOR);
	CHECK(tri.validate_calls == 1 && tri.validated_bytes == plan.disk_bytes);
	fsimg_disk_plan_free(&plan);

	tri_reset();
	tri.leading = MIB / TRI_SECTOR + 1;
	tri.trailing = 0;
	CHECK(tri_plan(ctx, NULL, NULL, 1, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_OK);
	CHECK(plan.table.records[0].offset == 2 * MIB);
	CHECK(plan.disk_bytes == 2 * MIB + 8192);
	fsimg_disk_plan_free(&plan);

	tri_reset();
	CHECK(tri_plan(ctx, NULL, NULL, 2, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, empty, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "partition 2") == 0 && tri.validate_calls == 0);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

/*
 * Final validation sees the disk after the container rounds it: a 1 MiB
 * VHD alignment rounds the 1 MiB + 3 sector disk to 2 MiB, the trailing
 * tables end there, and a limit between the two lengths fails only after
 * rounding. A container granularity that is no multiple of the sector
 * (set here directly on the container's plan, since every container's
 * granularity is a power of two) would leave a partial final sector,
 * which is refused rather than truncated.
 */
static void
test_final_validation_after_rounding(void)
{
	const uint64_t lengths[1] = { 4096 };
	const uint64_t unrounded = MIB + 4096 + 2 * TRI_SECTOR;
	fsimg_layer_options_t aligned;
	fsimg_disk_plan_t plan;
	fsimg_error_t error;
	fsimg_ctx_t *ctx = context_new();

	fsimg_layer_options_init(&aligned);
	aligned.vhd_align_bytes = (uint32_t)MIB;
	tri_reset();
	CHECK(tri_plan(ctx, "vhd", &aligned, 1, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_OK);
	CHECK(plan.disk_bytes == 2 * MIB && tri.validated_bytes == 2 * MIB);
	CHECK(plan.padding_bytes == 2 * MIB - unrounded);
	fsimg_disk_plan_free(&plan);

	tri_reset();
	tri.cap_bytes = unrounded;
	CHECK(tri_plan(ctx, "vhd", &aligned, 1, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "tri disk") != NULL);
	CHECK(tri.validated_bytes == 2 * MIB);
	fsimg_disk_plan_free(&plan);

	tri_reset();
	CHECK(tri_plan(ctx, "vhd", NULL, 1, &plan, NULL) == FSIMG_OK);
	plan.container_plan->granularity = 3 * 1024;
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "tri:disk") == 0 && tri.validate_calls == 0);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

/*
 * A fixed length is the descriptor's to request, and every refusal of it
 * names the descriptor's key: a partial sector, a disk one sector short
 * of its partition and tables, and a fit with no room for one granule. An
 * exact fit is accepted, and fit gives an absent volume the longest
 * multiple of the filesystem's granule, which differs from the sector.
 */
static void
test_fixed_disk_uses_descriptor_key(void)
{
	const uint64_t exact = 4 * MIB;
	uint64_t lengths[1], volume;
	fsimg_disk_plan_t plan;
	fsimg_error_t error;
	fsimg_ctx_t *ctx = context_new();

	tri_reset();
	tri.requested_disk_bytes = exact + 512;
	CHECK(tri_plan(ctx, NULL, NULL, 1, &plan, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "tri:disk") == 0);
	fsimg_disk_plan_free(&plan);

	tri_reset();
	tri.requested_disk_bytes = exact;
	CHECK(tri_plan(ctx, NULL, NULL, 1, &plan, NULL) == FSIMG_OK);
	CHECK(plan.fixed && plan.disk_bytes == exact);
	volume = 0;
	CHECK(fsimg_disk_plan_fit(&plan, 1536, &volume, &error) == FSIMG_OK);
	CHECK(volume == (exact - MIB - 2 * TRI_SECTOR) / 1536 * 1536);
	lengths[0] = exact - MIB - 2 * TRI_SECTOR;
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_OK);
	CHECK(plan.disk_bytes == exact && tri.validated_bytes == exact);
	lengths[0] += 1;
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "tri:disk") == 0);
	volume = exact;
	CHECK(fsimg_disk_plan_fit(&plan, 512, &volume, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "tri:disk") == 0);
	fsimg_disk_plan_free(&plan);

	tri_reset();
	tri.requested_disk_bytes = MIB + TRI_SECTOR;
	CHECK(tri_plan(ctx, NULL, NULL, 1, &plan, NULL) == FSIMG_OK);
	volume = 0;
	CHECK(fsimg_disk_plan_fit(&plan, 512, &volume, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "tri:disk") == 0);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

/*
 * Footprints whose byte lengths overflow 64 bits are refused as range
 * errors by placement and fit rather than wrapping; a leading run that
 * fits but cannot round up to the alignment is refused too. No disk of
 * that size is ever allocated.
 */
static void
test_large_footprints_are_checked(void)
{
	const uint64_t lengths[1] = { 4096 };
	uint64_t volume;
	fsimg_disk_plan_t plan;
	fsimg_error_t error;
	fsimg_ctx_t *ctx = context_new();

	tri_reset();
	tri.leading = UINT64_MAX / TRI_SECTOR + 1;
	CHECK(tri_plan(ctx, NULL, NULL, 1, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_ERANGE);
	fsimg_disk_plan_free(&plan);

	tri_reset();
	tri.trailing = UINT64_MAX / TRI_SECTOR + 1;
	tri.requested_disk_bytes = 4 * MIB;
	CHECK(tri_plan(ctx, NULL, NULL, 1, &plan, NULL) == FSIMG_OK);
	volume = 0;
	CHECK(fsimg_disk_plan_fit(&plan, 512, &volume, &error) == FSIMG_ERANGE);
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_ERANGE);
	fsimg_disk_plan_free(&plan);

	tri_reset();
	tri.leading = UINT64_MAX / TRI_SECTOR;
	CHECK(tri_plan(ctx, NULL, NULL, 1, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_ERANGE);
	fsimg_disk_plan_free(&plan);

	tri_reset();
	tri.trailing = (UINT64_MAX - MIB) / TRI_SECTOR;
	CHECK(tri_plan(ctx, NULL, NULL, 1, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_ERANGE);
	CHECK(tri.validate_calls == 0);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

/* The planned report: common fields from the plan, the scheme's name, and
 * the descriptor's own slot count, identity and partition metadata; a
 * slot outside the table leaves the scheme's fields empty.
 */
static void
test_report_is_descriptor_owned(void)
{
	const uint64_t lengths[1] = { 4096 };
	fsimg_layer_options_t layers;
	fsimg_image_info_t image;
	fsimg_partition_info_t partition;
	fsimg_disk_plan_t plan;
	fsimg_ctx_t *ctx = context_new();

	fsimg_layer_options_init(&layers);
	tri_reset();
	CHECK(tri_plan(ctx, NULL, NULL, 1, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, NULL,
	    NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, lengths, NULL) == FSIMG_OK);
	fsimg_disk_plan_report(&plan, 1, &image, &partition);
	CHECK(tri.report_calls == 1);
	CHECK(strcmp(image.scheme, "tri") == 0);
	CHECK(image.partition_count == TRI_SLOTS && image.selected == 1);
	CHECK(image.sector_bytes == TRI_SECTOR);
	CHECK(image.mbr_disk_id == ((uint32_t)EPOCH ^ 0x7e57u));
	CHECK(image.disk_bytes == plan.disk_bytes &&
	    image.file_bytes == plan.disk_bytes);
	CHECK(partition.used && partition.index == 1 &&
	    partition.offset == MIB && partition.length == 4096 &&
	    partition.mbr_type == TRI_DEFAULT_TYPE + 1);
	fsimg_disk_plan_report(&plan, 2, &image, &partition);
	CHECK(tri.report_calls == 1 && image.scheme[0] == '\0' &&
	    image.disk_bytes == plan.disk_bytes);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

/*
 * Emission encodes through the descriptor: its 'L' bytes over the leading
 * three sectors, zeros to the 1 MiB start, the volume, and its 'T' bytes
 * over the last two sectors of the disk.
 */
static void
test_emission_uses_descriptor_encoder(void)
{
	const uint64_t lengths[1] = { 10000 };
	fsimg_layer_options_t layers;
	fsimg_io_ops_t ops;
	fsimg_disk_plan_t plan;
	fsimg_session_t session;
	memory_t memory;
	fsimg_io_t *io;
	fsimg_ctx_t *ctx = context_new();
	uint64_t trailing;

	fsimg_layer_options_init(&layers);
	tri_reset();
	CHECK(tri_plan(ctx, NULL, NULL, 1, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, NULL,
	    NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, lengths, NULL) == FSIMG_OK);

	memory.length = plan.disk_bytes;
	memory.bytes = malloc((size_t)memory.length);
	CHECK(memory.bytes != NULL);
	memset(memory.bytes, 0xa5, (size_t)memory.length);
	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.write_at = memory_write;
	ops.get_size = memory_size;
	ops.resize = memory_resize;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, &memory, &io, NULL) ==
	    FSIMG_OK);
	fsimg_session_init(&session, ctx, "test", 0);
	CHECK(fsimg_disk_plan_emit(&plan, lengths[0], io, &session,
	    fill_volume, NULL, NULL) == FSIMG_OK);
	CHECK(tri.encode_calls == 1);

	trailing = 2 * TRI_SECTOR;
	CHECK(bytes_are(memory.bytes, 3 * TRI_SECTOR, 'L'));
	CHECK(bytes_are(memory.bytes + 3 * TRI_SECTOR, MIB - 3 * TRI_SECTOR, 0));
	CHECK(bytes_are(memory.bytes + MIB, lengths[0], 'V'));
	CHECK(bytes_are(memory.bytes + MIB + lengths[0],
	    plan.disk_bytes - trailing - MIB - lengths[0], 0));
	CHECK(bytes_are(memory.bytes + plan.disk_bytes - trailing, trailing,
	    'T'));
	fsimg_io_free(io);
	free(memory.bytes);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

/*
 * One owner cleans up after a failure at every boundary: the record
 * allocation, the first, middle and last partition, and final validation.
 * Freeing twice is safe, and every charge returns.
 */
static void
test_cleanup_at_every_boundary(void)
{
	const uint64_t lengths[TRI_SLOTS] = { 4096, 4096, 4096 };
	fsimg_layer_options_t layers;
	fsimg_disk_plan_t plan;
	fsimg_error_t error;
	fsimg_ctx_t *ctx = context_new();
	uint64_t baseline = memory_in_use(ctx);
	fsimg_status_t status;

	fsimg_layer_options_init(&layers);
	tri_reset();
	status = FSIMG_ENOMEM;
	for (uint64_t attempt = 1; status == FSIMG_ENOMEM; attempt++) {
		CHECK(attempt < 100);
		fsimg_test_fail_alloc_after(ctx, attempt);
		status = tri_plan(ctx, NULL, NULL, TRI_SLOTS, &plan, &error);
		fsimg_test_fail_alloc_after(ctx, 0);
		CHECK(status != FSIMG_ENOMEM || plan.table.records == NULL);
		fsimg_disk_plan_free(&plan);
		fsimg_disk_plan_free(&plan);
		CHECK(memory_in_use(ctx) == baseline);
	}
	CHECK(status == FSIMG_OK);

	for (uint32_t failing = 1; failing <= TRI_SLOTS; failing++) {
		tri_reset();
		tri.fail_slot = failing;
		CHECK(tri_plan(ctx, NULL, NULL, TRI_SLOTS, &plan, &error) == FSIMG_OK);
		status = FSIMG_OK;
		for (uint32_t slot = 1; status == FSIMG_OK && slot <= TRI_SLOTS;
		    slot++)
			status = fsimg_disk_plan_partition(&plan, slot, &layers,
			    NULL, NULL, &error);
		CHECK(status == FSIMG_EINVAL && tri.partition_calls == failing);
		fsimg_disk_plan_free(&plan);
		fsimg_disk_plan_free(&plan);
		CHECK(plan.table.records == NULL && plan.table.count == 0);
		CHECK(memory_in_use(ctx) == baseline);
	}

	tri_reset();
	tri.cap_bytes = 0;
	CHECK(tri_plan(ctx, NULL, NULL, TRI_SLOTS, &plan, &error) == FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_EINVAL);
	fsimg_disk_plan_free(&plan);
	CHECK(memory_in_use(ctx) == baseline);
	fsimg_ctx_free(ctx);
}

/* Without a scheme no partition planner runs: the plan refuses a partition
 * and its report names no scheme.
 */
static void
test_unpartitioned_plan_has_no_planner(void)
{
	const uint64_t lengths[1] = { 4096 };
	fsimg_layer_options_t layers;
	fsimg_image_info_t image;
	fsimg_partition_info_t partition;
	fsimg_disk_plan_t plan;
	fsimg_error_t error;
	fsimg_ctx_t *ctx = context_new();

	fsimg_layer_options_init(&layers);
	CHECK(scheme_plan(ctx, "fat16", &layers, FSIMG_WRITE_REPRODUCIBLE, 1,
	    &plan, &error) == FSIMG_OK);
	CHECK(plan.scheme == NULL && plan.table.records == NULL);
	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, "fat16",
	    &error) == FSIMG_ESTATE);
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_OK);
	fsimg_disk_plan_report(&plan, 1, &image, &partition);
	CHECK(image.scheme[0] == '\0' && image.disk_bytes == 4096 &&
	    !partition.used);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

/* A compose part's strings and GUID are borrowed for the call alone: the
 * record holds copies that survive the caller overwriting them.
 */
static void
test_records_retain_no_request_pointers(void)
{
	char type[8], name[8];
	unsigned char guid[16];
	fsimg_partition_request_t part = { .type = type, .name = name,
	    .guid = guid };
	const fsimg_partition_record_t *record;
	fsimg_layer_options_t layers;
	fsimg_disk_plan_t plan;
	fsimg_ctx_t *ctx = context_new();

	snprintf(type, sizeof(type), "linux");
	snprintf(name, sizeof(name), "Root");
	memset(guid, 0x11, sizeof(guid));
	fsimg_layer_options_init(&layers);
	CHECK(scheme_plan(ctx, "gpt", &layers, FSIMG_WRITE_REPRODUCIBLE, 1,
	    &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 1, NULL, &part, NULL, NULL) ==
	    FSIMG_OK);
	memset(type, 'x', sizeof(type));
	memset(name, 'x', sizeof(name));
	memset(guid, 0x22, sizeof(guid));
	record = &plan.table.records[0];
	CHECK(record->name[0] == 'R' && record->name[3] == 't' &&
	    record->name[4] == 0);
	CHECK(record->guid[0] == 0x11 && record->guid[15] == 0x11);
	CHECK(record->type_guid[0] == 0x0f && record->type_guid[1] == 0xc6);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

/*
 * GPT's disk resolver: 512-byte sectors by default, 4096 when asked, any
 * other size refused at gpt:sector; 1 and 128 partitions accepted, 0 and
 * 129 refused with the 128-slot limit, which is the write table's and not
 * the 1024 entries a reader accepts.
 */
static void
test_gpt_sector_and_count(void)
{
	static const struct {
		uint32_t	 requested, resolved;
	} sectors[] = { { 0, 512 }, { 512, 512 }, { 4096, 4096 }, { 1024, 0 } };
	static const struct {
		uint32_t	 count;
		bool		 accepted;
	} counts[] = { { 0, false }, { 1, true }, { 128, true }, { 129, false } };
	fsimg_layer_options_t layers;
	fsimg_disk_plan_t plan;
	fsimg_error_t error;
	fsimg_status_t status;
	fsimg_ctx_t *ctx = context_new();

	for (size_t i = 0; i < sizeof(sectors) / sizeof(sectors[0]); i++) {
		fsimg_layer_options_init(&layers);
		layers.gpt_sector_bytes = sectors[i].requested;
		status = scheme_plan(ctx, "gpt", &layers,
		    FSIMG_WRITE_REPRODUCIBLE, 1, &plan, &error);
		if (sectors[i].resolved == 0) {
			CHECK(status == FSIMG_EINVAL);
			CHECK(strcmp(error.path, "gpt:sector") == 0);
		} else {
			CHECK(status == FSIMG_OK);
			CHECK(plan.table.sector_bytes == sectors[i].resolved);
			CHECK(strcmp(plan.disk_key, "gpt:disk") == 0);
		}
		fsimg_disk_plan_free(&plan);
	}
	for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
		fsimg_layer_options_init(&layers);
		status = scheme_plan(ctx, "gpt", &layers,
		    FSIMG_WRITE_REPRODUCIBLE, counts[i].count, &plan, &error);
		CHECK((status == FSIMG_OK) == counts[i].accepted);
		if (!counts[i].accepted)
			CHECK(error.limit == 128 && error.observed == counts[i].count);
		fsimg_disk_plan_free(&plan);
	}
	fsimg_ctx_free(ctx);
}

/*
 * GPT names: the type's conventional name by default, 36 UTF-16 units
 * accepted and 37 refused, a supplementary character counting two units,
 * malformed UTF-8 refused, and the rest of the field zero.
 */
static void
test_gpt_names(void)
{
	/* U+1F600, four UTF-8 bytes and a surrogate pair. */
	static const char smile[] = "\xf0\x9f\x98\x80";
	static const struct {
		unsigned	 ascii, supplementary;
		bool		 accepted;
	} lengths[] = { { 36, 0, true }, { 37, 0, false }, { 0, 18, true },
	    { 1, 18, false }, { 34, 1, true } };
	const char *basic = "Microsoft basic data";
	const fsimg_partition_record_t *record;
	fsimg_layer_options_t layers;
	fsimg_disk_plan_t plan;
	fsimg_error_t error;
	fsimg_status_t status;
	size_t used;
	fsimg_ctx_t *ctx = context_new();

	fsimg_layer_options_init(&layers);
	CHECK(scheme_plan(ctx, "gpt", &layers, FSIMG_WRITE_REPRODUCIBLE, 1,
	    &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, "fat16",
	    &error) == FSIMG_OK);
	record = &plan.table.records[0];
	for (size_t i = 0; i < 36; i++)
		CHECK(record->name[i] == (i < strlen(basic) ?
		    (uint16_t)basic[i] : 0));
	fsimg_disk_plan_free(&plan);

	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		fsimg_layer_options_init(&layers);
		used = 0;
		for (unsigned c = 0; c < lengths[i].ascii; c++)
			layers.gpt_name[used++] = 'n';
		for (unsigned c = 0; c < lengths[i].supplementary; c++) {
			memcpy(layers.gpt_name + used, smile, 4);
			used += 4;
		}
		CHECK(scheme_plan(ctx, "gpt", &layers,
		    FSIMG_WRITE_REPRODUCIBLE, 1, &plan, NULL) == FSIMG_OK);
		status = fsimg_disk_plan_partition(&plan, 1, &layers, NULL,
		    "fat16", &error);
		CHECK((status == FSIMG_OK) == lengths[i].accepted);
		if (!lengths[i].accepted)
			CHECK(status == FSIMG_EINVAL &&
			    strcmp(error.path, "gpt:name") == 0);
		record = &plan.table.records[0];
		if (lengths[i].accepted && lengths[i].supplementary == 1)
			CHECK(record->name[34] == 0xd83d &&
			    record->name[35] == 0xde00);
		if (lengths[i].accepted && lengths[i].ascii == 0)
			CHECK(record->name[0] == 0xd83d && record->name[1] == 0xde00);
		fsimg_disk_plan_free(&plan);
	}

	fsimg_layer_options_init(&layers);
	snprintf(layers.gpt_name, sizeof(layers.gpt_name), "bad\xc3");
	CHECK(scheme_plan(ctx, "gpt", &layers, FSIMG_WRITE_REPRODUCIBLE, 1,
	    &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, "fat16",
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "gpt:name") == 0);
	fsimg_disk_plan_free(&plan);

	fsimg_layer_options_init(&layers);
	snprintf(layers.gpt_name, sizeof(layers.gpt_name), "Hi");
	CHECK(scheme_plan(ctx, "gpt", &layers, FSIMG_WRITE_REPRODUCIBLE, 1,
	    &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, "fat16",
	    NULL) == FSIMG_OK);
	record = &plan.table.records[0];
	CHECK(record->name[0] == 'H' && record->name[1] == 'i');
	for (size_t i = 2; i < 36; i++)
		CHECK(record->name[i] == 0);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

/*
 * GPT identities: explicit GUIDs kept as given; unset ones derived in
 * reproducible mode and refused outside it, naming gpt:disk-guid, gpt:guid
 * or the compose part; the derivation spelled out byte by byte for a
 * negative epoch and slots 1 and 128; and duplicate partition GUIDs or a
 * partition GUID equal to the disk's refused when the plan is placed,
 * naming the partitions.
 */
static void
test_gpt_identities(void)
{
	static const unsigned char negative_slot_128[16] = {
		0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0x4f, 0xff,
		0x82, 0x80, 0, 0, 0, 0, 0, 0 };
	const char *given = "11111111-2222-4333-8444-555555555555";
	fsimg_partition_request_t part = { .type = "linux" };
	unsigned char guid[16];
	uint64_t lengths[128];
	fsimg_layer_options_t layers;
	fsimg_disk_plan_t plan;
	fsimg_disk_request_t request;
	fsimg_session_t session;
	fsimg_stack_t stack;
	fsimg_error_t error;
	fsimg_ctx_t *ctx = context_new();

	fsimg_layer_options_init(&layers);
	CHECK(scheme_plan(ctx, "gpt", &layers, 0, 1, &plan, &error) ==
	    FSIMG_EINVAL);
	CHECK(strcmp(error.path, "gpt:disk-guid") == 0);
	fsimg_disk_plan_free(&plan);

	guid_text(given, layers.gpt_disk_guid);
	CHECK(scheme_plan(ctx, "gpt", &layers, 0, 2, &plan, NULL) == FSIMG_OK);
	CHECK(bcmp(plan.table.disk_guid, layers.gpt_disk_guid, 16) == 0);
	CHECK(fsimg_disk_plan_partition(&plan, 2, NULL, &part, NULL,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "partition 2") == 0);
	fsimg_layer_options_init(&layers);
	guid_text(given, layers.gpt_disk_guid);
	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, "fat16",
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "gpt:guid") == 0);
	fsimg_disk_plan_free(&plan);

	/* Explicit identifiers survive reproducible mode. */
	fsimg_layer_options_init(&layers);
	guid_text(given, layers.gpt_disk_guid);
	guid_text("66666666-7777-4888-8999-aaaaaaaaaaaa", layers.gpt_guid);
	CHECK(scheme_plan(ctx, "gpt", &layers, FSIMG_WRITE_REPRODUCIBLE, 1,
	    &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, "fat16",
	    NULL) == FSIMG_OK);
	CHECK(bcmp(plan.table.disk_guid, layers.gpt_disk_guid, 16) == 0);
	CHECK(bcmp(plan.table.records[0].guid, layers.gpt_guid, 16) == 0);
	fsimg_disk_plan_free(&plan);

	/* 128 derived GUIDs at a negative epoch are distinct and place. */
	CHECK(fsimg_stack_parse("gpt", &stack, NULL) == FSIMG_OK);
	fsimg_layer_options_init(&layers);
	fsimg_session_init(&session, ctx, "test", 0);
	request = (fsimg_disk_request_t){ .session = &session,
	    .stack = &stack, .layers = &layers, .compression = "",
	    .epoch = -5, .flags = FSIMG_WRITE_REPRODUCIBLE, .partitions = 128 };
	CHECK(fsimg_disk_plan_init(ctx, &request, &plan, NULL) == FSIMG_OK);
	part.guid = NULL;
	for (uint32_t slot = 1; slot <= 128; slot++) {
		CHECK(fsimg_disk_plan_partition(&plan, slot, NULL, &part, NULL,
		    NULL) == FSIMG_OK);
		lengths[slot - 1] = 512;
	}
	CHECK(bcmp(plan.table.records[127].guid, negative_slot_128, 16) == 0);
	CHECK(plan.table.records[0].guid[9] == 1 &&
	    plan.table.records[0].guid[8] == 0x82);
	CHECK(plan.table.disk_guid[8] == 0x81 && plan.table.disk_guid[9] == 0);
	CHECK(fsimg_disk_plan_place(&plan, lengths, NULL) == FSIMG_OK);
	fsimg_disk_plan_free(&plan);

	/* Duplicates, and a partition carrying the disk GUID. */
	fsimg_layer_options_init(&layers);
	guid_text(given, layers.gpt_disk_guid);
	guid_text("66666666-7777-4888-8999-aaaaaaaaaaaa", guid);
	part.guid = guid;
	CHECK(scheme_plan(ctx, "gpt", &layers, 0, 2, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 1, NULL, &part, NULL, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 2, NULL, &part, NULL, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "partitions 1 and 2") != NULL);
	fsimg_disk_plan_free(&plan);
	part.guid = layers.gpt_disk_guid;
	CHECK(scheme_plan(ctx, "gpt", &layers, 0, 1, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 1, NULL, &part, NULL, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "partition 1 has the disk GUID") != NULL);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

/*
 * MBR metadata: an explicit zero disk identifier kept, an absent one
 * refused outside reproducible mode at mbr:id; the first-byte defaults of
 * FAT16, FAT32, QNX6 and QNX4; a GUID literal, a type without a byte and
 * the extended and protective bytes refused at mbr:type; a compose part's
 * name or GUID refused naming the part; the active flag kept from the
 * layer options.
 */
static void
test_mbr_metadata(void)
{
	static const struct {
		const char	*filesystem;
		uint8_t		 byte;
	} defaults[] = { { "fat16", 0x06 }, { "fat32", 0x0c }, { "qnx6", 0xb2 },
	    { "qnx4", 0x4d }, { "exfat", 0x07 } };
	static const char *const refused[] = { "apfs",
	    "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "0x05", "0x0f", "0x85",
	    "0xee", "0x00" };
	const unsigned char guid[16] = { 1 };
	fsimg_partition_request_t part = { .type = "linux" };
	fsimg_layer_options_t layers;
	fsimg_disk_plan_t plan;
	fsimg_error_t error;
	fsimg_ctx_t *ctx = context_new();

	fsimg_layer_options_init(&layers);
	CHECK(scheme_plan(ctx, "mbr", &layers, 0, 1, &plan, &error) ==
	    FSIMG_EINVAL);
	CHECK(strcmp(error.path, "mbr:id") == 0);
	fsimg_disk_plan_free(&plan);
	layers.mbr_id_set = true;
	CHECK(scheme_plan(ctx, "mbr", &layers, FSIMG_WRITE_REPRODUCIBLE, 1,
	    &plan, NULL) == FSIMG_OK);
	CHECK(plan.table.mbr_disk_id == 0 && plan.table.sector_bytes == 512);
	CHECK(strcmp(plan.disk_key, "mbr:disk") == 0);
	fsimg_disk_plan_free(&plan);

	for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
		fsimg_layer_options_init(&layers);
		CHECK(scheme_plan(ctx, "mbr", &layers,
		    FSIMG_WRITE_REPRODUCIBLE, 1, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL,
		    defaults[i].filesystem, NULL) == FSIMG_OK);
		CHECK(plan.table.records[0].mbr_type == defaults[i].byte);
		CHECK(!plan.table.records[0].active);
		fsimg_disk_plan_free(&plan);
	}
	for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		fsimg_layer_options_init(&layers);
		snprintf(layers.mbr_type, sizeof(layers.mbr_type), "%s",
		    refused[i]);
		CHECK(scheme_plan(ctx, "mbr", &layers,
		    FSIMG_WRITE_REPRODUCIBLE, 1, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, "fat16",
		    &error) == FSIMG_EINVAL);
		CHECK(strcmp(error.path, "mbr:type") == 0);
		CHECK(!plan.table.records[0].used);
		fsimg_disk_plan_free(&plan);
	}

	fsimg_layer_options_init(&layers);
	layers.mbr_active = true;
	CHECK(scheme_plan(ctx, "mbr", &layers, FSIMG_WRITE_REPRODUCIBLE, 2,
	    &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, NULL,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "mbr:type") == 0);
	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, "fat32",
	    NULL) == FSIMG_OK);
	CHECK(plan.table.records[0].active &&
	    plan.table.records[0].mbr_type == 0x0c);
	CHECK(fsimg_disk_plan_partition(&plan, 2, NULL, &part, NULL, NULL) ==
	    FSIMG_OK);
	CHECK(!plan.table.records[1].active &&
	    plan.table.records[1].mbr_type == 0x83);
	part.name = "named";
	CHECK(fsimg_disk_plan_partition(&plan, 2, NULL, &part, NULL,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "partition 2") == 0);
	part.name = "";
	part.guid = guid;
	CHECK(fsimg_disk_plan_partition(&plan, 2, NULL, &part, NULL,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "partition 2") == 0);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

/*
 * MBR's addressable limit, 2^32 sectors: a fixed disk of exactly that is
 * accepted and one sector more refused at mbr:disk, before any partition
 * is resolved; an automatic disk is checked once placed, so a volume that
 * ends the disk at 2^32 sectors places and one sector more is refused.
 * Placement allocates no disk, so these lengths cost nothing.
 */
static void
test_mbr_limits(void)
{
	const uint64_t limit = (UINT64_C(1) << 32) * 512;
	uint64_t lengths[1];
	fsimg_layer_options_t layers;
	fsimg_disk_plan_t plan;
	fsimg_error_t error;
	fsimg_ctx_t *ctx = context_new();

	fsimg_layer_options_init(&layers);
	layers.mbr_disk_bytes = limit;
	CHECK(scheme_plan(ctx, "mbr", &layers, FSIMG_WRITE_REPRODUCIBLE, 1,
	    &plan, NULL) == FSIMG_OK);
	fsimg_disk_plan_free(&plan);
	layers.mbr_disk_bytes = limit + 512;
	CHECK(scheme_plan(ctx, "mbr", &layers, FSIMG_WRITE_REPRODUCIBLE, 1,
	    &plan, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "mbr:disk") == 0);
	CHECK(plan.table.records == NULL);
	fsimg_disk_plan_free(&plan);

	fsimg_layer_options_init(&layers);
	CHECK(scheme_plan(ctx, "mbr", &layers, FSIMG_WRITE_REPRODUCIBLE, 1,
	    &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_disk_plan_partition(&plan, 1, &layers, NULL, "fat32",
	    NULL) == FSIMG_OK);
	lengths[0] = limit - MIB;
	CHECK(fsimg_disk_plan_place(&plan, lengths, NULL) == FSIMG_OK);
	CHECK(plan.disk_bytes == limit);
	lengths[0] += 512;
	CHECK(fsimg_disk_plan_place(&plan, lengths, &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "2^32 sectors") != NULL);
	fsimg_disk_plan_free(&plan);
	fsimg_ctx_free(ctx);
}

int
main(void)
{
	test_registry_write_sets();
	test_capabilities_follow_write_set();
	test_read_only_scheme_refused_before_callbacks();
	test_descriptor_supplies_disk_defaults();
	test_partition_sources_and_keys();
	test_placement_follows_footprint();
	test_final_validation_after_rounding();
	test_fixed_disk_uses_descriptor_key();
	test_large_footprints_are_checked();
	test_report_is_descriptor_owned();
	test_emission_uses_descriptor_encoder();
	test_cleanup_at_every_boundary();
	test_unpartitioned_plan_has_no_planner();
	test_records_retain_no_request_pointers();
	test_gpt_sector_and_count();
	test_gpt_names();
	test_gpt_identities();
	test_mbr_metadata();
	test_mbr_limits();
	return (0);
}
