/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "example.h"

static fsimg_status_t
stdout_failure(fsimg_error_t *error)
{
	int saved = errno;

	bzero(error, sizeof(*error));
	error->status = FSIMG_EIO;
	error->operation = FSIMG_OP_IO;
	error->os_error = saved;
	return (FSIMG_EIO);
}

static bool
decimal(const char *text, uint64_t *out)
{
	uint64_t value;
	unsigned digit;

	if (*text < '0' || *text > '9')
		return (false);
	value = 0;
	for (; *text != '\0'; text++) {
		if (*text < '0' || *text > '9')
			return (false);
		digit = (unsigned)(*text - '0');
		if (value > (UINT64_MAX - digit) / 10)
			return (false);
		value = value * 10 + digit;
	}
	*out = value;
	return (true);
}

int
main(int argc, char **argv)
{
	fsimg_ctx_t *ctx = NULL;
	fsimg_io_t *input = NULL, *slice = NULL;
	fsimg_reader_t *reader = NULL;
	const fsimg_entry_t *entry;
	fsimg_error_t error;
	fsimg_status_t status;
	unsigned char sample[64];
	uint64_t offset, length;
	size_t transferred;
	int fd, result;

	if (argc != 5 || !decimal(argv[2], &offset) || !decimal(argv[3], &length)) {
		fprintf(stderr, "usage: example-read-slice image offset_bytes length_bytes private_scratch\n");
		return (2);
	}
	fd = open(argv[1], O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd == -1)
		return (example_host_failure());
	status = example_context(argv[4], &ctx, &error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_io_from_fd(ctx, fd, FSIMG_IO_READ, &input, &error);
	if (status != FSIMG_OK)
		goto done;
	/* The slice borrows input. Its declared range is checked before parsing. */
	status = fsimg_io_slice(input, offset, length, &slice, &error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_reader_open(ctx, slice, NULL, &reader, &error);
	if (status != FSIMG_OK)
		goto done;
	while ((status = fsimg_reader_next(reader, &entry, &error)) == FSIMG_OK) {
		if (entry->kind != FSIMG_ENTRY_FILE)
			continue;
		status = fsimg_reader_read_at(reader, 0, sample, sizeof(sample), &transferred, &error);
		/* Confirmed bytes can accompany failure. Do not use that prefix as
		 * proof that the complete file or volume was successfully read.
		 */
		if (status != FSIMG_OK)
			goto done;
		if (printf("file logical_bytes=%llu sampled_bytes=%zu\n",
		    (unsigned long long)entry->size, transferred) < 0) {
			status = stdout_failure(&error);
			goto done;
		}
	}
	if (status == FSIMG_DONE) {
		if (fflush(stdout) == EOF)
			status = stdout_failure(&error);
		else
			status = FSIMG_OK;
	}
done:
	result = status == FSIMG_OK ? 0 : example_failure(&error);
	fsimg_reader_free(reader);
	fsimg_io_free(slice);
	fsimg_io_free(input);
	fsimg_ctx_free(ctx);
	if (close(fd) == -1 && result == 0)
		result = example_host_failure();
	return (result);
}
