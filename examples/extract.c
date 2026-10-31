/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "example.h"

int
main(int argc, char **argv)
{
	fsimg_ctx_t *ctx = NULL;
	fsimg_io_t *input = NULL;
	fsimg_reader_t *reader = NULL;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	fsimg_status_t status;
	int fd, parent, result;

	if (argc != 5) {
		fprintf(stderr, "usage: example-extract image output_parent new_basename private_scratch\n");
		return (2);
	}
	fd = open(argv[1], O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd == -1)
		return (example_host_failure());
	parent = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (parent == -1) {
		result = example_host_failure();
		close(fd);
		return (result);
	}
	status = example_context(argv[4], &ctx, &error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_io_from_fd(ctx, fd, FSIMG_IO_READ, &input, &error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_reader_open(ctx, input, NULL, &reader, &error);
	if (status != FSIMG_OK)
		goto done;
	fsimg_extract_options_init(&options);
	/* Default extraction restores content in a fresh private tree. It does
	 * not restore timestamps or sandbox this embedding application.
	 */
	status = fsimg_extract_at(reader, parent, argv[3], &options, &error);
done:
	result = status == FSIMG_OK ? 0 : example_failure(&error);
	if (status != FSIMG_OK && error.output_state == FSIMG_OUTPUT_INCOMPLETE)
		fprintf(stderr, "The new extraction directory contains incomplete output.\n");
	fsimg_reader_free(reader);
	fsimg_io_free(input);
	fsimg_ctx_free(ctx);
	if (close(fd) == -1 && result == 0)
		result = example_host_failure();
	if (close(parent) == -1 && result == 0)
		result = example_host_failure();
	return (result);
}
