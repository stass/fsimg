/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "io.h"
#include "mtree-writer.h"

#define MTREE_WRITER_BUFFER_BYTES 65536u
/* The longest plain keyword text, every field at its widest: " type=fifo"
 * (10), " mode=7777" (10), " uid=4294967295 gid=4294967295" (30),
 * " time=-9223372036854775808.4294967295" (37, the nanoseconds field as
 * wide as its uint32_t can print), " size=18446744073709551615
 * sha256digest=" (40) and 64 hexadecimal digits.
 */
#define MTREE_KEYWORDS_MAX (10u + 10u + 30u + 37u + 40u + 64u)
#define MTREE_KEYWORDS_BYTES 256u
_Static_assert(MTREE_KEYWORDS_MAX < MTREE_KEYWORDS_BYTES,
    "the keyword buffer holds the longest keywords and a NUL");

/* Bytes [0, used) of buffer follow the written_bytes already in the file.
 * The file is grown to its new length just before each write, so it never
 * holds a byte that was not written.
 */
struct fsimg_mtree_writer {
	fsimg_session_t	*session;
	fsimg_io_t	*output;
	unsigned char	*buffer;
	uint64_t	 written_bytes;
	size_t		 used;
};

static fsimg_status_t write_buffer(fsimg_mtree_writer_t *, fsimg_error_t *);
static fsimg_status_t put_bytes(fsimg_mtree_writer_t *, const void *, size_t,
    fsimg_error_t *);
static fsimg_status_t put_text(fsimg_mtree_writer_t *, const char *,
    fsimg_error_t *);
static fsimg_status_t put_encoded(fsimg_mtree_writer_t *,
    const unsigned char *, size_t, fsimg_error_t *);
static void format_keywords(const fsimg_mtree_record_t *,
    char [static MTREE_KEYWORDS_BYTES]);
static const char *kind_name(fsimg_entry_kind_t);

fsimg_status_t
fsimg_mtree_writer_check(int fd, fsimg_error_t *error)
{
	struct stat st;
	int flags;

	flags = fcntl(fd, F_GETFL);
	if (flags == -1)
		return (fsimg_error_set(error, FSIMG_EINVAL, errno,
		    FSIMG_OP_EXTRACT, "manifest descriptor is not open"));
	if ((flags & O_ACCMODE) == O_RDONLY || (flags & O_APPEND) != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_EXTRACT,
		    "manifest descriptor is not writable without O_APPEND"));
	if (fstat(fd, &st) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_EXTRACT, "manifest descriptor stat failed"));
	if (!S_ISREG(st.st_mode) || st.st_size != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_EXTRACT, "manifest is not an empty regular file"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_mtree_writer_open(fsimg_ctx_t *ctx, fsimg_session_t *session, int fd,
    fsimg_mtree_writer_t **out, fsimg_error_t *error)
{
	fsimg_io_t *output;
	fsimg_status_t status;

	*out = NULL;
	status = fsimg_mtree_writer_check(fd, error);
	if (status == FSIMG_OK)
		status = fsimg_io_borrow_fd(ctx, fd, FSIMG_IO_WRITE, &output, error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_mtree_writer_start(ctx, session, output, out, error));
}

fsimg_status_t
fsimg_mtree_writer_start(fsimg_ctx_t *ctx, fsimg_session_t *session,
    fsimg_io_t *output, fsimg_mtree_writer_t **out, fsimg_error_t *error)
{
	fsimg_mtree_writer_t *writer;
	fsimg_error_t detail;
	fsimg_status_t status;

	*out = NULL;
	writer = fsimg_alloc(ctx, sizeof(*writer), &detail);
	if (writer == NULL) {
		fsimg_io_free(output);
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(writer, sizeof(*writer));
	writer->session = session;
	writer->output = output;
	writer->buffer = fsimg_alloc(ctx, MTREE_WRITER_BUFFER_BYTES, &detail);
	if (writer->buffer == NULL) {
		fsimg_mtree_writer_free(writer);
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = put_text(writer, "#mtree v2.0\n. type=dir\n", error);
	if (status != FSIMG_OK) {
		fsimg_mtree_writer_free(writer);
		return (status);
	}
	*out = writer;
	return (FSIMG_OK);
}

/* A line is the encoded path, the plain keywords, an encoded link target
 * for a symbolic link, and LF.
 */
fsimg_status_t
fsimg_mtree_writer_add(fsimg_mtree_writer_t *writer,
    const fsimg_mtree_record_t *record, fsimg_error_t *error)
{
	char keywords[MTREE_KEYWORDS_BYTES];
	const char *path = record->entry->path;
	fsimg_status_t status;

	/* The path is absolute; "." before it makes the full form "./...". */
	status = put_text(writer, ".", error);
	if (status != FSIMG_OK)
		return (status);
	status = put_encoded(writer, (const unsigned char *)path, strlen(path),
	    error);
	if (status != FSIMG_OK)
		return (status);

	format_keywords(record, keywords);
	status = put_text(writer, keywords, error);
	if (status != FSIMG_OK)
		return (status);

	if (record->link != NULL) {
		status = put_text(writer, " link=", error);
		if (status != FSIMG_OK)
			return (status);
		status = put_encoded(writer, record->link, record->link_length,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (put_text(writer, "\n", error));
}

fsimg_status_t
fsimg_mtree_writer_finish(fsimg_mtree_writer_t *writer, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = write_buffer(writer, error);
	if (status == FSIMG_OK)
		status = fsimg_io_flush(writer->output, true, error);
	fsimg_mtree_writer_free(writer);
	return (status);
}

void
fsimg_mtree_writer_free(fsimg_mtree_writer_t *writer)
{
	if (writer == NULL)
		return;
	fsimg_io_free(writer->output);
	fsimg_dealloc(writer->buffer);
	fsimg_dealloc(writer);
}

static fsimg_status_t
write_buffer(fsimg_mtree_writer_t *writer, fsimg_error_t *error)
{
	uint64_t length;
	size_t written;
	fsimg_status_t status;

	if (writer->used == 0)
		return (FSIMG_OK);
	if (!fsimg_add_u64(writer->written_bytes, writer->used, &length))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_EXTRACT, "manifest length overflows"));
	status = fsimg_io_resize(writer->output, length, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_io_write_session(writer->output, writer->session,
	    writer->written_bytes, writer->buffer, writer->used, &written, error);
	if (status != FSIMG_OK)
		return (status);
	writer->written_bytes = length;
	writer->used = 0;
	return (FSIMG_OK);
}

static fsimg_status_t
put_bytes(fsimg_mtree_writer_t *writer, const void *bytes, size_t length,
    fsimg_error_t *error)
{
	const unsigned char *next = bytes;
	size_t piece;
	fsimg_status_t status;

	while (length != 0) {
		if (writer->used == MTREE_WRITER_BUFFER_BYTES) {
			status = write_buffer(writer, error);
			if (status != FSIMG_OK)
				return (status);
		}
		piece = MTREE_WRITER_BUFFER_BYTES - writer->used;
		if (piece > length)
			piece = length;
		bcopy(next, writer->buffer + writer->used, piece);
		writer->used += piece;
		next += piece;
		length -= piece;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
put_text(fsimg_mtree_writer_t *writer, const char *text, fsimg_error_t *error)
{
	return (put_bytes(writer, text, strlen(text), error));
}

/* The keywords that need no escape, each with its leading space, in one
 * fixed order: type, mode, uid and gid, time, then size and sha256digest
 * for a copied file. A field the entry does not carry is left out, so the
 * manifest's reader sees it absent, as the image reported it. The text
 * is at most MTREE_KEYWORDS_MAX bytes.
 */
static void
format_keywords(const fsimg_mtree_record_t *record,
    char text[static MTREE_KEYWORDS_BYTES])
{
	static const char hex[] = "0123456789abcdef";
	const fsimg_entry_t *entry = record->entry;
	size_t length;

	length = (size_t)snprintf(text, MTREE_KEYWORDS_BYTES, " type=%s",
	    kind_name(entry->kind));
	if ((entry->metadata.valid & FSIMG_META_MODE) != 0)
		length += (size_t)snprintf(text + length,
		    MTREE_KEYWORDS_BYTES - length, " mode=%04" PRIo32,
		    entry->metadata.mode & 07777);
	if ((entry->metadata.valid & FSIMG_META_OWNER) != 0)
		length += (size_t)snprintf(text + length,
		    MTREE_KEYWORDS_BYTES - length, " uid=%" PRIu32 " gid=%" PRIu32,
		    entry->metadata.uid, entry->metadata.gid);
	if (record->modified_valid)
		length += (size_t)snprintf(text + length,
		    MTREE_KEYWORDS_BYTES - length, " time=%" PRId64 ".%09" PRIu32,
		    record->modified_seconds, record->modified_nanoseconds);
	if (record->digest == NULL)
		return;
	length += (size_t)snprintf(text + length, MTREE_KEYWORDS_BYTES - length,
	    " size=%" PRIu64 " sha256digest=", entry->size);
	for (size_t i = 0; i < FSIMG_SHA256_DIGEST_LENGTH; i++) {
		text[length++] = hex[record->digest[i] >> 4];
		text[length++] = hex[record->digest[i] & 0x0f];
	}
	text[length] = '\0';
}

/* vis(3) octal encoding, the one the manifest parser decodes: a byte that
 * is not plain printable ASCII, or that is '\' or '#', becomes a backslash
 * and three octal digits, so "a b" is written "a\040b".
 */
static fsimg_status_t
put_encoded(fsimg_mtree_writer_t *writer, const unsigned char *bytes,
    size_t length, fsimg_error_t *error)
{
	char escape[5];
	fsimg_status_t status;

	for (size_t i = 0; i < length; i++) {
		if (bytes[i] >= 0x21 && bytes[i] <= 0x7e && bytes[i] != '\\' &&
		    bytes[i] != '#') {
			status = put_bytes(writer, &bytes[i], 1, error);
		} else {
			snprintf(escape, sizeof(escape), "\\%03o", bytes[i]);
			status = put_bytes(writer, escape, 4, error);
		}
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

static const char *
kind_name(fsimg_entry_kind_t kind)
{
	switch (kind) {
	case FSIMG_ENTRY_DIRECTORY:
		return ("dir");
	case FSIMG_ENTRY_SYMLINK:
		return ("link");
	case FSIMG_ENTRY_FIFO:
		return ("fifo");
	case FSIMG_ENTRY_FILE:
		break;
	}
	return ("file");
}
