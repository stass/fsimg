/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <limits.h>
#include <string.h>
#include <strings.h>

#include "emit.h"
#include "io.h"
#include "publish.h"
#include "source.h"
#include "format.h"
#include "writer.h"

static fsimg_status_t reject_aliases(fsimg_writer_t *, fsimg_io_t *,
    fsimg_error_t *);
static fsimg_status_t check_capabilities(fsimg_writer_t *, fsimg_error_t *);
static fsimg_status_t check_entry_capabilities(fsimg_writer_t *,
    const fsimg_entry_t *, bool, fsimg_error_t *);

void
fsimg_write_options_init(fsimg_write_options_t *options)
{
	bzero(options, sizeof(*options));
	options->size = sizeof(*options);
	options->version = FSIMG_OPTIONS_VERSION;
	options->flags = FSIMG_WRITE_ALLOW_LOSS;
	options->epoch = 315532800;
	options->sector_bytes = 512;
	options->reserve_percent = 3;
}

/* Whether every format-specific setting outside the format's mask holds its
 * default, so no setting is ever silently ignored.
 */
static bool
settings_supported(const fsimg_write_options_t *options, uint32_t settings)
{
	static const unsigned char zero[sizeof(options->volume_uuid)];

	return (((settings & FSIMG_SETTING_BYTE_ORDER) != 0 ||
	    options->byte_order == FSIMG_BYTE_ORDER_LITTLE) &&
	    ((settings & FSIMG_SETTING_VOLUME_UUID) != 0 ||
	    bcmp(options->volume_uuid, zero, sizeof(zero)) == 0) &&
	    ((settings & FSIMG_SETTING_INODE_CAPACITY) != 0 ||
	    options->inode_capacity == 0) &&
	    ((settings & FSIMG_SETTING_RESERVE) != 0 ||
	    options->reserve_percent == 3));
}

fsimg_status_t
fsimg_writer_new(fsimg_ctx_t *ctx, const char *format,
    const fsimg_write_options_t *options, fsimg_writer_t **out,
    fsimg_error_t *error)
{
	fsimg_write_options_t selected;
	fsimg_writer_t *writer;
	const fsimg_format_t *driver;
	const fsimg_format_variant_t *variant;
	size_t length;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL || format == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "missing writer format or output"));
	driver = fsimg_format_find(format, &variant);
	if (driver == NULL || driver->writer == NULL)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER, "unsupported writer format"));
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
	if (!settings_supported(&selected, variant->info.write_settings))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "write setting does not apply to the image format"));
	status = driver->writer->validate(&selected, variant->id, error);
	if (status != FSIMG_OK)
		return (status);
	length = 0;
	if (variant->info.max_label_bytes != 0) {
		if (selected.label == NULL)
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_WRITER, "format validation did not select a label"));
		length = strnlen(selected.label, sizeof(writer->label));
		if (length == sizeof(writer->label))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "volume label exceeds common backing capacity"));
	}
	status = fsimg_writer_collect_new(ctx, &writer, error);
	if (status != FSIMG_OK)
		return (status);
	writer->format = driver;
	writer->capabilities = &variant->info;
	writer->variant = variant->id;
	writer->options = selected;
	if (selected.label != NULL) {
		bcopy(selected.label, writer->label, length + 1);
		writer->options.label = writer->label;
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
	uint64_t capacity;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "missing writer output"));
	capacity = (uint64_t)ctx->options.max_path_bytes + FSIMG_CATALOG_HEADER;
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
	fsimg_session_init(&writer->session, ctx, "source collection", 0);
	writer->record = fsimg_alloc(ctx, (size_t)capacity, &detail);
	if (writer->record == NULL) {
		status = detail.status;
		if (error != NULL)
			*error = detail;
		goto fail;
	}
	writer->source_record = fsimg_alloc(ctx, (size_t)capacity, &detail);
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
	status = fsimg_stream_new(&writer->session, (uint32_t)capacity,
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
	fsimg_stream_free(writer->entries);
	fsimg_dealloc(writer->record);
	fsimg_dealloc(writer->source_record);
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
	status = fsimg_writer_collect(writer, entry, storage, id, error);
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
	fsimg_status_t status;

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
	status = check_capabilities(writer, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	status = writer->format->writer->plan(writer, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	fsimg_stat_add(&writer->plan->info.metadata_losses, writer->omitted_fields);
	*out = writer->plan;
	return (FSIMG_OK);
}

/* Planning's first step compares the catalog with the selected format's
 * capability record, so no driver ever receives an entry kind it lacks and
 * the policy for a field a format does not store is the same for every
 * format. A kind is never optional. An omitted field follows the optional
 * loss policy: a required one fails, an optional one is counted once and
 * reported under ALLOW_LOSS and fails without it.
 *
 * The catalog's unions of kinds, valid fields and required fields decide
 * the outcome before any entry is read again, because each failure
 * condition holds for the catalog exactly when it holds for some entry.
 * The pass over the catalog then either names the first failing entry or,
 * when nothing fails, reports each omission; a failed plan reports no loss.
 * When nothing is omitted the catalog is not read at all.
 */
static fsimg_status_t
check_capabilities(fsimg_writer_t *writer, fsimg_error_t *error)
{
	const fsimg_format_info_t *capabilities = writer->capabilities;
	const fsimg_catalog_entry_t *catalog;
	fsimg_status_t status;
	uint32_t omitted;
	bool fails;

	writer->omitted_fields = 0;
	omitted = writer->catalog_fields & ~capabilities->metadata_fields;
	fails = (writer->catalog_kinds & ~capabilities->entry_kinds) != 0 ||
	    (writer->catalog_required & omitted) != 0 ||
	    (omitted != 0 && (writer->options.flags & FSIMG_WRITE_ALLOW_LOSS) == 0);
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

/* Fail for an entry the format cannot store; otherwise, when report is set,
 * count and report the fields the format omits.
 */
static fsimg_status_t
check_entry_capabilities(fsimg_writer_t *writer, const fsimg_entry_t *entry,
    bool report, fsimg_error_t *error)
{
	const fsimg_format_info_t *capabilities = writer->capabilities;
	fsimg_status_t status;
	uint32_t omitted;

	status = FSIMG_OK;
	omitted = entry->metadata.valid & ~capabilities->metadata_fields;
	if ((entry->kind & ~capabilities->entry_kinds) != 0)
		status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER, "the image format cannot store this entry kind");
	else if ((entry->metadata.required & omitted) != 0 ||
	    (omitted != 0 && (writer->options.flags & FSIMG_WRITE_ALLOW_LOSS) == 0))
		status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER, "the image format cannot store requested metadata");
	if (status != FSIMG_OK) {
		fsimg_error_path(error, entry->path);
		return (status);
	}
	if (report)
		fsimg_stat_add(&writer->omitted_fields,
		    fsimg_report_loss(writer->ctx, entry->path, omitted));
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

static fsimg_status_t
reject_aliases(fsimg_writer_t *writer, fsimg_io_t *output, fsimg_error_t *error)
{
	const fsimg_catalog_entry_t *catalog;
	fsimg_host_file_t identity;
	fsimg_status_t status;
	bool known;

	status = fsimg_io_identity(output, &identity, &known, error);
	if (status != FSIMG_OK || !known)
		return (status);
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		if (catalog->storage != FSIMG_STORAGE_HOST)
			continue;
		status = fsimg_source_open(writer, id, error);
		if (status != FSIMG_OK)
			return (status);
		if (writer->source.identity.device == identity.device &&
		    writer->source.identity.inode == identity.inode) {
			fsimg_source_close(writer);
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "output aliases a collected host source"));
		}
		status = fsimg_source_finish(writer, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* Emission order (specs/design-emission.md): plan, reject source aliases,
 * check sink capabilities and the output limit, resize, then zero the sink and
 * run the driver (fsimg_emit_image), then flush. Nothing before the resize
 * mutates the sink, so a rejected alias or capability leaves it untouched;
 * every failure after that point marks the output incomplete.
 */
fsimg_status_t
fsimg_writer_emit(fsimg_writer_t *writer, fsimg_io_t *output,
    fsimg_error_t *error)
{
	const fsimg_plan_t *plan;
	fsimg_status_t status;
	bool resize, mutated;

	status = fsimg_ctx_ready(writer == NULL ? NULL : writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (writer->state != FSIMG_WRITER_COLLECTING &&
	    writer->state != FSIMG_WRITER_SEALED)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "writer emission is single-use"));
	mutated = false;
	if (fsimg_io_context(output) != writer->ctx) {
		status = fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "writer and output require the same context");
		goto fail;
	}
	status = fsimg_writer_plan(writer, &plan, error);
	if (status != FSIMG_OK)
		goto fail;
	writer->state = FSIMG_WRITER_EMITTING;
	writer->session.progress.phase = "image emission";
	status = reject_aliases(writer, output, error);
	if (status != FSIMG_OK)
		goto fail;
	status = fsimg_io_check_output(output, plan->info.volume_bytes,
	    (writer->options.flags & FSIMG_WRITE_DURABLE) != 0, &resize, error);
	if (status != FSIMG_OK)
		goto fail;
	if (writer->ctx->options.max_output_bytes != 0 &&
	    plan->info.volume_bytes > writer->ctx->options.max_output_bytes) {
		status = fsimg_limit_error(error, FSIMG_RESOURCE_OUTPUT,
		    writer->ctx->options.max_output_bytes, plan->info.volume_bytes);
		goto fail;
	}
	mutated = true;
	if (resize) {
		status = fsimg_io_resize(output, plan->info.volume_bytes, error);
		if (status != FSIMG_OK)
			goto fail;
	}
	status = fsimg_emit_image(writer, output, error);
	if (status != FSIMG_OK)
		goto fail;
	status = fsimg_io_flush(output,
	    (writer->options.flags & FSIMG_WRITE_DURABLE) != 0, error);
	if (status != FSIMG_OK)
		goto fail;
	fsimg_sources_release(writer);
	writer->state = FSIMG_WRITER_FINISHED;
	return (FSIMG_OK);
fail:
	if (mutated && error != NULL)
		error->output_state = FSIMG_OUTPUT_INCOMPLETE;
	return (fsimg_writer_fail(writer, status));
}

fsimg_status_t
fsimg_writer_write_at(fsimg_writer_t *writer, int parent, const char *name,
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
	status = fsimg_writer_plan(writer, &plan, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_output_write(writer, parent, name, plan->info.volume_bytes,
	    NULL, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	return (FSIMG_OK);
}
