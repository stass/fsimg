/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include "ifs.h"

/*
 * Full verification. Open has already validated every record, path and
 * data range, and the child pass counted, for each boot and script
 * reference, the nodes carrying its inode word. An ImageFS has no
 * allocation to reconcile, so verification checks what open neither reads
 * nor decides, in this order:
 *
 * 1. Scope. An invalid mountpoint has no observed form to check, so the
 *    volume cannot be verified completely: FSIMG_EUNSUPPORTED before any
 *    work.
 * 2. Length. A well-formed volume is exactly image_size bytes; a longer
 *    one contradicts the image's own size (reading tolerates it), and a
 *    shorter one already failed open.
 * 3. Checksum. The 32-bit words of [0, image_size) in the image's byte
 *    order, the trailer included, sum to zero modulo 2^32: one sequential
 *    pass in windows.
 * 4. References. Each nonzero boot_ino[] and script_ino matches exactly
 *    one node, omitted nodes included and shadowed records never, and
 *    that node is a regular file.
 * 5. Walk. The common walker replays every directory once, omitted ones
 *    included, so every node's record is decoded a second time and every
 *    content index is built; payload read-through reads each file's and
 *    link's bytes through common delivery.
 *
 * Shadowed records have no entry and nothing to walk; open validated them
 * like any other and info reports their count. Nothing is skipped, so the
 * report is complete whenever verification succeeds.
 */

/* The checksum window and the payload buffer: whole 32-bit words. */
#define VERIFY_BUFFER_BYTES	65536u
_Static_assert(VERIFY_BUFFER_BYTES % 4 == 0,
    "checksum windows hold whole words");

static fsimg_status_t verify_checksum(fsimg_reader_t *, unsigned char *,
    fsimg_error_t *);
static fsimg_status_t verify_references(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t read_payload(fsimg_reader_t *, unsigned char *,
    fsimg_verify_report_t *, fsimg_error_t *);
static fsimg_status_t traverse(fsimg_reader_t *, unsigned char *,
    fsimg_verify_report_t *, fsimg_error_t *);

/* Sum [0, image_size) in windows of whole words, charging one work unit
 * per window. image_size is a multiple of 4, which the header bounds
 * checked, so every window ends on a word; the sum wraps modulo 2^32.
 */
static fsimg_status_t
verify_checksum(fsimg_reader_t *reader, unsigned char *buffer,
    fsimg_error_t *error)
{
	const ifs_header_t *header = &ifs_state(reader)->header;
	uint64_t offset;
	uint32_t sum;
	size_t batch;
	fsimg_status_t status;

	sum = 0;
	for (offset = 0; offset < header->image_size; offset += batch) {
		batch = header->image_size - offset > VERIFY_BUFFER_BYTES ?
		    VERIFY_BUFFER_BYTES : (size_t)(header->image_size - offset);
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = ifs_read_exact(reader, offset, buffer, batch,
			    error);
		if (status != FSIMG_OK)
			return (status);
		for (size_t i = 0; i < batch; i += 4)
			sum += ifs_u32(buffer + i, header->order);
	}
	if (sum != 0)
		return (ifs_corrupt(error, "ImageFS checksum does not match"));
	return (FSIMG_OK);
}

/* Each nonzero reference must name exactly one node, a regular file. The
 * child pass counted the matches; a reference whose only match is a
 * shadowed record has none.
 */
static fsimg_status_t
verify_references(fsimg_reader_t *reader, fsimg_error_t *error)
{
	static const char *const unmatched[IFS_REFERENCES] = {
		"ImageFS boot_ino[0] names no entry or several",
		"ImageFS boot_ino[1] names no entry or several",
		"ImageFS boot_ino[2] names no entry or several",
		"ImageFS boot_ino[3] names no entry or several",
		"ImageFS script_ino names no entry or several"
	};
	static const char *const non_file[IFS_REFERENCES] = {
		"ImageFS boot_ino[0] names a non-file",
		"ImageFS boot_ino[1] names a non-file",
		"ImageFS boot_ino[2] names a non-file",
		"ImageFS boot_ino[3] names a non-file",
		"ImageFS script_ino names a non-file"
	};
	fsimg_ifs_reader_t *ifs = ifs_state(reader);

	for (unsigned i = 0; i < IFS_REFERENCES; i++) {
		if (ifs_reference(&ifs->header, i) == 0)
			continue;
		if (ifs->counts.reference_matches[i] != 1)
			return (ifs_corrupt(error, unmatched[i]));
		if (ifs->counts.reference_kind[i] != IFS_KIND_FILE)
			return (ifs_unsupported(error, non_file[i]));
	}
	return (FSIMG_OK);
}

/* Read the current file's or link's bytes through common delivery. */
static fsimg_status_t
read_payload(fsimg_reader_t *reader, unsigned char *buffer,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	uint64_t offset;
	size_t batch, count;
	fsimg_status_t status;

	for (offset = 0; offset < reader->initialized; offset += batch) {
		batch = reader->initialized - offset > VERIFY_BUFFER_BYTES ?
		    VERIFY_BUFFER_BYTES : (size_t)(reader->initialized - offset);
		status = fsimg_reader_content(reader, offset, buffer, batch,
		    &count, error);
		report->payload_bytes_read += count;
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* Walk every node once, omitted ones included, accounting the delivered
 * ones; the walker never yields the root, so it is counted first. payload,
 * when not NULL, receives the initialized bytes of every entry but a
 * directory: a file's data and a link's target, none for a FIFO.
 */
static fsimg_status_t
traverse(fsimg_reader_t *reader, unsigned char *payload,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_status_t status;

	report->directories = 1;
	for (;;) {
		status = fsimg_walk_next(reader, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status == FSIMG_OK)
			status = fsimg_verify_account(reader, report, error);
		if (status != FSIMG_OK)
			return (status);
		if (payload == NULL ||
		    reader->entry.kind == FSIMG_ENTRY_DIRECTORY)
			continue;
		status = read_payload(reader, payload, report, error);
		if (status != FSIMG_OK)
			return (status);
	}
}

fsimg_status_t
ifs_verify(fsimg_reader_t *reader, unsigned flags,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	unsigned char *buffer;
	fsimg_error_t detail;
	fsimg_status_t status;

	reader->session.progress.phase = "ImageFS verification";
	if (!ifs->mountpoint_valid)
		return (ifs_unsupported(error,
		    "ImageFS mountpoint is invalid; verification is incomplete"));
	if (ifs->volume_bytes != ifs->header.image_size)
		return (ifs_corrupt(error,
		    "ImageFS volume is longer than its image size"));

	buffer = fsimg_alloc(reader->ctx, VERIFY_BUFFER_BYTES, &detail);
	if (buffer == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = verify_checksum(reader, buffer, error);
	if (status == FSIMG_OK)
		status = verify_references(reader, error);
	if (status == FSIMG_OK)
		status = traverse(reader, (flags & FSIMG_VERIFY_PAYLOAD) != 0 ?
		    buffer : NULL, report, error);
	if (status == FSIMG_OK)
		status = fsimg_reader_stable(reader, error);
	fsimg_dealloc(buffer);
	if (status != FSIMG_OK)
		return (status);

	/* Every byte of the image is an allocation unit in use. */
	report->allocated_clusters = ifs->header.image_size;
	reader->info.free_bytes = 0;
	reader->info.valid |= FSIMG_VOLUME_FREE;
	return (FSIMG_OK);
}
