/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>

#include "generated.h"
#include "io.h"
#include "mtree.h"
#include "source.h"

/* Generated content is hashed through a buffer of this size when its
 * manifest line claims a digest.
 */
#define MANIFEST_DIGEST_BUFFER	(64u * 1024)

/* A manifest is a source root whose scanner is the mtree(5) parser: every
 * entry line becomes exactly what the host scanner appends for a host
 * object, one catalog entry at the root's rank and, for a file or link, one
 * record in the source-record stream. Nothing after collection can tell
 * the two apart.
 *
 * The root starts descriptorless. The caller's content directory, when
 * offered, is held as a candidate and becomes the root's descriptor, with
 * its identity and mount, only when the first entry needing host bytes is
 * parsed; a manifest of generated files never touches it and closes it at
 * the end, so it behaves the same with or without one. Without a candidate
 * an entry needing host bytes is refused.
 *
 * The manifest itself is read through a bounded I/O object over a
 * duplicate of the caller's descriptor, from offset zero to the length it
 * had when the call began, and must be unchanged when the parse ends.
 */
typedef struct manifest_import {
	fsimg_writer_t	*writer;
	fsimg_source_root_t *root;
	fsimg_io_t	*manifest;
	/* One charged allocation: digest_buffer, MANIFEST_DIGEST_BUFFER bytes,
	 * then walk_path, the path limit's bytes.
	 */
	unsigned char	*digest_buffer;
	char		*walk_path;
	uint64_t	 manifest_length;
	uint64_t	 manifest_offset;
	fsimg_host_file_t manifest_identity;
	int		 candidate;
} manifest_import_t;

static fsimg_status_t open_manifest(manifest_import_t *, int,
    fsimg_error_t *);
static fsimg_status_t offer_contents(manifest_import_t *, int,
    fsimg_error_t *);
static fsimg_status_t allocate_buffers(manifest_import_t *, fsimg_error_t *);
static fsimg_status_t register_root(manifest_import_t *, fsimg_error_t *);
static fsimg_status_t check_manifest_unchanged(manifest_import_t *,
    fsimg_error_t *);
static fsimg_status_t read_manifest(void *, unsigned char *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t import_entry(void *, const fsimg_mtree_entry_t *,
    fsimg_error_t *);
static fsimg_status_t import_link(manifest_import_t *,
    const fsimg_mtree_entry_t *, fsimg_error_t *);
static fsimg_status_t import_generated_file(manifest_import_t *,
    const fsimg_mtree_entry_t *, fsimg_error_t *);
static fsimg_status_t import_host_file(manifest_import_t *,
    const fsimg_mtree_entry_t *, fsimg_error_t *);
static fsimg_status_t append_generated(manifest_import_t *,
    const fsimg_entry_t *, const fsimg_generated_t *, fsimg_error_t *);
static fsimg_status_t adopt_content_directory(manifest_import_t *,
    fsimg_error_t *);
static fsimg_status_t inspect_contents_path(manifest_import_t *,
    const char *, fsimg_host_file_t *, fsimg_error_t *);

fsimg_status_t
fsimg_writer_add_manifest(fsimg_writer_t *writer, int manifest_fd,
    int contents_fd, fsimg_error_t *error)
{
	manifest_import_t import;
	fsimg_mtree_input_t input;
	fsimg_status_t status;

	status = fsimg_writer_collect_ready(writer, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(&import, sizeof(import));
	import.writer = writer;
	import.candidate = -1;

	status = open_manifest(&import, manifest_fd, error);
	if (status == FSIMG_OK && contents_fd != -1)
		status = offer_contents(&import, contents_fd, error);
	if (status == FSIMG_OK)
		status = allocate_buffers(&import, error);
	if (status == FSIMG_OK)
		status = register_root(&import, error);
	if (status == FSIMG_OK) {
		input.read = read_manifest;
		input.read_argument = &import;
		input.entry = import_entry;
		input.entry_argument = &import;
		status = fsimg_mtree_parse(&writer->session, &input, error);
	}
	if (status == FSIMG_OK)
		status = check_manifest_unchanged(&import, error);

	/* A candidate no entry needed is closed; an adopted one belongs to
	 * the root, which the registry releases with the other roots.
	 */
	if (import.candidate != -1)
		fsimg_fd_release(writer->ctx, import.candidate);
	fsimg_dealloc(import.digest_buffer);
	fsimg_io_free(import.manifest);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	return (FSIMG_OK);
}

/* The manifest is borrowed: reading goes through a duplicate, so the
 * caller's descriptor, its offset included, is left as it was.
 */
static fsimg_status_t
open_manifest(manifest_import_t *import, int manifest_fd, fsimg_error_t *error)
{
	fsimg_writer_t *writer = import->writer;
	fsimg_status_t status;
	bool known;

	status = fsimg_io_from_fd(writer->ctx, manifest_fd, FSIMG_IO_READ,
	    &import->manifest, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_io_bind_session(import->manifest, &writer->session);
	status = fsimg_io_get_size(import->manifest, &import->manifest_length,
	    error);
	if (status == FSIMG_OK)
		status = fsimg_io_identity(import->manifest,
		    &import->manifest_identity, &known, error);
	if (status != FSIMG_OK)
		return (status);
	if (!known || import->manifest_identity.length != import->manifest_length)
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "manifest changed while it was opened"));
	return (FSIMG_OK);
}

/* Validate and duplicate the offered content directory without taking
 * its identity: that happens when an entry first needs host bytes.
 */
static fsimg_status_t
offer_contents(manifest_import_t *import, int contents_fd, fsimg_error_t *error)
{
	fsimg_writer_t *writer = import->writer;
	fsimg_host_file_t identity;
	fsimg_status_t status;
	int flags, saved;

	if (contents_fd < 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_SOURCE,
		    "content directory descriptor is neither open nor -1"));
	flags = fcntl(contents_fd, F_GETFL);
	if (flags == -1 || fsimg_host_file(contents_fd, &identity) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_SOURCE, "content directory inspection failed"));
	if (identity.kind != FSIMG_HOST_DIRECTORY ||
	    (flags & O_ACCMODE) != O_RDONLY || fsimg_host_unsupported_io(flags))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_SOURCE,
		    "content directory must be a readable directory"));
	status = fsimg_fd_reserve(writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	import->candidate = fsimg_host_dup(contents_fd);
	if (import->candidate == -1) {
		saved = errno;
		fsimg_fd_release(writer->ctx, -1);
		return (fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_SOURCE, "content directory duplication failed"));
	}
	return (FSIMG_OK);
}

static fsimg_status_t
allocate_buffers(manifest_import_t *import, fsimg_error_t *error)
{
	fsimg_writer_t *writer = import->writer;
	fsimg_error_t detail;
	uint64_t total;
	size_t allocation_size;

	if (!fsimg_add_u64(MANIFEST_DIGEST_BUFFER,
	    writer->ctx->options.max_path_bytes, &total) ||
	    !fsimg_to_size(total, &allocation_size))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_SOURCE,
		    "manifest import buffers exceed the host range"));
	import->digest_buffer = fsimg_alloc(writer->ctx, allocation_size,
	    &detail);
	if (import->digest_buffer == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	import->walk_path = (char *)import->digest_buffer +
	    MANIFEST_DIGEST_BUFFER;
	return (FSIMG_OK);
}

/* The root is registered before any entry so that its records rank at its
 * registration order, the operand's position among the writer's roots.
 */
static fsimg_status_t
register_root(manifest_import_t *import, fsimg_error_t *error)
{
	fsimg_writer_t *writer = import->writer;
	fsimg_source_root_t *root;
	fsimg_error_t detail;

	root = fsimg_alloc(writer->ctx, sizeof(*root), &detail);
	if (root == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(root, sizeof(*root));
	root->fd = -1;
	root->id = writer->root_count;
	writer->root_count++;
	root->next = writer->roots;
	writer->roots = root;
	import->root = root;
	return (FSIMG_OK);
}

static fsimg_status_t
check_manifest_unchanged(manifest_import_t *import, fsimg_error_t *error)
{
	fsimg_host_file_t identity;
	fsimg_status_t status;
	bool known;

	status = fsimg_io_identity(import->manifest, &identity, &known, error);
	if (status != FSIMG_OK)
		return (status);
	if (!known ||
	    !fsimg_source_identity_equal(&identity, &import->manifest_identity))
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "manifest changed while it was read"));
	return (FSIMG_OK);
}

/* Each read is exact within the length the manifest had when it was
 * opened, so a manifest that shrinks fails rather than ending early at a
 * line boundary.
 */
static fsimg_status_t
read_manifest(void *argument, unsigned char *buffer, size_t capacity,
    size_t *length, fsimg_error_t *error)
{
	manifest_import_t *import = argument;
	uint64_t remaining;
	size_t request;
	fsimg_status_t status;

	*length = 0;
	remaining = import->manifest_length - import->manifest_offset;
	request = capacity;
	if (request > remaining)
		request = (size_t)remaining;
	if (request == 0) {
		fsimg_error_clear(error);
		return (FSIMG_OK);
	}
	status = fsimg_io_read_exact(import->manifest, import->manifest_offset,
	    buffer, request, length, error);
	if (status != FSIMG_OK)
		return (status);
	import->manifest_offset += *length;
	return (FSIMG_OK);
}

/* The parser has validated the entry's path, kind, keywords and metadata;
 * what remains needs the content directory or the generators.
 */
static fsimg_status_t
import_entry(void *argument, const fsimg_mtree_entry_t *entry,
    fsimg_error_t *error)
{
	manifest_import_t *import = argument;

	switch (entry->entry.kind) {
	case FSIMG_ENTRY_SYMLINK:
		return (import_link(import, entry, error));
	case FSIMG_ENTRY_FILE:
		if (entry->content == FSIMG_MTREE_CONTENT_HOST)
			return (import_host_file(import, entry, error));
		return (import_generated_file(import, entry, error));
	default:
		return (fsimg_writer_collect(import->writer, &entry->entry,
		    FSIMG_STORAGE_NONE, 0, import->root, error));
	}
}

/* A link's target is bytes the manifest states; it is stored in the
 * link's record and never resolved.
 */
static fsimg_status_t
import_link(manifest_import_t *import, const fsimg_mtree_entry_t *entry,
    fsimg_error_t *error)
{
	fsimg_generated_t generated;

	bzero(&generated, sizeof(generated));
	generated.generator = FSIMG_GENERATOR_LINK;
	generated.length = entry->link_length;
	generated.target = (const unsigned char *)entry->link;
	return (append_generated(import, &entry->entry, &generated, error));
}

/* Zero, seeded and positional content. A claimed digest is checked here,
 * where the bytes already exist, so a manifest whose generator and digest
 * disagree fails before any plan. A zero file is the zero source, which
 * has no record.
 */
static fsimg_status_t
import_generated_file(manifest_import_t *import,
    const fsimg_mtree_entry_t *entry, fsimg_error_t *error)
{
	unsigned char digest[FSIMG_SHA256_DIGEST_LENGTH];
	fsimg_generated_t generated;
	fsimg_status_t status;

	bzero(&generated, sizeof(generated));
	generated.length = entry->entry.size;
	switch (entry->content) {
	case FSIMG_MTREE_CONTENT_SEEDED:
		generated.generator = FSIMG_GENERATOR_SEEDED;
		break;
	case FSIMG_MTREE_CONTENT_POSITIONAL:
		generated.generator = FSIMG_GENERATOR_POSITIONAL;
		break;
	case FSIMG_MTREE_CONTENT_ZERO:
	case FSIMG_MTREE_CONTENT_HOST:
		/* Unreachable: import_entry sends host content to
		 * import_host_file.
		 */
		generated.generator = FSIMG_GENERATOR_ZERO;
		break;
	}
	if (generated.generator != FSIMG_GENERATOR_ZERO) {
		generated.seed = entry->seed;
		generated.hole_count = entry->hole_count;
	}
	/* Without fsimg.holes the parser leaves holes NULL. */
	if (entry->hole_count != 0)
		bcopy(entry->holes, generated.holes,
		    entry->hole_count * sizeof(generated.holes[0]));

	if (entry->digest_given) {
		status = fsimg_generated_digest(&import->writer->session,
		    &generated, import->digest_buffer, MANIFEST_DIGEST_BUFFER,
		    digest, error);
		if (status != FSIMG_OK)
			return (status);
		if (bcmp(digest, entry->digest, sizeof(digest)) != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_SOURCE, "generated content does not match sha256digest"));
	}
	if (generated.generator == FSIMG_GENERATOR_ZERO)
		return (fsimg_writer_collect(import->writer, &entry->entry,
		    FSIMG_STORAGE_ZERO, 0, import->root, error));
	return (append_generated(import, &entry->entry, &generated, error));
}

static fsimg_status_t
append_generated(manifest_import_t *import, const fsimg_entry_t *entry,
    const fsimg_generated_t *generated, fsimg_error_t *error)
{
	fsimg_writer_t *writer = import->writer;
	uint64_t reference;
	size_t length;
	fsimg_status_t status;

	length = fsimg_generated_encode(generated, writer->source_record);
	status = fsimg_stream_append(writer->host_sources, writer->source_record,
	    length, &reference, error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_writer_collect(writer, entry, FSIMG_STORAGE_GENERATED,
	    reference, import->root, error));
}

/* A file whose bytes are a host file beneath the content directory: the
 * contents= path, else the entry's own image path. The file contributes
 * its bytes and length only; owner, mode and times come from the manifest.
 * Its digest, when claimed, is checked as emission copies it, so input
 * reads no payload.
 */
static fsimg_status_t
import_host_file(manifest_import_t *import, const fsimg_mtree_entry_t *entry,
    fsimg_error_t *error)
{
	fsimg_writer_t *writer = import->writer;
	fsimg_host_file_t identity;
	fsimg_entry_t collected;
	const char *relative;
	uint64_t reference;
	fsimg_status_t status;

	status = adopt_content_directory(import, error);
	if (status != FSIMG_OK)
		return (status);
	relative = entry->contents != NULL ? entry->contents :
	    entry->entry.path + 1;
	status = inspect_contents_path(import, relative, &identity, error);
	if (status != FSIMG_OK)
		return (status);
	if (entry->size_given && entry->entry.size != identity.length)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_SOURCE,
		    "size differs from the contents file's length"));

	status = fsimg_source_append_host(writer, import->root, &identity,
	    entry->digest_given ? entry->digest : NULL, relative, &reference,
	    error);
	if (status != FSIMG_OK)
		return (status);
	collected = entry->entry;
	collected.size = identity.length;
	return (fsimg_writer_collect(writer, &collected, FSIMG_STORAGE_HOST,
	    reference, import->root, error));
}

/* The root takes the candidate's identity and mount the first time an
 * entry needs host bytes; every later contents path is walked beneath it.
 */
static fsimg_status_t
adopt_content_directory(manifest_import_t *import, fsimg_error_t *error)
{
	fsimg_writer_t *writer = import->writer;
	fsimg_source_root_t *root = import->root;
	fsimg_host_file_t identity;
	uint64_t mount_id;
	fsimg_status_t status;
	int saved;

	if (root->fd != -1)
		return (FSIMG_OK);
	if (import->candidate == -1)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_SOURCE,
		    "entry needs host bytes but the manifest has no content directory"));
	if (fsimg_host_file(import->candidate, &identity) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_SOURCE, "content directory inspection failed"));
	status = fsimg_source_check_object(writer, import->candidate, &identity,
	    NULL, error);
	if (status != FSIMG_OK)
		return (status);
	if (fsimg_host_mount_id(import->candidate, &mount_id) == -1) {
		saved = errno;
		return (fsimg_error_set(error, saved == ENOTSUP ?
		    FSIMG_EUNSUPPORTED : FSIMG_EIO, saved, FSIMG_OP_SOURCE,
		    "content directory mount identity is unavailable"));
	}
	root->fd = import->candidate;
	root->identity = identity;
	root->mount_id = mount_id;
	import->candidate = -1;
	return (FSIMG_OK);
}

/* Walk a contents path beneath the content directory as reopening walks a
 * scanned path, never following a link and never leaving the directory,
 * and require a regular file at its end. The walk splits a copy of the
 * path in import->walk_path. Only the file's identity is taken; its
 * descriptor is closed again.
 */
static fsimg_status_t
inspect_contents_path(manifest_import_t *import, const char *relative,
    fsimg_host_file_t *identity, fsimg_error_t *error)
{
	fsimg_writer_t *writer = import->writer;
	fsimg_source_root_t *root = import->root;
	char *name;
	size_t length;
	fsimg_status_t status;
	int fd, parent;

	length = strlen(relative) + 1;
	if (length > writer->ctx->options.max_path_bytes)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
		    writer->ctx->options.max_path_bytes, length));
	bcopy(relative, import->walk_path, length);

	status = fsimg_source_walk(writer, root, import->walk_path,
	    FSIMG_WALK_IMPORT, &parent, &name, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_source_open_child(writer, root, parent, name, false,
	    FSIMG_WALK_IMPORT, &fd, identity, error);
	if (parent != root->fd)
		fsimg_fd_release(writer->ctx, parent);
	if (status != FSIMG_OK)
		return (status);
	fsimg_fd_release(writer->ctx, fd);
	if (identity->kind != FSIMG_HOST_FILE)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_SOURCE, "contents path does not name a regular file"));
	return (FSIMG_OK);
}
