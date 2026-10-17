/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <limits.h>
#include <string.h>
#include <strings.h>

#include "io.h"
#include "publish.h"
#include "source.h"
#include "write.h"
#include "writer.h"

static fsimg_status_t reject_aliases(fsimg_writer_t *, fsimg_io_t *,
    fsimg_error_t *);

void
fsimg_write_options_init(fsimg_write_options_t *options)
{
	bzero(options, sizeof(*options));
	options->size = sizeof(*options);
	options->version = FSIMG_OPTIONS_VERSION;
	options->flags = FSIMG_WRITE_ALLOW_LOSS;
	options->epoch = 315532800;
	options->sector_bytes = 512;
}

fsimg_status_t
fsimg_writer_new(fsimg_ctx_t *ctx, const char *format,
    const fsimg_write_options_t *options, fsimg_writer_t **out,
    fsimg_error_t *error)
{
	fsimg_write_options_t selected;
	fsimg_writer_t *writer;
	uint32_t sector, cluster;
	unsigned variant;
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
	variant = strcmp(format, "fat12") == 0 ? 12 :
	    strcmp(format, "fat16") == 0 ? 16 : strcmp(format, "fat32") == 0 ? 32 : 0;
	if (variant == 0)
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
	    FSIMG_WRITE_DURABLE)) != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "unsupported write options version or flags"));
	for (size_t i = 0; i < 4; i++) {
		if (selected.reserved[i] != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "nonzero reserved write option"));
	}
	sector = selected.sector_bytes == 0 ? 512 : selected.sector_bytes;
	cluster = selected.allocation_unit_bytes;
	if ((sector != 512 && sector != 1024 && sector != 2048 && sector != 4096) ||
	    (cluster != 0 && (cluster < sector || cluster > 32768 ||
	    (cluster & (cluster - 1)) != 0)) ||
	    selected.volume_bytes % sector != 0 ||
	    selected.volume_bytes / sector > UINT32_MAX ||
	    selected.fat_root_entries > UINT16_MAX ||
	    (variant == 32 && selected.fat_root_entries != 0) ||
	    ((selected.flags & FSIMG_WRITE_REPRODUCIBLE) != 0 &&
	    (selected.epoch < 315532800 || selected.epoch > 4354819199LL)))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "invalid FAT geometry or reproducible epoch"));
	selected.sector_bytes = sector;
	if (selected.label == NULL)
		selected.label = "NO NAME";
	length = strnlen(selected.label, 12);
	if (length == 0 || length > 11 || selected.label[0] == ' ' ||
	    selected.label[length - 1] == ' ')
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "invalid FAT volume label length or spacing"));
	for (size_t i = 0; i < length; i++) {
		if (!((selected.label[i] >= 'A' && selected.label[i] <= 'Z') ||
		    (selected.label[i] >= '0' && selected.label[i] <= '9') ||
		    strchr(" $%'-_@~`!(){}^#&", selected.label[i]) != NULL))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "FAT label requires uppercase ASCII label characters"));
	}
	status = fsimg_writer_collect_new(ctx, &writer, error);
	if (status != FSIMG_OK)
		return (status);
	writer->variant = variant;
	writer->options = selected;
	bcopy(selected.label, writer->label, length + 1);
	writer->options.label = writer->label;
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
	fsimg_fat_write_free(writer->plan);
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
	if (entry == NULL || (entry->kind == FSIMG_ENTRY_DIRECTORY &&
	    source != NULL) || (entry->kind == FSIMG_ENTRY_FILE && source == NULL)) {
		status = fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "entry kind and source disagree");
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
	status = fsimg_writer_prepare(writer, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	writer->session.progress.phase = "FAT planning";
	status = fsimg_fat_write_plan(writer, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	*out = writer->plan;
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
		    writer->source.identity.inode == identity.inode)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "output aliases a collected host source"));
		status = fsimg_source_finish(writer, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

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
	writer->session.progress.phase = "FAT emission";
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
	status = fsimg_fat_write_emit(writer, output, error);
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
