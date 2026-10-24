/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "io.h"
#include "layer.h"
#include "publish.h"

/*
 * Layered output. Every disk is laid out once, as an assembled disk: a
 * sequence of regions that covers it in order, each a run of table bytes
 * in charged memory, a volume or a run of zeros. Wrap, unwrap and compose
 * stage nothing: their assembled disk is a read-only object over its
 * regions, which the transcode (fsimg_disk_output) reads once, as it reads
 * a writer's raw intermediate, so the output path has one input type. A
 * writer's disk is staged raw and filled in place from the same regions in
 * disk order (fsimg_disk_plan_emit).
 */

typedef enum region_kind {
	REGION_ZERO,
	REGION_TABLE,
	REGION_VOLUME
} region_kind_t;

/* A TABLE region's bytes, or the VOLUME read from its offset zero; a
 * writer's volume, which its driver emits in place, has none.
 */
typedef struct disk_region {
	region_kind_t	 kind;
	uint64_t	 offset;
	uint64_t	 length;
	const unsigned char *bytes;
	fsimg_io_t	*volume;
} disk_region_t;

/* Regions in disk order, contiguous from offset zero to length. session
 * is borrowed from the operation: the same session that charged the disk
 * plan's construction charges the volume reads of wrap, unwrap and
 * compose. A writer's in-place emission reads no volume through it.
 */
typedef struct assembled_disk {
	disk_region_t	*regions;
	size_t		 count;
	size_t		 capacity;
	uint64_t	 length;
	bool		 durable;
	fsimg_session_t	*session;
} assembled_disk_t;

static fsimg_status_t check_mode(const fsimg_stack_t *,
    const fsimg_write_options_t *, fsimg_write_options_t *,
    fsimg_layer_options_t *, fsimg_layer_inputs_t *, fsimg_error_t *);
static fsimg_status_t check_named_input(fsimg_ctx_t *, fsimg_io_t *,
    uint64_t, const fsimg_stack_t *, char [static 16], fsimg_error_t *);
static fsimg_status_t check_carried_input(fsimg_ctx_t *, fsimg_io_t *,
    uint64_t, const fsimg_stack_t *, const fsimg_layer_t *, char [static 16],
    fsimg_byte_order_t *, fsimg_error_t *);
static fsimg_status_t disk_new(fsimg_session_t *, size_t, bool,
    assembled_disk_t *, fsimg_error_t *);
static fsimg_status_t disk_append(assembled_disk_t *, region_kind_t,
    uint64_t, uint64_t, const unsigned char *, fsimg_io_t *, fsimg_error_t *);
static fsimg_status_t disk_finish(assembled_disk_t *, uint64_t,
    fsimg_error_t *);
static fsimg_status_t assemble_plan(assembled_disk_t *,
    const fsimg_disk_plan_t *, const fsimg_scheme_tables_t *,
    fsimg_io_t *const *, const uint64_t *, fsimg_error_t *);
static fsimg_status_t build_disk(fsimg_session_t *, const fsimg_disk_plan_t *,
    fsimg_io_t *const *, const uint64_t *, bool, assembled_disk_t *,
    fsimg_scheme_tables_t *, fsimg_error_t *);
static fsimg_status_t reject_aliases(const assembled_disk_t *,
    const fsimg_disk_plan_t *, const fsimg_sink_t *, fsimg_error_t *);
static fsimg_status_t disk_resolve(void *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t disk_next_zero(void *, uint64_t, uint64_t *, uint64_t *,
    fsimg_error_t *);
static void disk_keep(void *);
static fsimg_status_t identify_volume(fsimg_ctx_t *, fsimg_io_t *, uint64_t,
    const char *, char [static 16], fsimg_error_t *);
static fsimg_status_t write_plan(fsimg_session_t *, fsimg_disk_plan_t *,
    fsimg_io_t *const *, const uint64_t *, bool, const fsimg_sink_t *,
    fsimg_error_t *);

static const fsimg_io_resolver_ops_t disk_ops = {
	.resolve = disk_resolve,
	.next_zero = disk_next_zero,
	.release = disk_keep
};

/*
 * Fill a writer's staged disk in place from its assembled regions, in one pass
 * in disk order: a ZERO region is zeroed, emit writes the volume into a
 * checked slice, and a table is written after a cancellation poll. On a fresh
 * staged disk the zeroing writes nothing, since its record proves every range
 * not written zero whatever the order of the writes, so a fixed disk's long
 * gaps stay holes; any other output is zeroed in full.
 */
fsimg_status_t
fsimg_disk_plan_emit(const fsimg_disk_plan_t *plan, uint64_t volume_bytes,
    fsimg_io_t *output, fsimg_session_t *session, fsimg_volume_emit_t emit,
    void *argument, fsimg_error_t *error)
{
	/* The driver writes the volume in place, so no input backs it. */
	fsimg_io_t *const no_input[1] = { NULL };
	assembled_disk_t disk;
	fsimg_scheme_tables_t tables;
	const disk_region_t *region;
	fsimg_io_t *volume;
	size_t transferred;
	fsimg_status_t status;

	status = build_disk(session, plan, no_input, &volume_bytes, false,
	    &disk, &tables, error);
	for (size_t i = 0; status == FSIMG_OK && i < disk.count; i++) {
		region = &disk.regions[i];
		switch (region->kind) {
		case REGION_TABLE:
			status = fsimg_poll(session->ctx, NULL, error);
			if (status == FSIMG_OK)
				status = fsimg_io_write_session(output, session,
				    region->offset, region->bytes, region->length,
				    &transferred, error);
			break;
		case REGION_VOLUME:
			status = fsimg_io_slice(output, region->offset,
			    region->length, &volume, error);
			if (status != FSIMG_OK)
				break;
			status = emit(argument, volume, error);
			fsimg_io_free(volume);
			break;
		case REGION_ZERO:
			status = fsimg_io_zero_session(output, session,
			    region->offset, region->length, error);
			break;
		}
	}
	fsimg_scheme_tables_free(&tables);
	fsimg_dealloc(disk.regions);
	return (status);
}

/*
 * Without a filesystem writer, write options carry only the mode: the
 * reproducible, durable and loss flags, the epoch, the layer settings, the
 * file settings and the compression.
 * Every other field must hold its default, as a setting a format does not
 * store must, so none is silently ignored: the catalogue's filesystem
 * settings, and each ordinary volume field compared by value. Every
 * member of the record is one of these or a mode field; test/settings.c
 * keeps that list complete. selected is
 * the defaults with the caller's flags, epoch and compression, which the
 * disk plan resolves, and layers and inputs copies of the caller's
 * records, so selected carries no pointer to either. Shared layer checks
 * precede input selection, probing and verification for every output mode.
 */
static fsimg_status_t
check_mode(const fsimg_stack_t *stack, const fsimg_write_options_t *options,
    fsimg_write_options_t *selected, fsimg_layer_options_t *layers,
    fsimg_layer_inputs_t *inputs, fsimg_error_t *error)
{
	fsimg_write_options_t defaults;
	const fsimg_layer_t *compression;
	fsimg_status_t status;
	bool volume_defaults;

	fsimg_write_options_init(&defaults);
	*selected = defaults;
	fsimg_layer_options_init(layers);
	bzero(inputs, sizeof(*inputs));
	if (options == NULL)
		return (FSIMG_OK);
	if (options->size != sizeof(*options) ||
	    options->version != FSIMG_OPTIONS_VERSION ||
	    (options->flags & ~(FSIMG_WRITE_ALLOW_LOSS | FSIMG_WRITE_REPRODUCIBLE |
	    FSIMG_WRITE_DURABLE)) != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "unsupported write options size, version or flags"));
	status = fsimg_write_settings_check(options, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	volume_defaults = options->volume_bytes == defaults.volume_bytes &&
	    options->min_free_bytes == defaults.min_free_bytes &&
	    options->label == defaults.label &&
	    options->fat_root_entries == defaults.fat_root_entries &&
	    options->volume_serial == defaults.volume_serial;
	for (unsigned i = 0; i < 4; i++)
		volume_defaults = volume_defaults && options->reserved[i] == 0;
	if (!volume_defaults)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "filesystem settings must hold their defaults without a "
		    "filesystem writer"));
	selected->flags = options->flags;
	selected->epoch = options->epoch;
	if (options->layers != NULL)
		*layers = *options->layers;
	if (options->inputs != NULL)
		*inputs = *options->inputs;
	bcopy(options->compression, selected->compression,
	    sizeof(selected->compression));
	status = fsimg_compression_named(selected->compression,
	    sizeof(selected->compression), true, &compression, error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_layer_options_check(stack, compression, layers, inputs,
	    true, error));
}

/* An empty disk with room for capacity regions, charging its copy to the
 * operation's session.
 */
static fsimg_status_t
disk_new(fsimg_session_t *session, size_t capacity, bool durable,
    assembled_disk_t *disk, fsimg_error_t *error)
{
	bzero(disk, sizeof(*disk));
	disk->durable = durable;
	disk->session = session;
	disk->regions = fsimg_alloc(session->ctx,
	    capacity * sizeof(*disk->regions), error);
	if (disk->regions == NULL)
		return (error->status);
	disk->capacity = capacity;
	return (FSIMG_OK);
}

/* Append one region at offset, after a run of zeros to reach it. Regions
 * arrive in disk order; one that would overlap its predecessor is an
 * internal error.
 */
static fsimg_status_t
disk_append(assembled_disk_t *disk, region_kind_t kind, uint64_t offset,
    uint64_t length, const unsigned char *bytes, fsimg_io_t *volume,
    fsimg_error_t *error)
{
	if (offset < disk->length || length > UINT64_MAX - offset ||
	    disk->capacity - disk->count < 2)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "assembled disk regions are out of order"));
	if (offset > disk->length)
		disk->regions[disk->count++] = (disk_region_t){ .kind = REGION_ZERO,
		    .offset = disk->length, .length = offset - disk->length };
	if (length != 0)
		disk->regions[disk->count++] = (disk_region_t){ .kind = kind,
		    .offset = offset, .length = length, .bytes = bytes,
		    .volume = volume };
	disk->length = offset + length;
	return (FSIMG_OK);
}

/* Zeros from the last region to the end of the disk. */
static fsimg_status_t
disk_finish(assembled_disk_t *disk, uint64_t length, fsimg_error_t *error)
{
	return (disk_append(disk, REGION_ZERO, length, 0, NULL, NULL, error));
}

/* The placed plan's disk: the leading tables, each volume at its
 * partition's offset, the trailing tables, and zeros everywhere between,
 * including a partition's padding past its volume.
 */
static fsimg_status_t
assemble_plan(assembled_disk_t *disk, const fsimg_disk_plan_t *plan,
    const fsimg_scheme_tables_t *tables, fsimg_io_t *const *volumes,
    const uint64_t *volume_bytes, fsimg_error_t *error)
{
	const fsimg_table_region_t *region;
	fsimg_status_t status;

	region = &tables->regions[0];
	status = disk_append(disk, REGION_TABLE, region->offset, region->length,
	    region->bytes, NULL, error);
	for (uint32_t i = 0; status == FSIMG_OK && i < plan->table.count; i++)
		status = disk_append(disk, REGION_VOLUME,
		    plan->table.records[i].offset, volume_bytes[i], NULL,
		    volumes[i], error);
	for (unsigned i = 1; status == FSIMG_OK && i < tables->count; i++) {
		region = &tables->regions[i];
		status = disk_append(disk, REGION_TABLE, region->offset,
		    region->length, region->bytes, NULL, error);
	}
	if (status == FSIMG_OK)
		status = disk_finish(disk, plan->disk_bytes, error);
	return (status);
}

/* A placed plan's assembled disk: its tables encoded and its regions in
 * disk order, the volume regions reading volumes[i]. Without a scheme the
 * one volume starts the disk, followed by the container's padding. The
 * caller frees tables and disk->regions on every outcome.
 */
static fsimg_status_t
build_disk(fsimg_session_t *session, const fsimg_disk_plan_t *plan,
    fsimg_io_t *const *volumes, const uint64_t *volume_bytes, bool durable,
    assembled_disk_t *disk, fsimg_scheme_tables_t *tables,
    fsimg_error_t *error)
{
	fsimg_ctx_t *ctx = session->ctx;
	size_t count;
	fsimg_status_t status;

	bzero(tables, sizeof(*tables));
	count = plan->scheme == NULL ? 1 : plan->table.count;
	/* Each volume and table may follow a run of zeros, and so may the
	 * disk's end.
	 */
	status = disk_new(session, 2 * (count + FSIMG_TABLE_REGIONS) + 1,
	    durable, disk, error);
	if (status != FSIMG_OK)
		return (status);
	if (plan->scheme == NULL) {
		status = disk_append(disk, REGION_VOLUME, 0, volume_bytes[0], NULL,
		    volumes[0], error);
		if (status == FSIMG_OK)
			status = disk_finish(disk, plan->disk_bytes, error);
		return (status);
	}
	status = plan->scheme->scheme->encode(ctx, &plan->table, plan->disk_bytes,
	    tables, error);
	if (status == FSIMG_OK)
		status = assemble_plan(disk, plan, tables, volumes, volume_bytes,
		    error);
	return (status);
}

/* No input, a file setting included, may share its storage with the
 * output, through any slice or name, before the output is touched.
 */
static fsimg_status_t
reject_aliases(const assembled_disk_t *disk, const fsimg_disk_plan_t *plan,
    const fsimg_sink_t *sink, fsimg_error_t *error)
{
	fsimg_host_file_t output, input;
	bool output_known, input_known;
	fsimg_status_t status;

	status = fsimg_sink_identity(sink, &output, &output_known, error);
	if (status != FSIMG_OK || !output_known)
		return (status);
	status = fsimg_disk_plan_input_aliases(plan, &output, error);
	if (status != FSIMG_OK)
		return (status);
	for (size_t i = 0; i < disk->count; i++) {
		if (disk->regions[i].kind != REGION_VOLUME)
			continue;
		status = fsimg_io_identity(disk->regions[i].volume, &input,
		    &input_known, error);
		if (status != FSIMG_OK)
			return (status);
		if (input_known && input.device == output.device &&
		    input.inode == output.inode)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "output aliases an input"));
	}
	return (FSIMG_OK);
}

/*
 * The assembled disk's bytes at offset: table bytes from memory, zeros,
 * and volume bytes read through the disk's session, region by region.
 * The object's bounds keep a request within the disk.
 */
static fsimg_status_t
disk_resolve(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *done, fsimg_error_t *error)
{
	assembled_disk_t *disk = argument;
	unsigned char *out = buffer;
	const disk_region_t *region;
	uint64_t within;
	size_t step, transferred;
	fsimg_status_t status;

	*done = 0;
	status = FSIMG_OK;
	for (size_t i = 0; status == FSIMG_OK && i < disk->count &&
	    *done < length; i++) {
		region = &disk->regions[i];
		if (offset + *done >= region->offset + region->length)
			continue;
		within = offset + *done - region->offset;
		step = region->length - within < length - *done ?
		    (size_t)(region->length - within) : length - *done;
		switch (region->kind) {
		case REGION_ZERO:
			bzero(out + *done, step);
			break;
		case REGION_TABLE:
			bcopy(region->bytes + within, out + *done, step);
			break;
		case REGION_VOLUME:
			status = fsimg_io_read_session(region->volume,
			    disk->session, within, out + *done, step,
			    &transferred, error);
			break;
		}
		if (status == FSIMG_OK)
			*done += step;
	}
	return (status);
}

/*
 * The assembled disk's next zero range, from the region containing offset: a
 * zero gap is zero to its end, a table is the empty answer at its end, and a
 * volume answers as its input does, translated into the disk; the input's
 * answer already lies within the region, whose length is the input's. Regions
 * are few, a scheme's tables and volumes, so the lookup scans.
 */
static fsimg_status_t
disk_next_zero(void *argument, uint64_t offset, uint64_t *start,
    uint64_t *end, fsimg_error_t *error)
{
	const assembled_disk_t *disk = argument;
	const disk_region_t *region;
	uint64_t low, high;
	fsimg_status_t status;
	unsigned input;
	size_t i;
	char name[32];

	/* The regions cover the disk, and the offset is below its length. */
	input = 0;
	for (i = 0; i + 1 < disk->count &&
	    offset >= disk->regions[i].offset + disk->regions[i].length; i++)
		input += disk->regions[i].kind == REGION_VOLUME;
	region = &disk->regions[i];
	*start = *end = region->offset + region->length;
	switch (region->kind) {
	case REGION_TABLE:
		break;
	case REGION_VOLUME:
		status = fsimg_io_next_zero_range(region->volume,
		    offset - region->offset, &low, &high, error);
		if (status != FSIMG_OK) {
			/* Name the input, counting from 1 in disk order. */
			if (error != NULL && error->path[0] == '\0') {
				snprintf(name, sizeof(name), "input %u",
				    input + 1);
				fsimg_error_path(error, name);
			}
			return (status);
		}
		/* The input is the region's length; clip in case it is not. */
		high = high < region->length ? high : region->length;
		low = low < high ? low : high;
		*start = region->offset + low;
		*end = region->offset + high;
		break;
	case REGION_ZERO:
		*start = offset;
		break;
	}
	return (FSIMG_OK);
}

/* The object borrows the disk, which write_plan frees. */
static void
disk_keep(void *argument FSIMG_UNUSED)
{
}

/* The filesystem a volume's prefix identifies, forced or detected; name is
 * empty when no single format claims it.
 */
static fsimg_status_t
identify_volume(fsimg_ctx_t *ctx, fsimg_io_t *volume, uint64_t length,
    const char *forced, char name[static 16], fsimg_error_t *error)
{
	fsimg_session_t session;
	fsimg_claim_t claim;
	fsimg_status_t status;

	fsimg_session_init(&session, ctx, "volume identification", 0);
	status = fsimg_volume_claim(ctx, volume, &session, 0, length, forced,
	    &claim, name, error);
	if (status == FSIMG_OK && claim != FSIMG_CLAIM_FORMAT)
		name[0] = '\0';
	return (status);
}

/*
 * Place a resolved plan's volumes, encode its tables, check the output
 * limit and the sink against every input, and transcode the assembled
 * disk to the sink through the plan's container and compression, or copy
 * it without either, all charged to the session that made the plan.
 */
static fsimg_status_t
write_plan(fsimg_session_t *session, fsimg_disk_plan_t *plan,
    fsimg_io_t *const *volumes, const uint64_t *volume_bytes, bool durable,
    const fsimg_sink_t *sink, fsimg_error_t *error)
{
	fsimg_ctx_t *ctx = session->ctx;
	assembled_disk_t disk;
	fsimg_scheme_tables_t tables;
	fsimg_io_t *object;
	uint64_t written;
	fsimg_status_t status;

	object = NULL;
	status = fsimg_disk_plan_place(plan, volume_bytes, error);
	if (status == FSIMG_OK)
		status = fsimg_disk_plan_budget(ctx, plan, false, error);
	if (status != FSIMG_OK)
		return (status);
	status = build_disk(session, plan, volumes, volume_bytes, durable,
	    &disk, &tables, error);
	if (status == FSIMG_OK)
		status = reject_aliases(&disk, plan, sink, error);
	if (status == FSIMG_OK)
		status = fsimg_io_resolver(ctx, NULL, disk.length, &disk_ops,
		    &disk, &object, error);
	if (status == FSIMG_OK)
		status = fsimg_disk_output(session, object, plan, sink, durable,
		    &written, error);
	fsimg_io_free(object);
	fsimg_scheme_tables_free(&tables);
	fsimg_dealloc(disk.regions);
	return (status);
}

/* The filesystem a wrap's input holds, empty when no single format claims
 * it; one the stack names must be the one it holds, else FSIMG_EINVAL
 * naming the filesystem.
 */
static fsimg_status_t
check_named_input(fsimg_ctx_t *ctx, fsimg_io_t *input, uint64_t length,
    const fsimg_stack_t *stack, char filesystem[static 16],
    fsimg_error_t *error)
{
	const fsimg_stack_slot_t *slot = &stack->slots[FSIMG_LAYER_FILESYSTEM];
	const char *named;
	fsimg_status_t status;

	named = slot->state == FSIMG_SLOT_NAMED ? slot->name : NULL;
	status = identify_volume(ctx, input, length, named, filesystem, error);
	if (status != FSIMG_OK)
		return (status);
	if (named != NULL && strcmp(filesystem, named) != 0) {
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the volume does not hold the filesystem the stack names");
		fsimg_error_path(error, named);
		return (FSIMG_EINVAL);
	}
	return (FSIMG_OK);
}

/*
 * The input of a container that carries a filesystem, before any output
 * is produced (specs/design-layers.md#prefix-tables-opaque-sections-and-
 * carried-volumes): the stack holds nothing but the carried filesystem,
 * else FSIMG_EINVAL naming the container; the input draws that
 * filesystem's bounded-probe claim, else FSIMG_EFORMAT; it passes the
 * filesystem's structural verification, whose failure keeps its class; and
 * order receives the byte order it is stored in. Each refusal of the input
 * names it ("input").
 */
static fsimg_status_t
check_carried_input(fsimg_ctx_t *ctx, fsimg_io_t *input, uint64_t length,
    const fsimg_stack_t *stack, const fsimg_layer_t *container,
    char filesystem[static 16], fsimg_byte_order_t *order,
    fsimg_error_t *error)
{
	const char *carried = container->container->carries;
	fsimg_read_options_t read_options;
	fsimg_verify_report_t report;
	fsimg_reader_t *reader;
	fsimg_session_t session;
	fsimg_claim_t claim;
	fsimg_status_t status;

	status = fsimg_stack_carried(stack, container, error);
	if (status != FSIMG_OK)
		return (status);
	/* A corrupt or unsupported claim is a claim; the open reports it. */
	fsimg_session_init(&session, ctx, "volume identification", 0);
	status = fsimg_volume_claim(ctx, input, &session, 0, length, carried,
	    &claim, filesystem, error);
	if (status != FSIMG_OK)
		return (status);
	if (claim == FSIMG_CLAIM_NONE) {
		fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "the input does not hold the filesystem the container carries");
		fsimg_error_path(error, "input");
		return (FSIMG_EFORMAT);
	}
	snprintf(filesystem, 16, "%s", carried);

	fsimg_read_options_init(&read_options);
	read_options.format = carried;
	reader = NULL;
	status = fsimg_reader_open(ctx, input, &read_options, &reader, error);
	if (status == FSIMG_OK)
		status = fsimg_verify(reader, 0, &report, error);
	fsimg_reader_free(reader);
	if (status != FSIMG_OK) {
		fsimg_error_path(error, "input");
		return (status);
	}
	return (fsimg_volume_byte_order(ctx, input, &session, length, carried,
	    order, error));
}

/*
 * Wrap one raw volume. A filesystem the stack names must be the one the
 * volume holds, and a container that carries a filesystem requires its
 * input to hold that one and to verify. With a scheme the volume is the
 * one partition, typed by the layer settings or else by its filesystem;
 * without one it is the output.
 */
fsimg_status_t
fsimg_image_wrap(fsimg_ctx_t *ctx, fsimg_io_t *input, const char *text,
    const fsimg_write_options_t *options, const fsimg_sink_t *sink,
    fsimg_error_t *error)
{
	fsimg_write_options_t selected;
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_partition_request_t request;
	fsimg_disk_request_t disk_request;
	fsimg_disk_plan_t plan;
	fsimg_session_t session;
	fsimg_stack_t stack;
	const fsimg_layer_t *container;
	fsimg_byte_order_t order;
	char filesystem[16];
	uint64_t length;
	fsimg_error_t local;
	int raw_fd;
	fsimg_status_t status;

	if (error == NULL)
		error = &local;
	bzero(&plan, sizeof(plan));
	status = fsimg_sink_adopt(sink, FSIMG_RAW_NEVER, &raw_fd, error);
	if (status == FSIMG_OK)
		status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (input == NULL || text == NULL || fsimg_io_context(input) != ctx)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "wrap needs a stack and an input in its context"));
	status = fsimg_stack_parse(text, &stack, error);
	if (status == FSIMG_OK)
		status = check_mode(&stack, options, &selected, &layers, &inputs,
		    error);
	if (status == FSIMG_OK)
		status = fsimg_io_get_size(input, &length, error);
	if (status != FSIMG_OK)
		return (status);
	container = fsimg_stack_layer(&stack, FSIMG_LAYER_CONTAINER);
	order = FSIMG_BYTE_ORDER_LITTLE;
	if (container != NULL && container->container->carries != NULL)
		status = check_carried_input(ctx, input, length, &stack,
		    container, filesystem, &order, error);
	else
		status = check_named_input(ctx, input, length, &stack, filesystem,
		    error);
	if (status != FSIMG_OK)
		return (status);
	/* One session charges the container's plan and the output. */
	fsimg_session_init(&session, ctx, "image output", 0);
	disk_request = (fsimg_disk_request_t){ .session = &session,
	    .stack = &stack, .layers = &layers, .inputs = &inputs,
	    .compression = selected.compression, .epoch = selected.epoch,
	    .flags = selected.flags, .partitions = 1, .volume_order = order,
	    .verified_disk = container != NULL &&
	    container->container->carries != NULL };
	status = fsimg_disk_plan_init(ctx, &disk_request, &plan, error);
	if (status == FSIMG_OK && plan.scheme != NULL) {
		request = (fsimg_partition_request_t){
			.type = plan.table.kind == FSIMG_SCHEME_GPT ?
			    layers.gpt_type : layers.mbr_type,
			.name = layers.gpt_name,
			.guid = layers.gpt_guid,
			.filesystem = filesystem[0] != '\0' ? filesystem : NULL,
			.active = layers.mbr_active
		};
		status = fsimg_disk_plan_partition(&plan, 0, &request, error);
	}
	if (status == FSIMG_OK)
		status = write_plan(&session, &plan, &input, &length,
		    (selected.flags & FSIMG_WRITE_DURABLE) != 0, sink, error);
	fsimg_disk_plan_free(&plan);
	return (status);
}

/*
 * The bytes a selector names, raw: a partition, a range or the disk, or
 * under the section selector one of the container's sections, the byte
 * strings it stores beside the disk.
 */
fsimg_status_t
fsimg_image_unwrap(fsimg_image_t *image, const fsimg_selector_t *selector,
    const fsimg_write_options_t *options, const fsimg_sink_t *sink,
    fsimg_error_t *error)
{
	fsimg_write_options_t selected;
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_disk_request_t request;
	fsimg_disk_plan_t plan;
	fsimg_session_t session;
	fsimg_stack_t stack;
	fsimg_io_t *volume;
	fsimg_ctx_t *ctx;
	uint64_t length;
	fsimg_error_t local;
	int raw_fd;
	fsimg_status_t status;

	if (error == NULL)
		error = &local;
	volume = NULL;
	bzero(&stack, sizeof(stack));
	bzero(&plan, sizeof(plan));
	status = fsimg_sink_adopt(sink, FSIMG_RAW_NEVER, &raw_fd, error);
	if (status == FSIMG_OK)
		status = check_mode(&stack, options, &selected, &layers, &inputs,
		    error);
	if (status != FSIMG_OK)
		return (status);
	if (selector != NULL && selector->kind == FSIMG_SELECT_SECTION)
		status = fsimg_image_section(image, selector->text, &volume,
		    error);
	else
		status = fsimg_image_select(image, selector, &volume, error);
	if (status != FSIMG_OK)
		return (status);
	ctx = fsimg_io_context(volume);
	fsimg_session_init(&session, ctx, "image output", 0);
	status = fsimg_io_get_size(volume, &length, error);
	request = (fsimg_disk_request_t){ .session = &session, .stack = &stack,
	    .layers = &layers, .inputs = &inputs,
	    .compression = selected.compression,
	    .epoch = selected.epoch, .flags = selected.flags,
	    .partitions = 1 };
	if (status == FSIMG_OK)
		status = fsimg_disk_plan_init(ctx, &request, &plan, error);
	if (status == FSIMG_OK)
		status = write_plan(&session, &plan, &volume, &length,
		    (selected.flags & FSIMG_WRITE_DURABLE) != 0, sink, error);
	fsimg_disk_plan_free(&plan);
	fsimg_io_free(volume);
	return (status);
}

/*
 * A partitioned disk from volumes in order. The stack names the scheme
 * and nothing to write inside it; each part carries its own type, name
 * and GUID, so the layer options' partition settings must hold their
 * defaults. A part without a type takes its filesystem's.
 */
fsimg_status_t
fsimg_image_compose(fsimg_ctx_t *ctx, const char *text,
    const fsimg_part_t *parts, size_t count,
    const fsimg_write_options_t *options, const fsimg_sink_t *sink,
    fsimg_error_t *error)
{
	fsimg_write_options_t selected;
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_partition_request_t request;
	fsimg_disk_request_t disk_request;
	fsimg_disk_plan_t plan;
	fsimg_session_t session;
	fsimg_stack_t stack;
	fsimg_io_t **volumes;
	uint64_t *lengths;
	bool typed;
	char filesystem[16], key[32];
	fsimg_error_t local;
	int raw_fd;
	fsimg_status_t status;

	if (error == NULL)
		error = &local;
	bzero(&plan, sizeof(plan));
	status = fsimg_sink_adopt(sink, FSIMG_RAW_NEVER, &raw_fd, error);
	if (status == FSIMG_OK)
		status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (text == NULL || (parts == NULL && count != 0))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "compose needs a stack and its parts"));
	status = fsimg_stack_parse(text, &stack, error);
	if (status == FSIMG_OK)
		status = check_mode(&stack, options, &selected, &layers, &inputs,
		    error);
	if (status != FSIMG_OK)
		return (status);
	if (stack.slots[FSIMG_LAYER_FILESYSTEM].state == FSIMG_SLOT_NAMED ||
	    stack.slots[FSIMG_LAYER_SCHEME].state != FSIMG_SLOT_NAMED)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "compose needs a stack naming a scheme and no filesystem"));
	status = fsimg_layer_options_disk_only(&layers, error);
	if (status != FSIMG_OK)
		return (status);
	/* One session charges the container's plan and the output. */
	fsimg_session_init(&session, ctx, "image output", 0);
	disk_request = (fsimg_disk_request_t){ .session = &session,
	    .stack = &stack, .layers = &layers, .inputs = &inputs,
	    .compression = selected.compression, .epoch = selected.epoch,
	    .flags = selected.flags,
	    .partitions = count > UINT32_MAX ? UINT32_MAX : (uint32_t)count };
	status = fsimg_disk_plan_init(ctx, &disk_request, &plan, error);
	if (status != FSIMG_OK) {
		fsimg_disk_plan_free(&plan);
		return (status);
	}

	/* plan_init bounded count by the scheme's slots, so these are small. */
	lengths = NULL;
	volumes = fsimg_alloc(ctx, count * sizeof(*volumes), error);
	if (volumes != NULL)
		lengths = fsimg_alloc(ctx, count * sizeof(*lengths), error);
	if (lengths == NULL)
		status = error->status;
	for (size_t i = 0; status == FSIMG_OK && i < count; i++) {
		snprintf(key, sizeof(key), "partition %u", (unsigned)i + 1);
		volumes[i] = parts[i].volume;
		if (volumes[i] == NULL || fsimg_io_context(volumes[i]) != ctx) {
			status = fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "a part needs a volume in the context");
			fsimg_error_path(error, key);
			break;
		}
		status = fsimg_io_get_size(volumes[i], &lengths[i], error);
		/* An untyped part, NULL or empty, is typed by its filesystem. */
		typed = parts[i].type != NULL && parts[i].type[0] != '\0';
		filesystem[0] = '\0';
		if (status == FSIMG_OK && !typed)
			status = identify_volume(ctx, volumes[i], lengths[i], NULL,
			    filesystem, error);
		if (status != FSIMG_OK)
			break;
		request = (fsimg_partition_request_t){
			.type = typed ? parts[i].type : NULL,
			.name = parts[i].name,
			.guid = parts[i].guid,
			.filesystem = filesystem[0] != '\0' ? filesystem : NULL,
			.composed = true
		};
		status = fsimg_disk_plan_partition(&plan, (uint32_t)i, &request,
		    error);
	}
	if (status == FSIMG_OK)
		status = write_plan(&session, &plan, volumes, lengths,
		    (selected.flags & FSIMG_WRITE_DURABLE) != 0, sink, error);
	fsimg_dealloc(lengths);
	fsimg_dealloc(volumes);
	fsimg_disk_plan_free(&plan);
	return (status);
}
