/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "qnx6.h"

static fsimg_status_t qnx6_open(fsimg_reader_t *,
    const fsimg_format_variant_t *, fsimg_error_t *);
static void qnx6_close(fsimg_reader_t *);
static fsimg_status_t qnx6_resolve(fsimg_reader_t *, uint64_t,
    fsimg_extent_t *, fsimg_error_t *);

const fsimg_reader_ops_t fsimg_qnx6_reader_ops = {
	.open = qnx6_open,
	.close = qnx6_close,
	.prepare = qnx6_prepare,
	.replay = qnx6_replay,
	.keys = qnx6_keys,
	.key = qnx6_key,
	.ambiguous = "duplicate QNX6 directory name",
	.resolve = qnx6_resolve,
	.verify = qnx6_verify
};

fsimg_qnx6_reader_t *
qnx6_state(const fsimg_reader_t *reader)
{
	return (reader->driver);
}

fsimg_status_t
qnx6_corrupt(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_READER,
	    message));
}

fsimg_status_t
qnx6_unsupported(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_READER,
	    message));
}

/* Open in dependency order: the selected header fixes the geometry; the
 * bitmap validates itself; every later tree walk checks allocation bits
 * against it. The long-name tree is claimed before any directory, so a
 * directory sharing its storage is found when that directory is entered.
 */
static fsimg_status_t
qnx6_open(fsimg_reader_t *reader,
    const fsimg_format_variant_t *forced FSIMG_UNUSED, fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6;
	qnx6_object_t inodes, names;
	qnx6_inode_t root;
	fsimg_error_t detail;
	fsimg_status_t status;

	qnx6 = fsimg_alloc(reader->ctx, sizeof(*qnx6), &detail);
	if (qnx6 == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(qnx6, sizeof(*qnx6));
	reader->driver = qnx6;
	status = qnx6_headers_read(reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = qnx6_bitmap_open(reader, error);
	if (status != FSIMG_OK)
		return (status);

	inodes = (qnx6_object_t){
		.tree = &qnx6->header.inodes,
		.owner = QNX6_OWNER_INODES,
		.domain = QNX6_DOMAIN_SYSTEM,
		.role = QNX6_ROLE_INODES,
		.policy = QNX6_WALK_ALLOCATED
	};
	status = qnx6_index_build(reader, &inodes, &qnx6->inodes, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_bitset_new(&reader->session, qnx6->geometry.blocks,
	    &qnx6->claimed, error);
	if (status == FSIMG_OK)
		status = fsimg_bitset_new(&reader->session,
		    (uint64_t)qnx6->geometry.inode_count + 1, &qnx6->exposed,
		    error);
	if (status != FSIMG_OK)
		return (status);
	names = (qnx6_object_t){
		.tree = &qnx6->header.names,
		.owner = QNX6_OWNER_NAMES,
		.domain = QNX6_DOMAIN_CONTENT,
		.role = QNX6_ROLE_NAMES,
		.policy = QNX6_WALK_EXCLUSIVE
	};
	status = qnx6_index_build(reader, &names, &qnx6->names, error);
	if (status != FSIMG_OK)
		return (status);
	status = qnx6_inode_read(reader, QNX6_ROOT_INODE, &root, error);
	if (status != FSIMG_OK)
		return (status);
	if ((root.mode & QNX6_MODE_TYPE) != QNX6_MODE_DIRECTORY)
		return (qnx6_corrupt(error, "QNX6 root inode is not a directory"));

	strcpy(reader->info.format, "qnx6");
	strcpy(reader->info.profile, "classic");
	reader->info.valid |= FSIMG_VOLUME_GEOMETRY | FSIMG_VOLUME_UUID;
	bcopy(qnx6->header.uuid, reader->info.uuid, sizeof(reader->info.uuid));
	reader->info.volume_bytes = qnx6->geometry.volume_bytes;
	reader->info.sector_bytes = 512;
	reader->info.allocation_unit_bytes = qnx6->geometry.block_size;
	reader->info.allocation_units = qnx6->geometry.blocks;
	reader->info.time_basis = FSIMG_TIME_UTC;
	reader->info.scope = FSIMG_VALIDATION_HEADER;
	/* A committed snapshot is consistent; the mounted bit only records
	 * that a read-write mount was active when it was committed.
	 */
	reader->info.clean = (qnx6->header.flags & QNX6_FLAG_MOUNTED) == 0;
	reader->info.io_error = false;
	return (FSIMG_OK);
}

static void
qnx6_close(fsimg_reader_t *reader)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);

	if (qnx6 == NULL)
		return;
	fsimg_extent_index_free(qnx6->bitmap);
	fsimg_extent_index_free(qnx6->inodes);
	fsimg_extent_index_free(qnx6->names);
	fsimg_extent_index_free(qnx6->file);
	fsimg_bitset_free(qnx6->claimed);
	fsimg_bitset_free(qnx6->exposed);
	fsimg_stream_free(qnx6->record);
	fsimg_stream_free(qnx6->owners);
	fsimg_stream_free(qnx6->references);
	fsimg_dealloc(qnx6);
	reader->driver = NULL;
}

/* Replay indexed an inode's first name; a later name is indexed by its
 * first read, under the allocation check alone, since the first name
 * claimed the same tree. Either way a read walks the tree at most once per
 * exposed name.
 */
static fsimg_status_t
qnx6_resolve(fsimg_reader_t *reader, uint64_t offset, fsimg_extent_t *extent,
    fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	fsimg_status_t status;

	if (qnx6->file == NULL && qnx6->later_name) {
		status = qnx6_index_current(reader, QNX6_WALK_ALLOCATED, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (fsimg_extent_index_lookup(qnx6->file, offset, extent, error));
}
