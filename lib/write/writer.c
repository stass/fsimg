/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "emit.h"
#include "io.h"
#include "publish.h"
#include "settings.h"
#include "source.h"
#include "format.h"
#include "writer.h"

static fsimg_status_t reject_aliases(fsimg_writer_t *,
    const fsimg_host_file_t *, fsimg_error_t *);
static fsimg_status_t check_capabilities(fsimg_writer_t *, fsimg_error_t *);
static fsimg_status_t check_entry_capabilities(fsimg_writer_t *,
    const fsimg_entry_t *, bool, fsimg_error_t *);
static bool loss_refused(const fsimg_writer_t *, uint32_t, uint32_t);
static bool variant_of(const fsimg_format_t *, const fsimg_format_variant_t *);
static fsimg_status_t plan_layers(fsimg_ctx_t *, const fsimg_stack_t *,
    fsimg_write_options_t *, fsimg_layer_options_t *, fsimg_disk_plan_t *,
    fsimg_error_t *);
static fsimg_status_t fit_volume(const fsimg_format_t *,
    const fsimg_format_variant_t *, const fsimg_disk_plan_t *,
    fsimg_write_options_t *, fsimg_error_t *);
static fsimg_status_t writer_new_stack(fsimg_ctx_t *, const fsimg_stack_t *,
    const fsimg_format_t *, const fsimg_format_variant_t *,
    const fsimg_write_options_t *, fsimg_writer_t **, fsimg_error_t *);
static fsimg_status_t plan_disk(fsimg_writer_t *, fsimg_error_t *);
static fsimg_status_t emit_volume(void *, fsimg_io_t *, fsimg_error_t *);
static fsimg_status_t emit_disk(fsimg_writer_t *, fsimg_io_t *,
    fsimg_error_t *);
static fsimg_status_t emission_start(fsimg_writer_t *, bool, fsimg_error_t *);
static fsimg_status_t emit_raw_disk(fsimg_writer_t *, fsimg_io_t *, bool,
    bool *, fsimg_error_t *);
static fsimg_status_t emission_end(fsimg_writer_t *, fsimg_status_t, bool,
    fsimg_error_t *);
static fsimg_status_t output_through_raw(fsimg_writer_t *,
    const fsimg_sink_t *, fsimg_io_t *, fsimg_error_t *);

/* The filesystem settings' defaults come from their catalogue rows, the
 * same values the settings check compares with.
 */
void
fsimg_write_options_init(fsimg_write_options_t *options)
{
	const fsimg_setting_targets_t targets = { .write = options };

	bzero(options, sizeof(*options));
	options->size = sizeof(*options);
	options->version = FSIMG_OPTIONS_VERSION;
	options->flags = FSIMG_WRITE_ALLOW_LOSS;
	options->epoch = 315532800;
	fsimg_settings_init(&fsimg_filesystem_settings, &targets,
	    FSIMG_RECORD_WRITE);
}

/* Whether variant is one of the descriptor's own capability records. */
static bool
variant_of(const fsimg_format_t *driver, const fsimg_format_variant_t *variant)
{
	for (size_t i = 0; i < driver->variant_count; i++)
		if (&driver->variants[i] == variant)
			return (true);
	return (false);
}

/* Public construction parses the stack and selects the filesystem's writer
 * by name among the registered descriptors; a descriptor the registry does
 * not list cannot be reached through it.
 */
fsimg_status_t
fsimg_writer_new(fsimg_ctx_t *ctx, const char *format,
    const fsimg_write_options_t *options, fsimg_writer_t **out,
    fsimg_error_t *error)
{
	const fsimg_format_t *driver;
	const fsimg_format_variant_t *variant;
	fsimg_stack_t stack;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL || format == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "missing writer format or output"));
	status = fsimg_stack_parse(format, &stack, error);
	if (status != FSIMG_OK)
		return (status);
	if (stack.slots[FSIMG_LAYER_FILESYSTEM].state != FSIMG_SLOT_NAMED)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "the stack names no filesystem to write"));
	driver = fsimg_format_find(stack.slots[FSIMG_LAYER_FILESYSTEM].name,
	    &variant);
	return (writer_new_stack(ctx, &stack, driver, variant, options, out,
	    error));
}

/* A descriptor's variant alone: a bare volume. */
fsimg_status_t
fsimg_writer_new_variant(fsimg_ctx_t *ctx, const fsimg_format_t *driver,
    const fsimg_format_variant_t *variant,
    const fsimg_write_options_t *options, fsimg_writer_t **out,
    fsimg_error_t *error)
{
	fsimg_stack_t stack;

	bzero(&stack, sizeof(stack));
	if (variant != NULL) {
		stack.slots[FSIMG_LAYER_FILESYSTEM].state = FSIMG_SLOT_NAMED;
		snprintf(stack.slots[FSIMG_LAYER_FILESYSTEM].name,
		    sizeof(stack.slots[FSIMG_LAYER_FILESYSTEM].name), "%s",
		    variant->info.name);
	}
	return (writer_new_stack(ctx, &stack, driver, variant, options, out,
	    error));
}

/*
 * Resolve the disk around the volume before the format validates its
 * options: the compression, the scheme's settings and the partition's
 * type, name and identity are all checked here, before any source is
 * read. A derived disk is sized from the planned volume, so its MBR limit
 * is checked when the plan is placed; a fixed disk's room for the volume
 * is checked by fit_volume once the format has resolved its sector.
 */
static fsimg_status_t
plan_layers(fsimg_ctx_t *ctx, const fsimg_stack_t *stack,
    fsimg_write_options_t *selected, fsimg_layer_options_t *layers,
    fsimg_disk_plan_t *disk, fsimg_error_t *error)
{
	fsimg_partition_request_t request;
	fsimg_disk_request_t disk_request;
	fsimg_layer_inputs_t inputs;
	bool gpt;
	fsimg_status_t status;

	/* The disk plan keeps the file settings' objects, which the caller
	 * lends until the output is written, and no pointer to either record.
	 */
	fsimg_layer_options_init(layers);
	if (selected->layers != NULL)
		*layers = *selected->layers;
	selected->layers = NULL;
	bzero(&inputs, sizeof(inputs));
	if (selected->inputs != NULL)
		inputs = *selected->inputs;
	selected->inputs = NULL;
	disk_request = (fsimg_disk_request_t){ .stack = stack, .layers = layers,
	    .inputs = &inputs, .compression = selected->compression,
	    .epoch = selected->epoch, .flags = selected->flags, .partitions = 1,
	    .volume_order = selected->byte_order };
	status = fsimg_disk_plan_init(ctx, &disk_request, disk, error);
	if (status != FSIMG_OK || disk->scheme == NULL)
		return (status);
	gpt = disk->table.kind == FSIMG_SCHEME_GPT;
	request = (fsimg_partition_request_t){
		.type = gpt ? layers->gpt_type : layers->mbr_type,
		.name = layers->gpt_name,
		.guid = layers->gpt_guid,
		.filesystem = stack->slots[FSIMG_LAYER_FILESYSTEM].name,
		.active = layers->mbr_active
	};
	return (fsimg_disk_plan_partition(disk, 0, &request, error));
}

/*
 * The volume of a fixed disk, once the format's validation has resolved
 * its sector into selected: without a volume size the volume takes the
 * partition's whole usable length, rounded down to that sector, as an
 * exact size, which the format then validates in turn; a given size must
 * fit. The fit therefore rounds to the sector the format writes, whatever
 * sector the format chose.
 */
static fsimg_status_t
fit_volume(const fsimg_format_t *driver, const fsimg_format_variant_t *variant,
    const fsimg_disk_plan_t *disk, fsimg_write_options_t *selected,
    fsimg_error_t *error)
{
	bool sized;
	fsimg_status_t status;

	sized = selected->volume_bytes != 0;
	if (selected->sector_bytes == 0)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "format validation did not resolve a sector size"));
	status = fsimg_disk_plan_fit(disk, selected->sector_bytes,
	    &selected->volume_bytes, error);
	if (status == FSIMG_OK && !sized)
		status = driver->writer->validate(selected, variant->id, error);
	return (status);
}

static fsimg_status_t
writer_new_stack(fsimg_ctx_t *ctx, const fsimg_stack_t *stack,
    const fsimg_format_t *driver, const fsimg_format_variant_t *variant,
    const fsimg_write_options_t *options, fsimg_writer_t **out,
    fsimg_error_t *error)
{
	fsimg_write_options_t selected;
	fsimg_layer_options_t layers;
	fsimg_disk_plan_t disk;
	fsimg_writer_t *writer;
	fsimg_error_t local;
	size_t length;
	fsimg_status_t status;

	/* The layer functions record every failure in an error record. */
	if (error == NULL)
		error = &local;
	if (out != NULL)
		*out = NULL;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "missing writer output"));
	if (driver == NULL || variant == NULL || driver->writer == NULL)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER, "unsupported writer format"));
	if (!variant_of(driver, variant))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "variant does not belong to the format"));
	fsimg_write_options_init(&selected);
	if (options != NULL) {
		if (options->size != sizeof(*options))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "unsupported write options size"));
		selected = *options;
	}
	if (selected.version != FSIMG_OPTIONS_VERSION ||
	    (selected.flags & ~(FSIMG_WRITE_ALLOW_LOSS | FSIMG_WRITE_REPRODUCIBLE |
	    FSIMG_WRITE_DURABLE | FSIMG_WRITE_IMPORT_POSIX)) != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "unsupported write options version or flags"));
	for (size_t i = 0; i < 4; i++) {
		if (selected.reserved[i] != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "nonzero reserved write option"));
	}
	/* A format without a volume label accepts none; a format with one
	 * selects its default when the caller passes NULL.
	 */
	if (variant->info.max_label_bytes == 0 && selected.label != NULL)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER, "the image format has no volume label"));
	/* A setting the format does not store must hold its default, so none
	 * is silently ignored.
	 */
	status = fsimg_write_settings_check(&selected, &variant->info, error);
	if (status != FSIMG_OK)
		return (status);
	status = plan_layers(ctx, stack, &selected, &layers, &disk, error);
	if (status == FSIMG_OK)
		status = driver->writer->validate(&selected, variant->id, error);
	if (status == FSIMG_OK && disk.scheme != NULL && disk.fixed)
		status = fit_volume(driver, variant, &disk, &selected, error);
	if (status == FSIMG_OK && selected.mountpoint != NULL &&
	    strnlen(selected.mountpoint, sizeof(writer->mountpoint)) ==
	    sizeof(writer->mountpoint))
		status = fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "mountpoint exceeds common backing capacity");
	length = 0;
	if (status == FSIMG_OK && variant->info.max_label_bytes != 0) {
		if (selected.label == NULL)
			status = fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_WRITER, "format validation did not select a label");
		else if ((length = strnlen(selected.label,
		    sizeof(writer->label))) == sizeof(writer->label))
			status = fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "volume label exceeds common backing capacity");
	}
	if (status == FSIMG_OK)
		status = fsimg_writer_collect_new(ctx, &writer, error);
	if (status != FSIMG_OK) {
		fsimg_disk_plan_free(&disk);
		return (status);
	}
	writer->format = driver;
	writer->capabilities = &variant->info;
	writer->variant = variant->id;
	writer->options = selected;
	writer->stack = *stack;
	writer->layers = layers;
	writer->options.layers = &writer->layers;
	writer->disk = disk;
	if (selected.label != NULL) {
		bcopy(selected.label, writer->label, length + 1);
		writer->options.label = writer->label;
	}
	if (selected.mountpoint != NULL) {
		bcopy(selected.mountpoint, writer->mountpoint,
		    strlen(selected.mountpoint) + 1);
		writer->options.mountpoint = writer->mountpoint;
	}
	*out = writer;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_writer_collect_new(fsimg_ctx_t *ctx, fsimg_writer_t **out,
    fsimg_error_t *error)
{
	fsimg_writer_t *writer;
	fsimg_error_t detail;
	uint64_t capacity, source_capacity;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "missing writer output"));
	/* Catalog records hold a path; source records hold a relative path or
	 * a generated description, whose size does not depend on the path
	 * limit, so the source-record buffer and stream may be the larger.
	 */
	capacity = (uint64_t)ctx->options.max_path_bytes + FSIMG_CATALOG_HEADER;
	source_capacity = capacity < FSIMG_GENERATED_RECORD_MAX ?
	    FSIMG_GENERATED_RECORD_MAX : capacity;
	if (capacity > UINT32_MAX)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_WRITER, "catalog record limit exceeds internal range"));
	writer = fsimg_alloc(ctx, sizeof(*writer), &detail);
	if (writer == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	writer->ctx = ctx;
	writer->record_capacity = (uint32_t)capacity;
	writer->source_record_capacity = (uint32_t)source_capacity;
	fsimg_session_init(&writer->session, ctx, "source collection", 0);
	writer->record = fsimg_alloc(ctx, (size_t)capacity, &detail);
	if (writer->record == NULL) {
		status = detail.status;
		if (error != NULL)
			*error = detail;
		goto fail;
	}
	writer->source_record = fsimg_alloc(ctx, (size_t)source_capacity,
	    &detail);
	if (writer->source_record == NULL) {
		status = detail.status;
		if (error != NULL)
			*error = detail;
		goto fail;
	}
	status = fsimg_stream_new(&writer->session, (uint32_t)capacity,
	    &writer->entries, error);
	if (status != FSIMG_OK)
		goto fail;
	status = fsimg_stream_new(&writer->session, (uint32_t)source_capacity,
	    &writer->host_sources, error);
	if (status != FSIMG_OK)
		goto fail;
	*out = writer;
	return (FSIMG_OK);
fail:
	fsimg_writer_free(writer);
	return (status);
}

fsimg_status_t
fsimg_writer_collect_ready(fsimg_writer_t *writer, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_ctx_ready(writer == NULL ? NULL : writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (writer->state != FSIMG_WRITER_COLLECTING)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "writer collection is sealed or failed"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_writer_fail(fsimg_writer_t *writer, fsimg_status_t status)
{
	writer->state = FSIMG_WRITER_FAILED;
	fsimg_sources_release(writer);
	fsimg_stream_free(writer->entries);
	writer->entries = NULL;
	fsimg_dealloc(writer->source_record);
	writer->source_record = NULL;
	fsimg_stream_free(writer->host_sources);
	writer->host_sources = NULL;
	fsimg_stream_free(writer->generated_digests);
	writer->generated_digests = NULL;
	/* A successfully published catalog remains inspectable after failure. */
	if (!writer->catalog_ready)
		fsimg_catalog_discard(writer);
	return (status);
}

void
fsimg_writer_free(fsimg_writer_t *writer)
{
	if (writer == NULL || writer->ctx->in_callback)
		return;
	fsimg_sources_release(writer);
	if (writer->format != NULL)
		writer->format->writer->free(writer->plan);
	fsimg_catalog_discard(writer);
	fsimg_stream_free(writer->host_sources);
	fsimg_stream_free(writer->generated_digests);
	fsimg_stream_free(writer->entries);
	fsimg_dealloc(writer->record);
	fsimg_dealloc(writer->source_record);
	fsimg_disk_plan_free(&writer->disk);
	fsimg_dealloc(writer);
}

void
fsimg_source_ops_init(fsimg_source_ops_t *ops)
{
	bzero(ops, sizeof(*ops));
	ops->size = sizeof(*ops);
	ops->version = FSIMG_OPTIONS_VERSION;
	ops->max_transfer = 64 * 1024;
}

fsimg_status_t
fsimg_writer_set_sources(fsimg_writer_t *writer, const fsimg_source_ops_t *ops,
    void *argument, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_writer_collect_ready(writer, error);
	if (status != FSIMG_OK)
		return (status);
	if (writer->provider_set)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "source provider is already registered"));
	if (ops == NULL || ops->size != sizeof(*ops)) {
		status = fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_SOURCE, "unsupported source table size");
		return (fsimg_writer_fail(writer, status));
	}
	if (ops->version != FSIMG_OPTIONS_VERSION || ops->flags != 0 ||
	    ops->validate == NULL || ops->read_at == NULL) {
		status = fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_SOURCE, "invalid source provider capabilities");
		return (fsimg_writer_fail(writer, status));
	}
	for (size_t i = 0; i < 4; i++) {
		if (ops->reserved[i] != 0) {
			status = fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_SOURCE, "nonzero reserved source field");
			return (fsimg_writer_fail(writer, status));
		}
	}
	writer->provider = *ops;
	if (writer->provider.max_transfer == 0 ||
	    writer->provider.max_transfer > SSIZE_MAX)
		writer->provider.max_transfer = SSIZE_MAX;
	writer->provider_argument = argument;
	writer->provider_set = true;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_writer_add(fsimg_writer_t *writer, const fsimg_entry_t *entry,
    const fsimg_source_t *source, fsimg_error_t *error)
{
	fsimg_status_t status;
	fsimg_source_storage_t storage;
	uint64_t id;

	status = fsimg_writer_collect_ready(writer, error);
	if (status != FSIMG_OK)
		return (status);
	storage = FSIMG_STORAGE_NONE;
	id = 0;
	if (entry == NULL) {
		status = fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "missing writer entry");
		return (fsimg_writer_fail(writer, status));
	}
	if (source != NULL) {
		if (source->kind == FSIMG_SOURCE_ZERO && source->id == 0)
			storage = FSIMG_STORAGE_ZERO;
		else if (source->kind == FSIMG_SOURCE_PROVIDER && writer->provider_set)
			storage = FSIMG_STORAGE_PROVIDER;
		else {
			status = fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_SOURCE, "invalid or unregistered source reference");
			return (fsimg_writer_fail(writer, status));
		}
		id = source->id;
	}
	status = fsimg_writer_collect(writer, entry, storage, id, NULL, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_writer_prepare(fsimg_writer_t *writer, const fsimg_sort_options_t *options,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_writer_collect_ready(writer, error);
	if (status != FSIMG_OK)
		return (status);
	writer->state = FSIMG_WRITER_PREPARING;
	status = fsimg_stream_seal(writer->entries, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(writer->host_sources, error);
	if (status == FSIMG_OK)
		status = fsimg_catalog_prepare(writer, options, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	fsimg_stream_free(writer->entries);
	writer->entries = NULL;
	writer->state = FSIMG_WRITER_SEALED;
	writer->catalog_ready = true;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_writer_plan(fsimg_writer_t *writer, const fsimg_plan_t **out,
    fsimg_error_t *error)
{
	fsimg_error_t local;
	fsimg_status_t status;

	if (error == NULL)
		error = &local;
	if (out != NULL)
		*out = NULL;
	status = fsimg_ctx_ready(writer == NULL ? NULL : writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "missing plan output"));
	if (writer->plan != NULL) {
		*out = writer->plan;
		return (FSIMG_OK);
	}
	if (writer->state != FSIMG_WRITER_COLLECTING)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "writer cannot plan in this state"));
	if (writer->format == NULL)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "catalog-only writer has no image format"));
	status = fsimg_writer_prepare(writer, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	writer->session.progress.phase = "image planning";
	writer->metadata_losses = 0;
	status = check_capabilities(writer, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	status = writer->format->writer->plan(writer, error);
	if (status == FSIMG_OK)
		status = plan_disk(writer, error);
	/* The digests a manifest claims for generated content are checked
	 * last, once the image, and plan_disk's output limit, have accepted
	 * every file's size: the bytes hashed never exceed the planned volume.
	 */
	if (status == FSIMG_OK)
		status = fsimg_manifest_check_digests(writer, error);
	if (status != FSIMG_OK) {
		/* A plan is complete only once every step accepted it; one a
		 * later step refused is not kept for inspection.
		 */
		writer->format->writer->free(writer->plan);
		writer->plan = NULL;
		return (fsimg_writer_fail(writer, status));
	}
	writer->plan->info.metadata_losses = writer->metadata_losses;
	*out = writer->plan;
	return (FSIMG_OK);
}

/* Place the planned volume in its disk, check the output limit, and
 * report the disk as an opened image would: the stack, the scheme and
 * container and their identifiers, and the volume's partition holding the
 * planned filesystem; then the output's bound, the container's padding and
 * the codec the output depends on.
 */
static fsimg_status_t
plan_disk(fsimg_writer_t *writer, fsimg_error_t *error)
{
	fsimg_plan_info_t *info = &writer->plan->info;
	fsimg_status_t status;

	status = fsimg_disk_plan_place(&writer->disk, &info->volume_bytes, error);
	if (status == FSIMG_OK)
		status = fsimg_disk_plan_budget(writer->ctx, &writer->disk,
		    fsimg_disk_plan_encoded(&writer->disk), error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_disk_plan_report(&writer->disk, 1, &info->image, &info->partition);
	info->output_bytes = writer->disk.output_bytes;
	info->padding_bytes = writer->disk.padding_bytes;
	fsimg_disk_plan_codec(&writer->disk, info->codec_version,
	    sizeof(info->codec_version));
	fsimg_stack_format(&writer->stack, info->image.stack,
	    sizeof(info->image.stack));
	if (writer->disk.scheme != NULL) {
		info->partition.claim = FSIMG_CLAIM_FORMAT;
		snprintf(info->partition.format, sizeof(info->partition.format),
		    "%s", info->format);
	}
	return (FSIMG_OK);
}

/* Planning's first step compares the catalog with the selected format's
 * capability record, so no driver ever receives an entry kind it lacks and
 * the policy for a field a format does not store is the same for every
 * format. An entry kind the format lacks or a symbolic-link target longer
 * than its limit always fails; neither is a metadata loss. A field the
 * format omits goes to fsimg_writer_loss(), one mask per entry; drivers
 * encode and report losses only for fields the format stores, so no field
 * is submitted twice. The catalog retains the original metadata.
 *
 * The catalog's unions of kinds, valid fields and required fields, and its
 * longest link target, decide the outcome before any entry is read again,
 * because each failure condition holds for the catalog exactly when it
 * holds for some entry.
 * The pass over the catalog then either names the first failing entry,
 * reporting no omission on the way, or, when nothing fails, reports each
 * omission. When nothing is omitted the catalog is not read at all.
 */
static fsimg_status_t
check_capabilities(fsimg_writer_t *writer, fsimg_error_t *error)
{
	const fsimg_format_info_t *capabilities = writer->capabilities;
	const fsimg_catalog_entry_t *catalog;
	fsimg_status_t status;
	uint32_t omitted;
	bool fails;

	omitted = writer->catalog_fields & ~capabilities->metadata_fields;
	fails = (writer->catalog_kinds & ~capabilities->entry_kinds) != 0 ||
	    writer->catalog_link_target_max > capabilities->max_link_target_bytes ||
	    loss_refused(writer, writer->catalog_required, omitted);
	if (!fails && omitted == 0)
		return (FSIMG_OK);
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		status = check_entry_capabilities(writer, &catalog->entry, !fails,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (fails)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "catalog capability summary disagrees with its entries"));
	return (FSIMG_OK);
}

/* Fail for an entry the format cannot store. Then submit the fields the
 * format omits to the loss policy: always when report is set, and
 * otherwise only when the policy refuses them, so a failing pass reports
 * no loss.
 */
static fsimg_status_t
check_entry_capabilities(fsimg_writer_t *writer, const fsimg_entry_t *entry,
    bool report, fsimg_error_t *error)
{
	const fsimg_format_info_t *capabilities = writer->capabilities;
	fsimg_status_t status;
	uint32_t omitted;

	if ((entry->kind & ~capabilities->entry_kinds) != 0) {
		status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER, "the image format cannot store this entry kind");
		fsimg_error_path(error, entry->path);
		return (status);
	}
	if (entry->kind == FSIMG_ENTRY_SYMLINK &&
	    entry->size > capabilities->max_link_target_bytes) {
		status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER,
		    "the image format cannot store a symbolic-link target this long");
		fsimg_error_path(error, entry->path);
		return (status);
	}
	omitted = entry->metadata.valid & ~capabilities->metadata_fields;
	if (!report && !loss_refused(writer, entry->metadata.required, omitted))
		return (FSIMG_OK);
	return (fsimg_writer_loss(writer, entry, omitted,
	    "the image format cannot store requested metadata", error));
}

/* Whether the loss policy refuses losing fields, given the entry's
 * required mask: when any of them is required, or without ALLOW_LOSS.
 */
static bool
loss_refused(const fsimg_writer_t *writer, uint32_t required, uint32_t fields)
{
	return (fields != 0 && ((required & fields) != 0 ||
	    (writer->options.flags & FSIMG_WRITE_ALLOW_LOSS) == 0));
}

fsimg_status_t
fsimg_writer_loss(fsimg_writer_t *writer, const fsimg_entry_t *entry,
    uint32_t fields, const char *message, fsimg_error_t *error)
{
	uint64_t count;

	if (fields == 0)
		return (FSIMG_OK);
	if ((fields & ~(entry->metadata.valid & FSIMG_META_KNOWN)) != 0)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "a metadata loss names a field the entry lacks"));
	if (loss_refused(writer, entry->metadata.required, fields)) {
		fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_WRITER,
		    message);
		fsimg_error_path(error, entry->path);
		return (FSIMG_EUNSUPPORTED);
	}
	count = fsimg_report_loss(writer->ctx, entry->path, fields);
	fsimg_stat_add(&writer->metadata_losses, count);
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_plan_get_info(const fsimg_plan_t *plan, fsimg_plan_info_t *out,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	if (out != NULL)
		bzero(out, sizeof(*out));
	status = fsimg_ctx_ready(plan == NULL ? NULL : plan->writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "missing plan information output"));
	*out = plan->info;
	return (FSIMG_OK);
}

/* No collected host source and no file setting may be the file the output
 * writes.
 */
static fsimg_status_t
reject_aliases(fsimg_writer_t *writer, const fsimg_host_file_t *output,
    fsimg_error_t *error)
{
	const fsimg_catalog_entry_t *catalog;
	fsimg_status_t status;
	bool aliased;

	status = fsimg_disk_plan_input_aliases(&writer->disk, output, error);
	if (status != FSIMG_OK)
		return (status);
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		if (catalog->storage != FSIMG_STORAGE_HOST)
			continue;
		/* Opening validates the source's identity, which is all this
		 * probe needs, so it closes rather than finishes: finishing is
		 * for a cursor that served the bytes, whose digest it checks. A
		 * source that changed since collection is named by its image
		 * path; source access leaves the catalog snapshot intact.
		 */
		status = fsimg_source_open(writer, id, error);
		if (status != FSIMG_OK) {
			fsimg_error_path(error, catalog->entry.path);
			return (status);
		}
		aliased = writer->source.identity.device == output->device &&
		    writer->source.identity.inode == output->inode;
		fsimg_source_close(writer);
		if (aliased)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "output aliases a collected host source"));
	}
	return (FSIMG_OK);
}

static fsimg_status_t
emit_volume(void *writer, fsimg_io_t *volume, fsimg_error_t *error)
{
	return (fsimg_emit_image(writer, volume, error));
}

/* A bare volume that fills the disk is the disk; otherwise the volume is
 * emitted into its slice of the disk the layered output lays out, after a
 * scheme's leading tables or before a container's padding.
 */
static fsimg_status_t
emit_disk(fsimg_writer_t *writer, fsimg_io_t *output, fsimg_error_t *error)
{
	uint64_t volume_bytes = writer->plan->info.volume_bytes;

	if (writer->disk.scheme == NULL &&
	    writer->disk.disk_bytes == volume_bytes)
		return (fsimg_emit_image(writer, output, error));
	return (fsimg_disk_plan_emit(&writer->disk, volume_bytes, output,
	    &writer->session, emit_volume, writer, error));
}

/*
 * Start a writer's one emission: the writer must not have emitted, and
 * plans now if it has not. through_intermediate says the caller emits the
 * raw disk into a raw intermediate and encodes the output from it, the
 * only way a container or compressed output is written; refusing one here
 * leaves the writer usable.
 */
static fsimg_status_t
emission_start(fsimg_writer_t *writer, bool through_intermediate,
    fsimg_error_t *error)
{
	const fsimg_plan_t *plan;
	fsimg_status_t status;

	status = fsimg_ctx_ready(writer == NULL ? NULL : writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (writer->state != FSIMG_WRITER_COLLECTING &&
	    writer->state != FSIMG_WRITER_SEALED)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "writer emission is single-use"));
	if (!through_intermediate && fsimg_disk_plan_encoded(&writer->disk))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "a container or compressed output is written only through an "
		    "output sink with a raw intermediate"));
	return (fsimg_writer_plan(writer, &plan, error));
}

/*
 * Emission order (specs/design-emission.md): reject source aliases, check
 * the output's capabilities, resize it, then zero it and run the driver
 * (fsimg_emit_image) inside its disk, then flush. Nothing before the
 * resize mutates the output, so a rejected alias or capability leaves it
 * untouched and *mutated false. The output holds the whole raw disk, which
 * is the volume when the stack has neither a scheme nor padding.
 */
static fsimg_status_t
emit_raw_disk(fsimg_writer_t *writer, fsimg_io_t *output, bool durable,
    bool *mutated, fsimg_error_t *error)
{
	fsimg_host_file_t identity;
	uint64_t disk_bytes;
	fsimg_status_t status;
	bool known, resize;

	*mutated = false;
	if (fsimg_io_context(output) != writer->ctx)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "writer and output require the same context"));
	writer->state = FSIMG_WRITER_EMITTING;
	writer->session.progress.phase = "image emission";
	status = fsimg_io_identity(output, &identity, &known, error);
	if (status == FSIMG_OK && known)
		status = reject_aliases(writer, &identity, error);
	if (status != FSIMG_OK)
		return (status);
	disk_bytes = writer->plan->info.image.disk_bytes;
	status = fsimg_io_check_output(output, disk_bytes, durable, &resize,
	    error);
	if (status != FSIMG_OK)
		return (status);
	*mutated = true;
	if (resize) {
		status = fsimg_io_resize(output, disk_bytes, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = emit_disk(writer, output, error);
	if (status == FSIMG_OK)
		status = fsimg_io_flush(output, durable, error);
	return (status);
}

/* A finished emission releases its sources; a failed one fails the
 * writer, leaving an output it may have changed incomplete.
 */
static fsimg_status_t
emission_end(fsimg_writer_t *writer, fsimg_status_t status, bool mutated,
    fsimg_error_t *error)
{
	if (status == FSIMG_OK) {
		fsimg_sources_release(writer);
		writer->state = FSIMG_WRITER_FINISHED;
		return (FSIMG_OK);
	}
	if (mutated && error != NULL)
		error->output_state = FSIMG_OUTPUT_INCOMPLETE;
	return (fsimg_writer_fail(writer, status));
}

fsimg_status_t
fsimg_writer_emit(fsimg_writer_t *writer, fsimg_io_t *output,
    fsimg_error_t *error)
{
	fsimg_error_t local;
	fsimg_status_t status;
	bool mutated;

	if (error == NULL)
		error = &local;
	status = emission_start(writer, false, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_raw_disk(writer, output,
	    (writer->options.flags & FSIMG_WRITE_DURABLE) != 0, &mutated, error);
	return (emission_end(writer, status, mutated, error));
}

fsimg_status_t
fsimg_writer_write_at(fsimg_writer_t *writer, int parent, const char *name,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = emission_start(writer, false, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_output_write(writer, parent, name, NULL, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	return (FSIMG_OK);
}

/*
 * The output passes through the raw intermediate raw: the writer emits
 * the raw disk into it, never durably since it has no name, then one
 * transcode encodes it through the container and compression, or copies
 * it, to the sink.
 * The sink may be neither a collected source nor the raw intermediate
 * itself. A failure while the raw disk is emitted leaves the sink
 * untouched; the transcode reports its own output state.
 */
static fsimg_status_t
output_through_raw(fsimg_writer_t *writer, const fsimg_sink_t *sink,
    fsimg_io_t *raw, fsimg_error_t *error)
{
	fsimg_host_file_t output, intermediate;
	uint64_t written;
	fsimg_status_t status;
	bool known, raw_known, mutated;

	status = emission_start(writer, true, error);
	if (status != FSIMG_OK)
		return (status);
	/* Planning charged the raw disk only for an encoded output; a
	 * stream of the bare disk holds it too. Nothing is written yet, so a refusal
	 * leaves the writer usable for a smaller output.
	 */
	status = fsimg_disk_plan_budget(writer->ctx, &writer->disk, true,
	    error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_sink_identity(sink, &output, &known, error);
	if (status == FSIMG_OK)
		status = fsimg_io_identity(raw, &intermediate, &raw_known, error);
	if (status == FSIMG_OK && known && raw_known &&
	    output.device == intermediate.device &&
	    output.inode == intermediate.inode)
		status = fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "output aliases the raw intermediate");
	if (status == FSIMG_OK && known)
		status = reject_aliases(writer, &output, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	status = emit_raw_disk(writer, raw, false, &mutated, error);
	if (status != FSIMG_OK) {
		error->output_state = FSIMG_OUTPUT_ABSENT;
		return (fsimg_writer_fail(writer, status));
	}
	writer->session.progress.phase = "image transcode";
	status = fsimg_disk_output(&writer->session, raw, &writer->disk, sink,
	    (writer->options.flags & FSIMG_WRITE_DURABLE) != 0, &written, error);
	return (emission_end(writer, status, false, error));
}

/*
 * The sink cases. Without a container or compression, a FILE sink stages
 * the raw disk as write_at does and an IO sink receives it as emit does;
 * an encoded output or a STREAM sink goes through the raw intermediate,
 * which the sink hands over and this call adopts on every outcome.
 */
fsimg_status_t
fsimg_writer_output(fsimg_writer_t *writer, const fsimg_sink_t *sink,
    fsimg_error_t *error)
{
	fsimg_error_t local;
	fsimg_io_t *raw;
	fsimg_raw_need_t need;
	fsimg_status_t status;
	int raw_fd;

	if (error == NULL)
		error = &local;
	need = writer != NULL && fsimg_disk_plan_encoded(&writer->disk) ?
	    FSIMG_RAW_ALWAYS : FSIMG_RAW_FOR_STREAM;
	status = fsimg_sink_adopt(sink, need, &raw_fd, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_ctx_ready(writer == NULL ? NULL : writer->ctx, error);
	if (status != FSIMG_OK) {
		if (raw_fd != -1)
			close(raw_fd);
		return (status);
	}
	if (raw_fd == -1 && sink->kind == FSIMG_SINK_FILE)
		return (fsimg_writer_write_at(writer, sink->parent_fd, sink->name,
		    error));
	if (raw_fd == -1)
		return (fsimg_writer_emit(writer, sink->io, error));
	status = fsimg_io_adopt_handed_over(writer->ctx, raw_fd,
	    "raw intermediate", &raw, error);
	if (status != FSIMG_OK)
		return (status);
	status = output_through_raw(writer, sink, raw, error);
	fsimg_io_free(raw);
	return (status);
}
