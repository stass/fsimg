/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>

#include "source.h"
#include "unicode.h"

#define DIRECTORY_BUFFER_SIZE 8192u

typedef struct scan_directory {
	fsimg_host_file_t identity;
	size_t		 path_length;
	size_t		 cursor;
	size_t		 used;
	int		 fd;
	unsigned	 interruptions;
	unsigned char	 buffer[DIRECTORY_BUFFER_SIZE];
} scan_directory_t;

/* Each frame owns an open directory and its unread record batch. Frames
 * remember a prefix length into one shared relative path: descending only
 * appends components, so returning to a parent restores its prefix by length.
 * Directory width never adds resident frames or retained child names.
 */

static fsimg_status_t open_object(fsimg_writer_t *, int, const char *,
    bool, int *, fsimg_host_file_t *, fsimg_error_t *);
static fsimg_status_t scan_entry(fsimg_writer_t *, fsimg_source_root_t *,
    const fsimg_host_file_t *, const char *, const char *, fsimg_error_t *);
static fsimg_status_t directory_unchanged(scan_directory_t *, fsimg_error_t *);
static int read_directory_batch(fsimg_writer_t *, scan_directory_t *);

/* Open one object without following a link. A child that the no-follow
 * open refuses as a symbolic link is inspected instead: identity describes
 * the link, whose target is read once to learn its length (reopening reads
 * it again), and *out stays -1 because a link has no descriptor.
 */
static fsimg_status_t
open_object(fsimg_writer_t *writer, int parent, const char *name, bool directory,
    int *out, fsimg_host_file_t *identity, fsimg_error_t *error)
{
	fsimg_status_t status;
	unsigned char target[FSIMG_SYMLINK_TARGET_MAX + 1];
	int fd, saved;

	*out = -1;
	status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fd_reserve(writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	fd = fsimg_host_source_open(parent, name, directory);
	if (fd == -1) {
		saved = errno;
		fsimg_fd_release(writer->ctx, -1);
		if (saved == ELOOP && !directory)
			return (fsimg_source_read_link(writer, parent, name,
			    identity, target, error));
		/* A link beneath a directory open, and an object no open
		 * reads (ENXIO: a socket or a device without a driver), are
		 * kinds import rejects, as it rejects a FIFO once opened.
		 */
		return (fsimg_error_set(error,
		    saved == ELOOP || saved == ENXIO ? FSIMG_EUNSUPPORTED :
		    saved == ENOENT ? FSIMG_ESOURCE_CHANGED : FSIMG_EIO,
		    saved, FSIMG_OP_SOURCE, "source object open failed"));
	}
	if (fsimg_host_file(fd, identity) == -1) {
		saved = errno;
		fsimg_fd_release(writer->ctx, fd);
		return (fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_SOURCE, "opened source object inspection failed"));
	}
	*out = fd;
	return (FSIMG_OK);
}

/* Files and links are host sources: their records hold the identity that
 * reopening must find again and the relative path that leads to it.
 * Directories have no source. Owner and mode are optional fields that the
 * scan records only on request.
 */
static fsimg_status_t
scan_entry(fsimg_writer_t *writer, fsimg_source_root_t *root,
    const fsimg_host_file_t *identity, const char *image_path,
    const char *relative_path, fsimg_error_t *error)
{
	fsimg_entry_t entry = { .path = image_path, .metadata = identity->metadata };
	fsimg_source_storage_t storage;
	uint64_t reference;
	fsimg_status_t status;

	storage = FSIMG_STORAGE_NONE;
	reference = 0;
	entry.kind = FSIMG_ENTRY_DIRECTORY;
	if ((writer->options.flags & FSIMG_WRITE_IMPORT_POSIX) != 0)
		entry.metadata.valid |= FSIMG_META_OWNER | FSIMG_META_MODE;
	if (identity->kind == FSIMG_HOST_FILE || identity->kind == FSIMG_HOST_LINK) {
		entry.kind = identity->kind == FSIMG_HOST_FILE ? FSIMG_ENTRY_FILE :
		    FSIMG_ENTRY_SYMLINK;
		entry.size = identity->length;
		storage = FSIMG_STORAGE_HOST;
		status = fsimg_source_append_host(writer, root, identity, NULL,
		    relative_path, &reference, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (fsimg_writer_collect(writer, &entry, storage, reference, root,
	    error));
}

/* The path is placed first, with bcopy's overlap-safe copy, because a
 * caller may have built it in the record already.
 */
fsimg_status_t
fsimg_source_append_host(fsimg_writer_t *writer, const fsimg_source_root_t *root,
    const fsimg_host_file_t *identity, const unsigned char *digest,
    const char *relative_path, uint64_t *reference, fsimg_error_t *error)
{
	unsigned char *record = writer->source_record;
	size_t path_length;

	path_length = strlen(relative_path) + 1;
	if (path_length > writer->ctx->options.max_path_bytes)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
		    writer->ctx->options.max_path_bytes, path_length));
	bcopy(relative_path, record + FSIMG_SOURCE_HEADER, path_length);
	bzero(record, FSIMG_SOURCE_HEADER);
	fsimg_meta_put_u64(record, root->id);
	fsimg_meta_put_u64(record + 8, identity->device);
	fsimg_meta_put_u64(record + 16, identity->inode);
	fsimg_meta_put_u64(record + 24, identity->length);
	fsimg_meta_put_u64(record + 32, (uint64_t)identity->modified.seconds);
	fsimg_meta_put_u64(record + 40, identity->modified.nanoseconds);
	fsimg_meta_put_u64(record + 48, (uint64_t)identity->changed.seconds);
	fsimg_meta_put_u64(record + 56, identity->changed.nanoseconds);
	fsimg_meta_put_u64(record + 64, identity->kind);
	fsimg_meta_put_u64(record + 72, 1);
	if (digest != NULL) {
		fsimg_meta_put_u64(record + 80, 1);
		bcopy(digest, record + FSIMG_SOURCE_DIGEST,
		    FSIMG_SHA256_DIGEST_LENGTH);
	}
	return (fsimg_stream_append(writer->host_sources, record,
	    FSIMG_SOURCE_HEADER + path_length, reference, error));
}

static fsimg_status_t
directory_unchanged(scan_directory_t *directory, fsimg_error_t *error)
{
	fsimg_host_file_t identity;

	if (fsimg_host_file(directory->fd, &identity) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_SOURCE, "source directory inspection failed"));
	if (!fsimg_source_identity_equal(&directory->identity, &identity))
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "source directory changed during scanning"));
	return (FSIMG_OK);
}

static int
read_directory_batch(fsimg_writer_t *writer, scan_directory_t *directory)
{
	if (writer->interrupt_directory_read_after != 0 &&
	    --writer->interrupt_directory_read_after == 0) {
		directory->used = 0;
		errno = EINTR;
		return (-1);
	}
	return (fsimg_host_directory_read(directory->fd, directory->buffer,
	    sizeof(directory->buffer), &directory->used));
}

fsimg_status_t
fsimg_source_scan(fsimg_writer_t *writer, fsimg_source_root_t *root,
    const char *root_image_path, fsimg_error_t *error)
{
	scan_directory_t *directories, *current;
	fsimg_host_file_t identity;
	fsimg_error_t detail;
	char *image_path, *relative_path;
	const char *name;
	uint64_t stack_bytes;
	size_t depth, image_length, prefix_length, name_length, relative_length;
	size_t allocation_size;
	uint32_t image_depth;
	fsimg_status_t status;
	int fd, result, saved;

	/* A regular-file root is one file entry at root_image_path. Its source
	 * record's empty relative path names the root descriptor itself, the
	 * identity recorded here is the one adoption inspected, and there is
	 * nothing to traverse.
	 */
	if (root->identity.kind == FSIMG_HOST_FILE) {
		status = scan_entry(writer, root, &root->identity,
		    root_image_path, "", error);
		if (status != FSIMG_OK)
			fsimg_error_path(error, root_image_path);
		return (status);
	}

	if (!fsimg_mul_u64((uint64_t)writer->ctx->options.max_depth + 1,
	    sizeof(*directories), &stack_bytes) ||
	    !fsimg_to_size(stack_bytes, &allocation_size))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_SOURCE, "source traversal stack exceeds host range"));
	directories = fsimg_alloc(writer->ctx, allocation_size, &detail);
	if (directories == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	image_path = fsimg_alloc(writer->ctx, writer->ctx->options.max_path_bytes,
	    &detail);
	if (image_path == NULL) {
		fsimg_dealloc(directories);
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	relative_path = fsimg_alloc(writer->ctx, writer->ctx->options.max_path_bytes,
	    &detail);
	if (relative_path == NULL) {
		fsimg_dealloc(image_path);
		fsimg_dealloc(directories);
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	prefix_length = strlen(root_image_path);
	bcopy(root_image_path, image_path, prefix_length + 1);
	depth = 0;
	status = open_object(writer, root->fd, ".", true, &fd, &identity, error);
	if (status != FSIMG_OK)
		goto done;
	directories[0].fd = fd;
	directories[0].identity = identity;
	directories[0].path_length = 0;
	if (prefix_length != 1) {
		status = scan_entry(writer, root, &identity, image_path, "", error);
		if (status != FSIMG_OK)
			goto close_directories;
	}
	for (;;) {
		current = &directories[depth];
		status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			break;
		result = fsimg_host_directory_name(current->buffer, current->used,
		    &current->cursor, &name, &name_length);
		if (result == -1) {
			status = fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_SOURCE, "invalid host directory entry");
			break;
		}
		if (result == 0) {
			/* A failed refill may clear used; retries must not parse the
			 * previous batch's exhausted cursor against that empty buffer.
			 */
			current->cursor = 0;
			result = read_directory_batch(writer, current);
			if (result == -1 && errno == EINTR && ++current->interruptions <= 1024)
				continue;
			if (result == -1) {
				status = fsimg_error_set(error, FSIMG_EIO, errno,
				    FSIMG_OP_SOURCE, "source directory read failed");
				break;
			}
			current->interruptions = 0;
			current->cursor = 0;
			if (current->used != 0)
				continue;
			status = directory_unchanged(current, error);
			if (status != FSIMG_OK)
				break;
			fsimg_fd_release(writer->ctx, current->fd);
			current->fd = -1;
			if (depth == 0)
				break;
			depth--;
			continue;
		}
		if ((name_length == 1 && name[0] == '.') ||
		    (name_length == 2 && name[0] == '.' && name[1] == '.'))
			continue;
		relative_length = current->path_length;
		if (relative_length != 0)
			relative_path[relative_length++] = '/';
		if (name_length + 1 > writer->ctx->options.max_path_bytes - relative_length) {
			status = fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
			    writer->ctx->options.max_path_bytes, relative_length + name_length + 1);
			break;
		}
		bcopy(name, relative_path + relative_length, name_length + 1);
		relative_length += name_length;
		image_length = prefix_length == 1 ? 1 : prefix_length + 1;
		if (relative_length + 1 > writer->ctx->options.max_path_bytes - image_length) {
			status = fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
			    writer->ctx->options.max_path_bytes, image_length + relative_length + 1);
			break;
		}
		if (prefix_length != 1)
			image_path[prefix_length] = '/';
		bcopy(relative_path, image_path + image_length, relative_length + 1);
		status = fsimg_path_validate(writer->ctx, image_path, false,
		    &image_length, &image_depth, error);
		if (status != FSIMG_OK)
			break;
		/* A link has no descriptor (fd -1); everything else is open.
		 * Any failure names the object by its image path.
		 */
		status = open_object(writer, current->fd, name, false, &fd, &identity, error);
		if (status == FSIMG_OK)
			status = fsimg_source_check_object(writer, fd, &identity, root, error);
		if (status == FSIMG_OK)
			status = scan_entry(writer, root, &identity, image_path, relative_path, error);
		if (status != FSIMG_OK) {
			/* identity may be partial here; fd alone says what is open. */
			if (fd != -1)
				fsimg_fd_release(writer->ctx, fd);
			fsimg_error_path(error, image_path);
			break;
		}
		if (identity.kind == FSIMG_HOST_DIRECTORY) {
			depth++;
			bzero(&directories[depth], sizeof(directories[depth]));
			directories[depth].identity = identity;
			directories[depth].fd = fd;
			directories[depth].path_length = relative_length;
		} else if (identity.kind != FSIMG_HOST_LINK) {
			fsimg_fd_release(writer->ctx, fd);
		}
	}
close_directories:
	saved = errno;
	for (size_t i = 0; i <= depth; i++) {
		if (directories[i].fd != -1)
			fsimg_fd_release(writer->ctx, directories[i].fd);
	}
	errno = saved;
done:
	fsimg_dealloc(relative_path);
	fsimg_dealloc(image_path);
	fsimg_dealloc(directories);
	return (status);
}
