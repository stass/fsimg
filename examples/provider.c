/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "example.h"

#include <string.h>

typedef struct example_content {
	const char	*bytes;
	size_t		 length;
} example_content_t;

static fsimg_status_t content_validate(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t content_read(void *, uint64_t, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);

static fsimg_status_t
content_validate(void *argument, uint64_t id, fsimg_error_t *error)
{
	const example_content_t *content = argument;

	if (id == 1 && content->bytes != NULL)
		return (FSIMG_OK);
	if (error != NULL) {
		bzero(error, sizeof(*error));
		error->status = FSIMG_ESOURCE_CHANGED;
		error->operation = FSIMG_OP_SOURCE;
	}
	return (FSIMG_ESOURCE_CHANGED);
}

static fsimg_status_t
content_read(void *argument, uint64_t id, uint64_t offset, void *buffer,
    size_t requested, size_t *transferred, fsimg_error_t *error)
{
	const example_content_t *content = argument;
	fsimg_status_t status;

	*transferred = 0;
	status = content_validate(argument, id, error);
	if (status != FSIMG_OK)
		return (status);
	if (offset > content->length) {
		if (error != NULL) {
			bzero(error, sizeof(*error));
			error->status = FSIMG_ERANGE;
			error->operation = FSIMG_OP_SOURCE;
		}
		return (FSIMG_ERANGE);
	}
	if (requested > content->length - offset)
		requested = content->length - (size_t)offset;
	/* Short successful reads are legal. The common layer completes the copy. */
	if (requested > 7)
		requested = 7;
	bcopy(content->bytes + (size_t)offset, buffer, requested);
	*transferred = requested;
	return (FSIMG_OK);
}

int
main(int argc, char **argv)
{
	example_content_t content = { .bytes = "hello from a source provider\n" };
	fsimg_ctx_t *ctx = NULL;
	fsimg_writer_t *writer = NULL;
	fsimg_entry_t entry = { .path = "/provider.txt", .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER, .id = 1 };
	fsimg_source_ops_t provider;
	fsimg_write_options_t options;
	fsimg_error_t error;
	fsimg_status_t status;
	int parent, result;

	if (argc != 4) {
		fprintf(stderr, "usage: example-provider output_parent basename private_scratch\n");
		return (2);
	}
	parent = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (parent == -1)
		return (example_host_failure());
	content.length = strlen(content.bytes);
	entry.size = content.length;
	status = example_context(argv[3], &ctx, &error);
	if (status != FSIMG_OK)
		goto done;
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	status = fsimg_writer_new(ctx, "exfat", &options, &writer, &error);
	if (status != FSIMG_OK)
		goto done;
	fsimg_source_ops_init(&provider);
	provider.validate = content_validate;
	provider.read_at = content_read;
	status = fsimg_writer_set_sources(writer, &provider, &content, &error);
	if (status != FSIMG_OK)
		goto done;
	/* Content and callback state stay borrowed until writer destruction and
	 * are disjoint from the output's storage. They contain no staged payload.
	 */
	status = fsimg_writer_add(writer, &entry, &source, &error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_writer_write_at(writer, parent, argv[2], &error);
done:
	result = status == FSIMG_OK ? 0 : example_failure(&error);
	fsimg_writer_free(writer);
	fsimg_ctx_free(ctx);
	if (close(parent) == -1 && result == 0)
		result = example_host_failure();
	return (result);
}
