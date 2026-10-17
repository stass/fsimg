/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <limits.h>
#include <strings.h>

#include "source.h"
#include "writer.h"

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
