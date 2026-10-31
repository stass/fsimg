/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>

#include "host.h"
#include "input.h"
#include "internal.h"

static fsimg_status_t input_query_error(fsimg_error_t *);

fsimg_status_t
fsimg_input_validate(const fsimg_input_t *input, fsimg_error_t *error)
{
	off_t host_length;

	switch (input->kind) {
	case FSIMG_INPUT_ABSENT:
	case FSIMG_INPUT_STREAM:
		if (input->length == 0 && input->quantum == 0)
			return (FSIMG_OK);
		break;
	case FSIMG_INPUT_FILE:
		if (input->quantum != 1)
			break;
		if (fsimg_host_offset(input->length, &host_length))
			return (FSIMG_OK);
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_IO,
		    "input length exceeds host range"));
	case FSIMG_INPUT_DEVICE:
		if (input->length == 0 || input->quantum == 0 ||
		    input->quantum > input->length ||
		    input->length % input->quantum != 0)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_IO, "device has unsupported capacity or sector size"));
		if (!fsimg_host_offset(input->length, &host_length) ||
		    input->quantum > SIZE_MAX || input->quantum > SSIZE_MAX)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_IO,
			    "device geometry exceeds host range"));
		return (FSIMG_OK);
	}
	return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IO,
	    "invalid prepared image input"));
}

static fsimg_status_t
input_query_error(fsimg_error_t *error)
{
	int saved = errno;
	fsimg_status_t status;

	status = FSIMG_EIO;
	if (saved == EOVERFLOW)
		status = FSIMG_ERANGE;
	else if (saved == ENOTTY || saved == ENOTSUP || saved == ENOSYS)
		status = FSIMG_EUNSUPPORTED;
	return (fsimg_error_set(error, status, saved, FSIMG_OP_IO,
	    "cannot discover disk device geometry"));
}

fsimg_status_t
fsimg_input_check(int fd, const fsimg_input_t *input, fsimg_error_t *error)
{
	struct stat st;
	fsimg_status_t status;
	int flags;

	status = fsimg_input_validate(input, error);
	if (status != FSIMG_OK)
		return (status);
	flags = fcntl(fd, F_GETFL);
	if (flags == -1 || fsimg_host_fstat(fd, &st) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno, FSIMG_OP_IO,
		    "image descriptor query failed"));
	if ((flags & O_ACCMODE) != O_RDONLY || fsimg_host_unsupported_io(flags))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IO,
		    "image input requires ordinary read-only access"));
	if (input->kind == FSIMG_INPUT_FILE && S_ISREG(st.st_mode)) {
		if (st.st_size < 0 || (uint64_t)st.st_size != input->length)
			return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
			    FSIMG_OP_IO, "image length changed after preparation"));
		return (FSIMG_OK);
	}
	if (input->kind == FSIMG_INPUT_DEVICE && fsimg_host_disk_mode(st.st_mode))
		return (FSIMG_OK);
	return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IO,
	    "image descriptor does not match its prepared kind"));
}

fsimg_status_t
fsimg_input_prepare(int fd, fsimg_input_checkpoint_t checkpoint, void *argument,
    fsimg_input_t *out, fsimg_error_t *error)
{
	fsimg_input_t input = { 0 };
	struct stat st;
	fsimg_status_t status;
	int flags;

	*out = input;
	if (checkpoint != NULL) {
		status = checkpoint(argument, error);
		if (status != FSIMG_OK)
			return (status);
	}
	flags = fcntl(fd, F_GETFL);
	if (flags == -1 || fsimg_host_fstat(fd, &st) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno, FSIMG_OP_IO,
		    "image descriptor query failed"));
	if ((flags & O_ACCMODE) != O_RDONLY || fsimg_host_unsupported_io(flags))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IO,
		    "image input requires ordinary read-only access"));
	if (S_ISREG(st.st_mode)) {
		if (st.st_size < 0)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_IO,
			    "negative image length"));
		input.kind = FSIMG_INPUT_FILE;
		input.length = (uint64_t)st.st_size;
		input.quantum = 1;
	} else {
		if (!fsimg_host_disk_mode(st.st_mode))
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_IO, "image is not a regular file or supported disk device"));
		input.kind = FSIMG_INPUT_DEVICE;
		if (fsimg_host_disk_sector(fd, &input.quantum) == -1)
			return (input_query_error(error));
		if (checkpoint != NULL) {
			status = checkpoint(argument, error);
			if (status != FSIMG_OK)
				return (status);
		}
		if (input.quantum == 0)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_IO, "device reports a zero sector size"));
		if (fsimg_host_disk_size(fd, input.quantum, &input.length) == -1)
			return (input_query_error(error));
	}
	if (checkpoint != NULL) {
		status = checkpoint(argument, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = fsimg_input_validate(&input, error);
	if (status == FSIMG_OK)
		*out = input;
	return (status);
}
