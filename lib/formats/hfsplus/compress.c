/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include <zlib.h>

#include "codec-memory.h"
#include "codec.h"
#include "hfsplus.h"

/*
 * Transparent compression: which types HFS Plus admits, the decode window and
 * the dispatch to the decoders. The shared decmpfs module owns the rest: each
 * type's facts and its raw marker's rule, and the framing of the header and
 * the chunk tables, which it reads from the resource fork through its index
 * here. A compressed file's bytes reach delivery as held extents, one
 * decoded chunk at a time, so its memory does not grow with the file and a
 * stream that inflates without end stops at the window.
 */

_Static_assert(FSIMG_DECMPFS_CHUNK_BYTES <= FSIMG_LZFSE_OUTPUT_MAX &&
    FSIMG_DECMPFS_STORED_MAX <= FSIMG_LZFSE_INPUT_MAX,
    "every chunk is within the imported decoders' bounds");

/* One charged allocation per reader, made by the first compressed file resolved
 * and released at close. output holds chunk `chunk`'s length decoded bytes while
 * valid; input holds the bytes being decoded. inflating says stream needs
 * inflateEnd; memory is its allocation adapter, which charges zlib's state to
 * the same context and stays beside it for its whole life. lzfse_scratch is the
 * LZFSE decoder's state, charged and allocated for the first LZFSE file.
 */
struct hfsplus_window {
	fsimg_codec_memory_t	 memory;
	z_stream		 stream;
	void			*lzfse_scratch;
	uint64_t		 chunk;
	size_t			 length;
	bool			 inflating;
	bool			 valid;
	unsigned char		 input[FSIMG_DECMPFS_STORED_MAX];
	unsigned char		 output[FSIMG_DECMPFS_CHUNK_BYTES];
};

static bool admitted(uint32_t, fsimg_decmpfs_info_t *);
static fsimg_status_t window_ready(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t zlib_failed(const hfsplus_window_t *, int,
    fsimg_error_t *);
static fsimg_status_t zlib_decode(fsimg_reader_t *, hfsplus_window_t *, size_t,
    size_t, fsimg_error_t *);
static fsimg_status_t chunk_decode_lzvn(hfsplus_window_t *, size_t, size_t,
    fsimg_error_t *);
static fsimg_status_t chunk_decode_lzfse(fsimg_reader_t *, hfsplus_window_t *,
    size_t, size_t, fsimg_error_t *);
static fsimg_status_t window_decode(fsimg_reader_t *, size_t, size_t,
    fsimg_error_t *);
static fsimg_status_t fork_bytes(void *, uint64_t, void *, size_t,
    fsimg_error_t *);
static fsimg_status_t table_open(fsimg_reader_t *, const hfsplus_file_t *,
    fsimg_decmpfs_kind_t, fsimg_error_t *);
static fsimg_status_t chunk_load(fsimg_reader_t *, uint64_t, fsimg_error_t *);

/* The types this reader decodes, with their facts: every type macOS writes
 * but LZBITMAP's 13 and 14, which have no public decoder. A type not listed is
 * unsupported, so a row added to the shared table changes nothing here.
 */
static bool
admitted(uint32_t type, fsimg_decmpfs_info_t *info)
{
	switch (type) {
	case FSIMG_DECMPFS_TYPE_STORED:
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

fsimg_status_t
hfsplus_compress_check(const hfsplus_file_t *file,
    const hfsplus_attr_scan_t *scan, fsimg_error_t *error)
{
	fsimg_decmpfs_info_t info;

	if (!scan->compression)
		return (hfsplus_corrupt(error,
		    "HFS Plus compressed file has no compression attribute"));
	if (file->data_fork.logical_size != 0 ||
	    file->data_fork.total_blocks != 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus compressed file has a nonempty data fork"));
	if (!admitted(scan->header.type, &info))
		return (hfsplus_unsupported(error,
		    "HFS Plus compression type is not supported"));
	if (info.kind == FSIMG_DECMPFS_INLINE &&
	    scan->header.uncompressed_size > FSIMG_DECMPFS_CHUNK_BYTES)
		return (hfsplus_unsupported(error,
		    "HFS Plus inline compressed file is larger than "
		    "the decode window"));
	return (FSIMG_OK);
}

bool
hfsplus_compress_known(uint8_t type)
{
	fsimg_decmpfs_info_t info;

	return (admitted(type, &info));
}

bool
hfsplus_compress_consumes_fork(uint8_t type)
{
	fsimg_decmpfs_info_t info;

	return (admitted(type, &info) && info.kind != FSIMG_DECMPFS_INLINE);
}

void
hfsplus_compress_reset(fsimg_hfsplus_reader_t *hfsplus)
{
	hfsplus->compressed = (hfsplus_compressed_t){ 0 };
	if (hfsplus->window != NULL)
		hfsplus->window->valid = false;
}

void
hfsplus_compress_free(fsimg_hfsplus_reader_t *hfsplus)
{
	hfsplus_window_t *window = hfsplus->window;

	if (window == NULL)
		return;
	if (window->inflating)
		inflateEnd(&window->stream);
	if (window->lzfse_scratch != NULL)
		fsimg_dealloc(window->lzfse_scratch);
	fsimg_dealloc(window);
	hfsplus->window = NULL;
}

static fsimg_status_t
window_ready(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	hfsplus_window_t *window;
	fsimg_error_t detail;

	if (hfsplus->window != NULL)
		return (FSIMG_OK);
	window = fsimg_alloc(reader->ctx, sizeof(*window), &detail);
	if (window == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(window, sizeof(*window));
	hfsplus->window = window;
	return (FSIMG_OK);
}

/* A zlib result that says nothing about the chunk: an allocation failure as the
 * adapter recorded it, or a fault of fsimg or of the zlib library.
 */
static fsimg_status_t
zlib_failed(const hfsplus_window_t *window, int code, fsimg_error_t *error)
{
	if (code == Z_MEM_ERROR)
		return (fsimg_codec_memory_error(&window->memory, error));
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_READER,
	    "zlib refused a call on the HFS Plus decode stream"));
}

/* Decode input's stored bytes into exactly expected bytes of output. Inflate is
 * given no more output than expected, so a stream wanting more stops there, and
 * Z_FINISH makes that an error, as it makes a stream that needs more input one.
 * Only a stream that ends having consumed all its bytes and filled the output is
 * the chunk: anything else is the image's fault.
 */
static fsimg_status_t
zlib_decode(fsimg_reader_t *reader, hfsplus_window_t *window, size_t stored,
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
		return (hfsplus_corrupt(error,
		    "HFS Plus zlib chunk does not decode to its length"));
	return (FSIMG_OK);
}

/* The imported decoder, bounded by exactly expected bytes of output; a stream
 * ending short of them is the image's fault too.
 */
static fsimg_status_t
chunk_decode_lzvn(hfsplus_window_t *window, size_t stored, size_t expected,
    fsimg_error_t *error)
{
	fsimg_codec_step_t step;
	fsimg_status_t status;

	status = fsimg_lzvn_decode(window->input, stored, window->output,
	    expected, &step, error);
	if (status != FSIMG_OK)
		return (status);
	if (step.produced != expected)
		return (hfsplus_corrupt(error,
		    "HFS Plus LZVN chunk does not decode to its length"));
	return (FSIMG_OK);
}

/* The imported decoder, bounded the same way, with its state charged on the first
 * use.
 */
static fsimg_status_t
chunk_decode_lzfse(fsimg_reader_t *reader, hfsplus_window_t *window,
    size_t stored, size_t expected, fsimg_error_t *error)
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
		return (hfsplus_corrupt(error,
		    "HFS Plus LZFSE chunk does not decode to its length"));
	return (FSIMG_OK);
}

/* Decode the current entry's stored bytes, in input, to output: expected bytes
 * or a failure. The work is charged first, which also polls for cancellation.
 * The shared raw-marker rule comes before the codec: it finds content stored
 * unencoded, which is copied, or a codec's stream, which goes to its decoder.
 * The output is not valid until the caller says so.
 */
static fsimg_status_t
window_decode(fsimg_reader_t *reader, size_t stored, size_t expected,
    fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	hfsplus_window_t *window = hfsplus->window;
	const unsigned char *content;
	fsimg_decmpfs_info_t info;
	fsimg_status_t status;

	status = fsimg_session_charge(&reader->session, 0, 0,
	    (uint64_t)stored + expected, error);
	if (status != FSIMG_OK)
		return (status);
	if (!admitted(hfsplus->current.compression, &info))
		return (hfsplus_corrupt(error,
		    "invalid prepared HFS Plus compression type"));
	status = fsimg_decmpfs_unencoded(&info, window->input, stored, expected,
	    &content, error);
	if (status != FSIMG_OK)
		return (status);
	if (content != NULL) {
		bcopy(content, window->output, expected);
		return (FSIMG_OK);
	}
	switch (info.codec) {
	case FSIMG_DECMPFS_CODEC_ZLIB:
		return (zlib_decode(reader, window, stored, expected, error));
	case FSIMG_DECMPFS_CODEC_LZVN:
		return (chunk_decode_lzvn(window, stored, expected, error));
	case FSIMG_DECMPFS_CODEC_LZFSE:
		return (chunk_decode_lzfse(reader, window, stored, expected, error));
	case FSIMG_DECMPFS_CODEC_NONE:		/* always unencoded content */
	case FSIMG_DECMPFS_CODEC_LZBITMAP:	/* never admitted */
		break;
	}
	return (hfsplus_corrupt(error,
	    "invalid prepared HFS Plus compression type"));
}

/* The framing validator's way to the bytes of the current entry's resource
 * fork: the fork's index, which table_open() builds.
 */
static fsimg_status_t
fork_bytes(void *context, uint64_t offset, void *buffer, size_t length,
    fsimg_error_t *error)
{
	fsimg_reader_t *reader = context;

	return (hfsplus_fork_read(reader, hfsplus_state(reader)->file, offset,
	    buffer, length, error));
}

/* Index the resource fork and check its chunk table, keeping the index only for
 * a table that passes. A fork of no blocks has no index; its logical size is then
 * zero, which the validator refuses before it reads.
 */
static fsimg_status_t
table_open(fsimg_reader_t *reader, const hfsplus_file_t *file,
    fsimg_decmpfs_kind_t kind, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	fsimg_status_t status;

	status = hfsplus_fork_index(reader, file->id, HFSPLUS_FORK_RESOURCE,
	    &file->resource_fork, &hfsplus->file, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_decmpfs_open(kind, file->resource_fork.logical_size,
	    hfsplus->current.size, fork_bytes, reader, &hfsplus->compressed.table,
	    error);
	if (status != FSIMG_OK) {
		fsimg_extent_index_free(hfsplus->file);
		hfsplus->file = NULL;
	}
	return (status);
}

/* Open the current entry's compressed content, on its first resolve. Its record
 * must still be what listing admitted. An inline payload decodes here, once and
 * whole, its record scanned again only for this; a fork's chunks wait until each
 * is asked for.
 */
fsimg_status_t
hfsplus_compress_open(fsimg_reader_t *reader, const hfsplus_file_t *file,
    fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	const hfsplus_child_t *current = &hfsplus->current;
	fsimg_decmpfs_info_t info;
	hfsplus_attr_scan_t scan;
	hfsplus_window_t *window;
	fsimg_status_t status;

	if ((file->bsd.owner_flags & HFSPLUS_OWNER_COMPRESSED) == 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus file record changed since it was listed"));
	status = window_ready(reader, error);
	if (status != FSIMG_OK)
		return (status);
	window = hfsplus->window;
	status = hfsplus_attr_scan(reader, file->id, true, window->input, &scan,
	    error);
	if (status == FSIMG_OK)
		status = hfsplus_compress_check(file, &scan, error);
	if (status != FSIMG_OK)
		return (status);
	if (scan.header.type != current->compression ||
	    scan.header.uncompressed_size != current->size)
		return (hfsplus_corrupt(error,
		    "HFS Plus compression attribute changed since it was listed"));
	if (!admitted(current->compression, &info))
		return (hfsplus_corrupt(error,
		    "invalid prepared HFS Plus compression type"));
	switch (info.kind) {
	case FSIMG_DECMPFS_TABLE_RESOURCE:
	case FSIMG_DECMPFS_TABLE_WORDS:
		status = table_open(reader, file, info.kind, error);
		if (status != FSIMG_OK)
			return (status);
		break;
	case FSIMG_DECMPFS_INLINE:
		status = window_decode(reader, scan.payload_bytes,
		    (size_t)current->size, error);
		if (status != FSIMG_OK)
			return (status);
		window->chunk = 0;
		window->length = (size_t)current->size;
		window->valid = true;
		break;
	}
	hfsplus->compressed.open = true;
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_compress_table_check(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	uint64_t offset;
	size_t stored;
	fsimg_status_t status;

	for (uint64_t chunk = 0; chunk < hfsplus->compressed.table.chunks;
	    chunk++) {
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_decmpfs_chunk(&hfsplus->compressed.table, chunk,
		    fork_bytes, reader, &offset, &stored, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

static fsimg_status_t
chunk_load(fsimg_reader_t *reader, uint64_t chunk, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	hfsplus_window_t *window = hfsplus->window;
	uint64_t logical, offset;
	size_t expected, stored;
	fsimg_status_t status;

	/* A failed decode may have overwritten output, so never leave it marked valid. */
	window->valid = false;
	status = fsimg_decmpfs_chunk(&hfsplus->compressed.table, chunk,
	    fork_bytes, reader, &offset, &stored, error);
	if (status == FSIMG_OK)
		status = hfsplus_fork_read(reader, hfsplus->file, offset,
		    window->input, stored, error);
	if (status != FSIMG_OK)
		return (status);
	logical = chunk * FSIMG_DECMPFS_CHUNK_BYTES;
	expected = hfsplus->current.size - logical < FSIMG_DECMPFS_CHUNK_BYTES ?
	    (size_t)(hfsplus->current.size - logical) : FSIMG_DECMPFS_CHUNK_BYTES;
	status = window_decode(reader, stored, expected, error);
	if (status != FSIMG_OK)
		return (status);
	window->chunk = chunk;
	window->length = expected;
	window->valid = true;
	return (FSIMG_OK);
}

/* The decoded span containing offset: the window's chunk, decoded unless it is
 * already there, so sequential reading decodes each chunk once. An inline file
 * never reaches chunk_load: open left its window valid, and every offset is in
 * chunk 0, as its size is at most one chunk.
 */
fsimg_status_t
hfsplus_compress_resolve(fsimg_reader_t *reader, uint64_t offset,
    fsimg_extent_t *extent, fsimg_error_t *error)
{
	hfsplus_window_t *window = hfsplus_state(reader)->window;
	uint64_t chunk = offset / FSIMG_DECMPFS_CHUNK_BYTES;
	fsimg_status_t status;

	if (!window->valid || window->chunk != chunk) {
		status = chunk_load(reader, chunk, error);
		if (status != FSIMG_OK)
			return (status);
	}
	extent->logical_offset = chunk * FSIMG_DECMPFS_CHUNK_BYTES;
	extent->length = window->length;
	reader->held = window->output;
	return (FSIMG_OK);
}
