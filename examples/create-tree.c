/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "example.h"

int
main(int argc, char **argv)
{
	fsimg_ctx_t *ctx = NULL;
	fsimg_writer_t *writer = NULL;
	fsimg_write_options_t options;
	fsimg_error_t error;
	fsimg_status_t status;
	int source, parent, result;

	if (argc != 5) {
		fprintf(stderr, "usage: example-create-tree source_directory output_parent basename private_scratch\n");
		return (2);
	}
	source = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (source == -1)
		return (example_host_failure());
	parent = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (parent == -1) {
		result = example_host_failure();
		close(source);
		return (result);
	}
	status = example_context(argv[4], &ctx, &error);
	if (status != FSIMG_OK)
		goto done;
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	options.label = "EXAMPLE";
	status = fsimg_writer_new(ctx, "exfat", &options, &writer, &error);
	if (status != FSIMG_OK)
		goto done;
	/* Keep the source immutable until writer destruction. Metadata checks
	 * detect ordinary changes; hostile mutation requires a stable snapshot.
	 */
	status = fsimg_writer_add_tree(writer, source, "/", &error);
	if (status != FSIMG_OK)
		goto done;
	/* This helper plans before emission and publishes without replacement.
	 * Inspect output_state/cleanup_error before deciding how to handle failure.
	 */
	status = fsimg_writer_write_at(writer, parent, argv[3], &error);
done:
	result = status == FSIMG_OK ? 0 : example_failure(&error);
	fsimg_writer_free(writer);
	fsimg_ctx_free(ctx);
	if (close(source) == -1 && result == 0)
		result = example_host_failure();
	if (close(parent) == -1 && result == 0)
		result = example_host_failure();
	return (result);
}
