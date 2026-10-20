/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "qnx4.h"

static fsimg_status_t qnx4_open(fsimg_reader_t *,
    const fsimg_format_variant_t *, fsimg_error_t *);
static void qnx4_close(fsimg_reader_t *);
static fsimg_status_t qnx4_resolve(fsimg_reader_t *, uint64_t,
    fsimg_extent_t *, fsimg_error_t *);

/* Full verification is not provided, so the descriptor advertises no
 * verify capability and a verification request is unsupported.
 */
const fsimg_reader_ops_t fsimg_qnx4_reader_ops = {
	.open = qnx4_open,
	.close = qnx4_close,
	.prepare = qnx4_prepare,
	.replay = qnx4_replay,
	.keys = qnx4_keys,
	.key = qnx4_key,
	.ambiguous = "duplicate QNX4 directory name",
	.resolve = qnx4_resolve,
	.verify = NULL
};

fsimg_qnx4_reader_t *
qnx4_state(const fsimg_reader_t *reader)
{
	return (reader->driver);
}

fsimg_status_t
qnx4_corrupt(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_READER,
	    message));
}

fsimg_status_t
qnx4_unsupported(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_READER,
	    message));
}

/* The volume offset of 1-based block b, which the caller keeps in [1, N]. */
uint64_t
qnx4_block_offset(uint32_t block)
{
	return (((uint64_t)block - 1) * QNX4_BLOCK_BYTES);
}

static fsimg_status_t
qnx4_open(fsimg_reader_t *reader,
    const fsimg_format_variant_t *forced FSIMG_UNUSED, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4;
	fsimg_error_t detail;
	fsimg_status_t status;

	qnx4 = fsimg_alloc(reader->ctx, sizeof(*qnx4), &detail);
	if (qnx4 == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(qnx4, sizeof(*qnx4));
	reader->driver = qnx4;
	status = qnx4_volume_open(reader, error);
	if (status != FSIMG_OK)
		return (status);

	strcpy(reader->info.format, "qnx4");
	strcpy(reader->info.profile, qnx4->has_names ? "long names" :
	    "48-byte names");
	reader->info.valid |= FSIMG_VOLUME_GEOMETRY;
	if (qnx4->label_valid && qnx4->label[0] != '\0') {
		reader->info.valid |= FSIMG_VOLUME_LABEL;
		bcopy(qnx4->label, reader->info.label, sizeof(qnx4->label));
	}
	reader->info.volume_bytes = qnx4->geometry.volume_bytes;
	reader->info.sector_bytes = QNX4_BLOCK_BYTES;
	reader->info.allocation_unit_bytes = QNX4_BLOCK_BYTES;
	reader->info.allocation_units = qnx4->geometry.blocks;
	reader->info.time_basis = FSIMG_TIME_UTC;
	reader->info.scope = FSIMG_VALIDATION_HEADER;
	/* The root record's clean bit is set by the formatter and on unmount
	 * and clear while the volume is mounted.
	 */
	reader->info.clean = (qnx4->root[QNX4_RECORD_ROOT].status &
	    QNX4_STATUS_CLEAN) != 0;
	reader->info.io_error = false;
	return (FSIMG_OK);
}

static void
qnx4_close(fsimg_reader_t *reader)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);

	if (qnx4 == NULL)
		return;
	qnx4_locator_free(&qnx4->inodes_locator);
	qnx4_locator_free(&qnx4->names_locator);
	fsimg_extent_index_free(qnx4->inodes);
	fsimg_extent_index_free(qnx4->names);
	fsimg_extent_index_free(qnx4->file);
	fsimg_bitset_free(qnx4->claimed);
	fsimg_bitset_free(qnx4->exposed);
	fsimg_dealloc(qnx4);
	reader->driver = NULL;
}

/* Replay indexed an object's first name; a later name of an .inodes object
 * is indexed by its first read under the allocation check alone, since the
 * first name claimed the same storage. Either way a read walks the chain at
 * most once per exposed name.
 */
static fsimg_status_t
qnx4_resolve(fsimg_reader_t *reader, uint64_t offset, fsimg_extent_t *extent,
    fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	fsimg_status_t status;

	if (qnx4->file == NULL && qnx4->later_name) {
		status = qnx4_index_current(reader, QNX4_WALK_ALLOCATED, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (fsimg_extent_index_lookup(qnx4->file, offset, extent, error));
}
