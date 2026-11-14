/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <strings.h>

#include <zlib.h>

#include "apfs.h"
#include "codec-memory.h"
#include "codec.h"

/*
 * Transparent compression (specs/design-apfs-driver.md, "Compression
 * framing" and "Content"; specs/design-apfs.md, "Transparent
 * compression"): which types APFS admits, how their stored bytes are
 * reached, the decode window and the dispatch to the decoders. The shared
 * decmpfs module owns the rest: each type's facts and its raw marker's
 * rule, and the framing of the header and the chunk tables, which it reads
 * from the resource fork through apfs_stream_read().
 *
 * replay validated the header and selected the stream holding the stored
 * bytes (inode.c). The first resolve opens the content: an inline payload,
 * from its attribute record or after the header in its stream, is decoded
 * whole; a resource fork's table is checked by the shared validator's
 * open, and each chunk waits until it is asked for, when the validator's
 * chunk checks its entry against its neighbours. A compressed file's bytes
 * reach delivery as held extents, one decoded chunk at a time, so memory
 * does not grow with the file, and a stream that inflates without end
 * stops at the chunk's length.
 */
#define ROLE_COMPRESSED	"compressed file"

_Static_assert(FSIMG_DECMPFS_CHUNK_BYTES <= FSIMG_LZFSE_OUTPUT_MAX &&
    FSIMG_DECMPFS_STORED_MAX <= FSIMG_LZFSE_INPUT_MAX,
    "every chunk is within the imported decoders' bounds");

/*
 * One charged allocation per reader, made by the first compressed file
 * resolved and released at close. input holds the stored bytes being
 * decoded and output the decoded chunk, which current.compression says is
 * valid while held is set. inflating says stream needs inflateEnd; memory
 * is its allocation adapter, which charges zlib's state to the same context
 * and stays beside it for its whole life. lzfse_scratch is the LZFSE
 * decoder's state, charged and allocated for the first LZFSE file.
 */
struct apfs_window {
	fsimg_codec_memory_t	 memory;
	z_stream		 stream;
	void			*lzfse_scratch;
	bool			 inflating;
	unsigned char		 input[FSIMG_DECMPFS_STORED_MAX];
	unsigned char		 output[FSIMG_DECMPFS_CHUNK_BYTES];
};

static bool		admitted(uint32_t, fsimg_decmpfs_info_t *);
static fsimg_status_t	chunk_load(fsimg_reader_t *, uint64_t,
			    fsimg_error_t *);
static fsimg_status_t	content_open(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t	decode_lzfse(fsimg_reader_t *, apfs_window_t *,
			    size_t, size_t, fsimg_error_t *);
static fsimg_status_t	decode_lzvn(apfs_window_t *, size_t, size_t,
			    fsimg_error_t *);
static fsimg_status_t	decode_zlib(fsimg_reader_t *, apfs_window_t *,
			    size_t, size_t, fsimg_error_t *);
static fsimg_status_t	payload_load(fsimg_reader_t *, size_t *,
			    fsimg_error_t *);
static fsimg_status_t	payload_from_record(fsimg_reader_t *, size_t *,
			    fsimg_error_t *);
static fsimg_status_t	stream_bytes(void *, uint64_t, void *, size_t,
			    fsimg_error_t *);
static fsimg_status_t	window_decode(fsimg_reader_t *,
			    const fsimg_decmpfs_info_t *, size_t, size_t,
			    fsimg_error_t *);
static fsimg_status_t	window_ready(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t	zlib_failed(const apfs_window_t *, int,
			    fsimg_error_t *);

/*
 * The types this reader decodes, with their facts: 3, 4 and 7 to 12. Type
 * 1 has no APFS specimen, and LZBITMAP's 13 and 14 have no public decoder.
 * A type not listed is unsupported, so a row added to the shared table
 * changes nothing here.
 */
static bool
admitted(uint32_t type, fsimg_decmpfs_info_t *info)
{
	switch (type) {
	case FSIMG_DECMPFS_TYPE_ZLIB_INLINE:
	case FSIMG_DECMPFS_TYPE_ZLIB_FORK:
	case FSIMG_DECMPFS_TYPE_LZVN_INLINE:
	case FSIMG_DECMPFS_TYPE_LZVN_FORK:
	case FSIMG_DECMPFS_TYPE_COPY_INLINE:
	case FSIMG_DECMPFS_TYPE_COPY_FORK:
	case FSIMG_DECMPFS_TYPE_LZFSE_INLINE:
	case FSIMG_DECMPFS_TYPE_LZFSE_FORK:
		return (fsimg_decmpfs_type_info(type, info));
	default:
		return (false);
	}
}

/*
 * The capability record's codecs, in type order. The imported LZVN and
 * LZFSE decoders refuse on a big-endian host, under the condition
 * lib/codec/lzfse.c tests, so there zlib is listed alone.
 */
void
apfs_codecs(char *out, size_t capacity)
{
	char version[48];

	fsimg_zlib_version(version, sizeof(version));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	(void)snprintf(out, capacity, "deflate, %s; lzvn; lzfse", version);
#else
	(void)snprintf(out, capacity, "deflate, %s", version);
#endif
}

void
apfs_compress_free(apfs_reader_t *apfs)
{
	apfs_window_t *window = apfs->window;

	if (window == NULL)
		return;
	if (window->inflating)
		inflateEnd(&window->stream);
	if (window->lzfse_scratch != NULL)
		fsimg_dealloc(window->lzfse_scratch);
	fsimg_dealloc(window);
	apfs->window = NULL;
}

static fsimg_status_t
window_ready(fsimg_reader_t *reader, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	apfs_window_t *window;
	fsimg_error_t detail;

	if (apfs->window != NULL)
		return (FSIMG_OK);
	window = fsimg_alloc(reader->ctx, sizeof(*window), &detail);
	if (window == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(window, sizeof(*window));
	apfs->window = window;
	return (FSIMG_OK);
}

/*
 * A zlib result that says nothing about the chunk: an allocation failure
 * as the adapter recorded it, or a fault of fsimg or of the zlib library.
 */
static fsimg_status_t
zlib_failed(const apfs_window_t *window, int code, fsimg_error_t *error)
{
	if (code == Z_MEM_ERROR)
		return (fsimg_codec_memory_error(&window->memory, error));
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_READER,
	    "zlib refused a call on the APFS decode stream"));
}

/*
 * Decode input's stored bytes into exactly expected bytes of output.
 * Inflate is given no more output than expected, so a stream wanting more
 * stops there, and Z_FINISH makes that an error, as it makes a stream that
 * needs more input one. Only a stream that ends having consumed all its
 * bytes and filled the output is the chunk: anything else is the image's
 * fault.
 */
static fsimg_status_t
decode_zlib(fsimg_reader_t *reader, apfs_window_t *window, size_t stored,
    size_t expected, fsimg_error_t *error)
{
	z_stream *stream = &window->stream;
	int code;

	if (!window->inflating) {
		fsimg_zlib_memory_bind(&window->memory, reader->ctx, stream);
		code = inflateInit(stream);
		if (code != Z_OK)
			return (zlib_failed(window, code, error));
		window->inflating = true;
	}
	code = inflateReset(stream);
	if (code != Z_OK)
		return (zlib_failed(window, code, error));
	stream->next_in = window->input;
	stream->avail_in = (uInt)stored;
	stream->next_out = window->output;
	stream->avail_out = (uInt)expected;
	code = inflate(stream, Z_FINISH);
	if (code == Z_MEM_ERROR || code == Z_STREAM_ERROR)
		return (zlib_failed(window, code, error));
	if (code != Z_STREAM_END || stream->avail_in != 0 ||
	    stream->avail_out != 0)
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "zlib chunk does not decode to its length"));
	return (FSIMG_OK);
}

/*
 * The imported decoder, bounded by exactly expected bytes of output; a
 * stream ending short of them is the image's fault too.
 */
static fsimg_status_t
decode_lzvn(apfs_window_t *window, size_t stored, size_t expected,
    fsimg_error_t *error)
{
	fsimg_codec_step_t step;
	fsimg_status_t status;

	status = fsimg_lzvn_decode(window->input, stored, window->output,
	    expected, &step, error);
	if (status != FSIMG_OK)
		return (status);
	if (step.produced != expected)
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "LZVN chunk does not decode to its length"));
	return (FSIMG_OK);
}

/*
 * The imported decoder, bounded the same way, with its state charged on
 * the first use.
 */
static fsimg_status_t
decode_lzfse(fsimg_reader_t *reader, apfs_window_t *window, size_t stored,
    size_t expected, fsimg_error_t *error)
{
	fsimg_codec_step_t step;
	fsimg_status_t status;
	fsimg_error_t detail;

	if (window->lzfse_scratch == NULL) {
		window->lzfse_scratch = fsimg_alloc(reader->ctx,
		    fsimg_lzfse_scratch_bytes(), &detail);
		if (window->lzfse_scratch == NULL) {
			if (error != NULL)
				*error = detail;
			return (detail.status);
		}
	}
	status = fsimg_lzfse_decode(window->input, stored, window->output,
	    expected, window->lzfse_scratch, &step, error);
	if (status != FSIMG_OK)
		return (status);
	if (step.produced != expected)
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "LZFSE chunk does not decode to its length"));
	return (FSIMG_OK);
}

/*
 * Decode stored bytes of input to exactly expected bytes of output, or
 * fail. The work is charged first, which also polls for cancellation. The
 * shared raw-marker rule comes before the codec: it finds content stored
 * unencoded, which is copied, or a codec's stream, which goes to its
 * decoder. The output is not valid until the caller says so.
 */
static fsimg_status_t
window_decode(fsimg_reader_t *reader, const fsimg_decmpfs_info_t *info,
    size_t stored, size_t expected, fsimg_error_t *error)
{
	apfs_window_t *window = ((apfs_reader_t *)reader->driver)->window;
	const unsigned char *content;
	fsimg_status_t status;

	status = fsimg_session_charge(&reader->session, 0, 0,
	    (uint64_t)stored + expected, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_decmpfs_unencoded(info, window->input, stored, expected,
	    &content, error);
	if (status != FSIMG_OK)
		return (status);
	if (content != NULL) {
		bcopy(content, window->output, expected);
		return (FSIMG_OK);
	}
	switch (info->codec) {
	case FSIMG_DECMPFS_CODEC_ZLIB:
		return (decode_zlib(reader, window, stored, expected, error));
	case FSIMG_DECMPFS_CODEC_LZVN:
		return (decode_lzvn(window, stored, expected, error));
	case FSIMG_DECMPFS_CODEC_LZFSE:
		return (decode_lzfse(reader, window, stored, expected, error));
	case FSIMG_DECMPFS_CODEC_NONE:		/* always unencoded content */
	case FSIMG_DECMPFS_CODEC_LZBITMAP:	/* never admitted */
		break;
	}
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_READER,
	    "APFS compression type was not admitted"));
}

/* The framing validator's way to the bytes of the current resource fork. */
static fsimg_status_t
stream_bytes(void *context, uint64_t offset, void *buffer, size_t length,
    fsimg_error_t *error)
{
	return (apfs_stream_read(context, offset, buffer, length, error));
}

/*
 * The compression value held in its attribute record, found again by name
 * through the lookup cursor: it must still be the header replay admitted.
 * Its payload, the bytes after the header, is copied into the window's
 * input, which the record's length bounds.
 */
static fsimg_status_t
payload_from_record(fsimg_reader_t *reader, size_t *stored,
    fsimg_error_t *error)
{
	static const char name[] = APFS_XATTR_DECMPFS;
	apfs_reader_t *apfs = reader->driver;
	const apfs_compressed_t *compression = &apfs->current.compression;
	unsigned char key[APFS_XATTR_KEY_BYTES + sizeof(name)];
	const apfs_name_key_t search = { .name = name,
	    .oid = apfs->current.inode, .name_len = sizeof(name) - 1 };
	fsimg_decmpfs_header_t header;
	apfs_name_key_t found_key;
	apfs_xattr_val_t val;
	apfs_record_t record;
	bool found;
	fsimg_status_t status;

	/* replay encoded a key of the same inode number. */
	if (!apfs_xattr_key_encode(key, sizeof(key), &search))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_READER,
		    "APFS compressed file's inode number is invalid"));
	status = apfs_cursor_seek(&apfs->lookup, key, sizeof(key),
	    APFS_SEEK_GE, &found, error);
	if (status != FSIMG_OK)
		return (status);
	if (found)
		apfs_cursor_record(&apfs->lookup, &record);
	if (!found || record.ghost ||
	    !apfs_xattr_key_decode(record.key, record.key_len, &found_key) ||
	    found_key.oid != search.oid || found_key.name_len != search.name_len ||
	    bcmp(found_key.name, name, search.name_len) != 0)
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "header record is missing"));
	if (!apfs_xattr_val_decode(record.val, record.val_len, &val) ||
	    (val.flags & APFS_XATTR_DATA_EMBEDDED) == 0 ||
	    !fsimg_decmpfs_header_decode(val.data, val.data_len, &header))
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "header record is malformed"));
	if (header.type != compression->type ||
	    header.uncompressed_size != compression->size)
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "header changed since it was listed"));
	*stored = val.data_len - FSIMG_DECMPFS_HEADER_BYTES;
	if (*stored > sizeof(apfs->window->input))
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "inline payload is too long"));
	bcopy(val.data + FSIMG_DECMPFS_HEADER_BYTES, apfs->window->input,
	    *stored);
	return (FSIMG_OK);
}

/*
 * An inline type's stored payload, into the window's input: from the
 * record, or the selected stream's bytes after the header, which replay
 * read and attr_classify() bounded to the input's length.
 */
static fsimg_status_t
payload_load(fsimg_reader_t *reader, size_t *stored, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	uint64_t length = apfs->current.stream.size;

	if (!apfs->current.compression.streamed)
		return (payload_from_record(reader, stored, error));
	if (length < FSIMG_DECMPFS_HEADER_BYTES ||
	    length - FSIMG_DECMPFS_HEADER_BYTES > sizeof(apfs->window->input))
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "header has an invalid length"));
	*stored = (size_t)(length - FSIMG_DECMPFS_HEADER_BYTES);
	return (apfs_stream_read(reader, FSIMG_DECMPFS_HEADER_BYTES,
	    apfs->window->input, *stored, error));
}

/*
 * Open the current file's compressed content, on its first resolve: refuse
 * a type this reader does not decode, then decode an inline payload whole,
 * or check a resource fork's table.
 */
static fsimg_status_t
content_open(fsimg_reader_t *reader, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	apfs_compressed_t *compression = &apfs->current.compression;
	fsimg_decmpfs_info_t info;
	size_t stored;
	fsimg_status_t status;

	if (!admitted(compression->type, &info))
		return (apfs_unsupported(error, ROLE_COMPRESSED,
		    "compression type is not supported"));
	status = window_ready(reader, error);
	if (status != FSIMG_OK)
		return (status);
	switch (info.kind) {
	case FSIMG_DECMPFS_INLINE:
		status = payload_load(reader, &stored, error);
		if (status == FSIMG_OK)
			status = window_decode(reader, &info, stored,
			    (size_t)compression->size, error);
		if (status != FSIMG_OK)
			return (status);
		compression->chunk = 0;
		compression->length = (size_t)compression->size;
		compression->held = true;
		break;
	case FSIMG_DECMPFS_TABLE_RESOURCE:
	case FSIMG_DECMPFS_TABLE_WORDS:
		status = fsimg_decmpfs_open(info.kind, apfs->current.stream.size,
		    compression->size, stream_bytes, reader, &compression->table,
		    error);
		if (status != FSIMG_OK)
			return (status);
		break;
	}
	compression->opened = true;
	return (FSIMG_OK);
}

/*
 * Decode chunk `chunk` of a resource-fork type into the window: its entry
 * checked by the shared validator, its stored bytes read through the fork's
 * extents, then decoded to the chunk's length, the size clipping the last.
 */
static fsimg_status_t
chunk_load(fsimg_reader_t *reader, uint64_t chunk, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	apfs_compressed_t *compression = &apfs->current.compression;
	fsimg_decmpfs_info_t info;
	uint64_t logical, offset;
	size_t expected, stored;
	fsimg_status_t status;

	/* A failed decode may have overwritten output, so it is not held. */
	compression->held = false;
	if (!admitted(compression->type, &info) ||
	    info.kind == FSIMG_DECMPFS_INLINE)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_READER,
		    "APFS compressed file has no chunk table"));
	status = fsimg_decmpfs_chunk(&compression->table, chunk, stream_bytes,
	    reader, &offset, &stored, error);
	if (status == FSIMG_OK)
		status = apfs_stream_read(reader, offset, apfs->window->input,
		    stored, error);
	if (status != FSIMG_OK)
		return (status);
	logical = chunk * FSIMG_DECMPFS_CHUNK_BYTES;
	expected = compression->size - logical < FSIMG_DECMPFS_CHUNK_BYTES ?
	    (size_t)(compression->size - logical) : FSIMG_DECMPFS_CHUNK_BYTES;
	status = window_decode(reader, &info, stored, expected, error);
	if (status != FSIMG_OK)
		return (status);
	compression->chunk = chunk;
	compression->length = expected;
	compression->held = true;
	return (FSIMG_OK);
}

/*
 * The decoded chunk holding offset, decoded unless the window holds it, so
 * a sequential read decodes each chunk once. An inline file never reaches
 * chunk_load(): its open left chunk 0 held, and every offset is in chunk
 * 0, since its size is at most one chunk.
 */
fsimg_status_t
apfs_compress_resolve(fsimg_reader_t *reader, uint64_t offset,
    fsimg_extent_t *extent, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	apfs_compressed_t *compression = &apfs->current.compression;
	uint64_t chunk = offset / FSIMG_DECMPFS_CHUNK_BYTES;
	fsimg_status_t status;

	if (!compression->opened) {
		status = content_open(reader, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (!compression->held || compression->chunk != chunk) {
		status = chunk_load(reader, chunk, error);
		if (status != FSIMG_OK)
			return (status);
	}
	extent->logical_offset = chunk * FSIMG_DECMPFS_CHUNK_BYTES;
	extent->length = compression->length;
	reader->held = apfs->window->output;
	return (FSIMG_OK);
}

/*
 * The table is checked for every type the shared module knows, LZBITMAP's
 * included, since checking it decodes nothing. It is opened into a table
 * of its own, so a later resolve still opens the content and refuses a
 * type not admitted. A file of size zero is checked too: its table must
 * hold no entry.
 */
fsimg_status_t
apfs_compress_table_check(fsimg_reader_t *reader, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	const apfs_compressed_t *compression = &apfs->current.compression;
	fsimg_decmpfs_table_t table;
	fsimg_decmpfs_info_t info;
	uint64_t offset;
	size_t stored;
	fsimg_status_t status;

	if (!apfs->current.compressed ||
	    !fsimg_decmpfs_type_info(compression->type, &info) ||
	    info.kind == FSIMG_DECMPFS_INLINE)
		return (FSIMG_OK);
	status = fsimg_decmpfs_open(info.kind, apfs->current.stream.size,
	    compression->size, stream_bytes, reader, &table, error);
	for (uint64_t chunk = 0; status == FSIMG_OK && chunk < table.chunks;
	    chunk++) {
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = fsimg_decmpfs_chunk(&table, chunk,
			    stream_bytes, reader, &offset, &stored, error);
	}
	return (status);
}
