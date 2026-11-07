/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <zlib.h>

#include "codec-memory.h"
#include "codec.h"
#include "erofs.h"
#include "internal.h"
#include "layer.h"

/*
 * Decode windows: a physical cluster's stored bytes turned into its logical
 * extent. A window holds one decoded cluster and the key naming it, the
 * whole normalized cluster but its logical offset, which is every field
 * that changes the decoded bytes or whether they are accepted. A window
 * whose key equals a cluster is reused without decoding, so a delivery that
 * resolves several times inside one cluster decodes it once, and a hit
 * always returns exactly what a fresh decode would.
 *
 * erofs_window_fill() is the only writer of a window. It clears the key,
 * fills the bytes, and sets the key last, only after the decoded length is
 * proved, so a decode that fails partway, by corruption or by a budget,
 * leaves an unkeyed window and never a key naming bytes it does not hold.
 *
 * Decoding a cluster reads its stored bytes into the input buffer, at most
 * 1 MiB: whole blocks of the volume, or an inline tail from the space of
 * its inode, keyed at its offset there with the space, so equal keys name
 * equal bytes. A
 * fragment is no cluster: the mapper resolves it to the packed inode's own.
 * A plain cluster is copied, rotated when interlaced. A compressed
 * stream is right-aligned in its blocks behind zero padding, so it starts at
 * the first nonzero byte and ends at the cluster's last byte; it is decoded
 * to exactly the extent's length, or under a partial reference to the
 * extent's length without requiring the stream to end there. The decoders
 * never see an index, a header or an address.
 *
 * LZ4 decodes through the project codec and DEFLATE through the system
 * zlib, in every build. MicroLZMA and Zstandard decode through the
 * block_decode operation of the xz and zstd compression rows, which exists
 * only when the build has the library; a cluster of an algorithm whose row
 * lacks it is FSIMG_EUNSUPPORTED naming the library, so the rest of the
 * volume stays readable (specs/design-compression-libs.md#erofs-block-
 * decoding).
 */

/* Decode one stream into the cluster's extent. */
typedef fsimg_status_t	stream_decoder_t(fsimg_reader_t *,
			    const erofs_cluster_t *, unsigned char *, size_t,
			    unsigned char *, fsimg_error_t *);

/*
 * One algorithm: its name in messages and in the capability listing, and
 * its decoder. library_decode serves every algorithm with a layer, the
 * compression row whose block_decode decodes it, and library names what a
 * build without that operation lacks.
 */
typedef struct algorithm {
	const char	*name;
	const char	*listed;
	stream_decoder_t *decode;
	const char	*layer;
	const char	*library;
} algorithm_t;

static fsimg_status_t	buffer_reserve(fsimg_reader_t *, unsigned char **,
			    size_t *, size_t, fsimg_error_t *);
static stream_decoder_t	lz4_decode;
static stream_decoder_t	deflate_decode;
static stream_decoder_t	library_decode;
static const fsimg_compression_ops_t *library_row(unsigned);
static fsimg_status_t	input_read(fsimg_reader_t *, erofs_window_t *,
			    const erofs_cluster_t *, fsimg_error_t *);
static bool		window_holds(const erofs_window_t *,
			    const erofs_cluster_t *);

static const algorithm_t algorithms[EROFS_ALGORITHM_COUNT] = {
	[EROFS_ALGORITHM_LZ4] = { .name = "LZ4", .listed = "lz4",
	    .decode = lz4_decode },
	[EROFS_ALGORITHM_LZMA] = { .name = "MicroLZMA", .listed = "lzma",
	    .decode = library_decode, .layer = "xz",
	    .library = "liblzma 5.4" },
	[EROFS_ALGORITHM_DEFLATE] = { .name = "DEFLATE", .listed = "deflate",
	    .decode = deflate_decode },
	[EROFS_ALGORITHM_ZSTD] = { .name = "Zstandard", .listed = "zstd",
	    .decode = library_decode, .layer = "zstd", .library = "libzstd" }
};

/*
 * Whether this build decodes cluster: a plain cluster always, an LZ4
 * stream only on a volume with LZ4_0PADDING, since without it nothing
 * records where a stream starts, DEFLATE always, and MicroLZMA and
 * Zstandard when the build has the library. A cluster it cannot decode is
 * FSIMG_EUNSUPPORTED naming what is missing.
 */
fsimg_status_t
erofs_cluster_readable(const fsimg_reader_t *reader,
    const erofs_cluster_t *cluster, fsimg_error_t *error)
{
	const fsimg_erofs_reader_t *erofs = erofs_state(reader);
	char phrase[sizeof(((fsimg_error_t *)NULL)->message)];
	const algorithm_t *algorithm;

	if (cluster->mode != EROFS_CLUSTER_COMPRESSED)
		return (FSIMG_OK);
	algorithm = &algorithms[cluster->algorithm];
	if (algorithm->layer != NULL &&
	    library_row(cluster->algorithm) == NULL) {
		snprintf(phrase, sizeof(phrase), "%s cluster needs %s",
		    algorithm->name, algorithm->library);
		return (erofs_reject(error, FSIMG_EUNSUPPORTED, phrase));
	}
	if (cluster->algorithm == EROFS_ALGORITHM_LZ4 &&
	    (erofs->super.incompat & EROFS_INCOMPAT_LZ4_0PADDING) == 0)
		return (erofs_reject(error, FSIMG_EUNSUPPORTED,
		    "LZ4 cluster without LZ4_0PADDING"));
	return (FSIMG_OK);
}

/*
 * The algorithms this build decodes, for the capability listing, in
 * identifier order: each listed name, then the library and version it
 * depends on, which a library row reports and zlib's own call gives for
 * DEFLATE; an algorithm whose row lacks block_decode is left out.
 */
void
erofs_codecs(char *out, size_t capacity)
{
	const fsimg_compression_ops_t *row;
	char version[48];
	size_t used = 0;

	out[0] = '\0';
	for (unsigned a = 0; a < EROFS_ALGORITHM_COUNT; a++) {
		version[0] = '\0';
		if (a == EROFS_ALGORITHM_DEFLATE)
			fsimg_zlib_version(version, sizeof(version));
		else if (algorithms[a].layer != NULL) {
			row = library_row(a);
			if (row == NULL)
				continue;
			row->version(version, sizeof(version));
		}
		used += (size_t)snprintf(out + used, capacity - used, "%s%s%s%s",
		    used == 0 ? "" : "; ", algorithms[a].listed,
		    version[0] == '\0' ? "" : ", ", version);
		if (used >= capacity)
			return;	/* truncated, still terminated */
	}
}

/*
 * Leave cluster's logical extent in window->output, decoding it unless the
 * window already holds it. The stored and decoded lengths are within the
 * cluster limits, which erofs_map() checked, and every byte read and
 * decoded is one unit of work.
 */
fsimg_status_t
erofs_window_fill(fsimg_reader_t *reader, erofs_window_t *window,
    const erofs_cluster_t *cluster, fsimg_error_t *error)
{
	size_t length = (size_t)cluster->logical_length;
	size_t stored = cluster->stored_bytes;
	size_t padding, rotation, within;
	fsimg_status_t status;

	if (window_holds(window, cluster))
		return (FSIMG_OK);
	window->keyed = false;
	if (cluster->source == EROFS_SOURCE_PACKED)
		return (erofs_reject(error, FSIMG_EINVAL,
		    "decoding a fragment as a cluster"));

	status = erofs_cluster_readable(reader, cluster, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_session_charge(&reader->session, 0, 0,
	    (uint64_t)stored + length, error);
	if (status != FSIMG_OK)
		return (status);
	status = buffer_reserve(reader, &window->input,
	    &window->input_capacity, stored, error);
	if (status != FSIMG_OK)
		return (status);
	status = buffer_reserve(reader, &window->output,
	    &window->output_capacity, length, error);
	if (status != FSIMG_OK)
		return (status);

	status = input_read(reader, window, cluster, error);
	if (status != FSIMG_OK)
		return (status);

	switch (cluster->mode) {
	case EROFS_CLUSTER_SHIFTED:
		/* A plain extent is no longer than its blocks. */
		bcopy(window->input, window->output, length);
		break;
	case EROFS_CLUSTER_INTERLACED:
		/* Extent byte j lies at stored byte (rotation + j) mod stored:
		 * first the part from rotation to the end, then the rest from
		 * the start. */
		rotation = cluster->rotation;
		within = stored - rotation < length ? stored - rotation : length;
		bcopy(window->input + rotation, window->output, within);
		bcopy(window->input, window->output + within, length - within);
		break;
	case EROFS_CLUSTER_COMPRESSED:
		for (padding = 0; padding < stored; padding++)
			if (window->input[padding] != 0)
				break;
		if (padding == stored)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "compressed cluster holds no stream"));
		status = algorithms[cluster->algorithm].decode(reader, cluster,
		    window->input + padding, stored - padding, window->output,
		    error);
		if (status != FSIMG_OK)
			return (status);
		break;
	}

	window->key = *cluster;
	window->key.logical_offset = 0;
	window->keyed = true;
	return (FSIMG_OK);
}

/* Release a window's buffers; the window is then empty. */
void
erofs_window_free(erofs_window_t *window)
{
	fsimg_dealloc(window->input);
	fsimg_dealloc(window->output);
	bzero(window, sizeof(*window));
}

/*
 * Read cluster's stored bytes into window's input: blocks the mapper
 * proved inside the content domain, where a range past the input's end is
 * a truncated volume, or an inline tail through the metadata reader in
 * the space of the inode holding it.
 */
static fsimg_status_t
input_read(fsimg_reader_t *reader, erofs_window_t *window,
    const erofs_cluster_t *cluster, fsimg_error_t *error)
{
	size_t transferred;
	fsimg_status_t status;

	if (cluster->source == EROFS_SOURCE_INLINE)
		return (erofs_meta_read(reader, cluster->space,
		    cluster->storage_offset, cluster->stored_bytes,
		    window->input, error));
	status = fsimg_reader_bytes(reader, cluster->storage_offset,
	    window->input, cluster->stored_bytes, &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	if (transferred != cluster->stored_bytes)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "physical cluster truncated"));
	return (FSIMG_OK);
}

/*
 * Whether window holds cluster's bytes: it is keyed and every key field
 * equals the cluster's, the logical offset aside, since the decoded bytes
 * do not depend on where in the file the extent lies.
 */
static bool
window_holds(const erofs_window_t *window, const erofs_cluster_t *cluster)
{
	const erofs_cluster_t *key = &window->key;

	return (window->keyed &&
	    key->logical_length == cluster->logical_length &&
	    key->storage_offset == cluster->storage_offset &&
	    key->stored_bytes == cluster->stored_bytes &&
	    key->rotation == cluster->rotation &&
	    key->source == cluster->source &&
	    key->space == cluster->space &&
	    key->mode == cluster->mode &&
	    key->algorithm == cluster->algorithm &&
	    key->partial == cluster->partial);
}

/*
 * Decode one LZ4 block of length bytes into the cluster's extent. A full
 * reference must end exactly at the block's end with the extent filled; a
 * partial one stops once the extent is full. Either way fewer bytes are
 * corrupt. The decoder writes nothing past the extent and reports a
 * rejected stream as FSIMG_ECORRUPT naming its check.
 */
static fsimg_status_t
lz4_decode(fsimg_reader_t *reader FSIMG_UNUSED, const erofs_cluster_t *cluster,
    unsigned char *stream, size_t length, unsigned char *out,
    fsimg_error_t *error)
{
	size_t extent = (size_t)cluster->logical_length;
	fsimg_codec_step_t step;
	fsimg_status_t status;

	if (cluster->partial)
		status = fsimg_lz4_decode_partial(stream, length, out, extent,
		    &step, error);
	else
		status = fsimg_lz4_decode(stream, length, out, extent, &step,
		    error);
	if (status != FSIMG_OK)
		return (status);
	if (step.produced != extent)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "LZ4 stream decodes to the wrong length"));
	return (FSIMG_OK);
}

/*
 * Decode one raw DEFLATE stream into the cluster's extent with a fresh
 * inflater whose state is charged to the context and freed before
 * returning. Inflate gets exactly the extent as output, so a longer stream
 * stops there. The stream carries its own end, the final block, at or
 * before the cluster's last byte; bytes after it are not judged. A stream
 * that ends with the extent full is the extent; one that fills the extent
 * still going is the extent only under a partial reference; one that ends
 * short, runs out of input or is invalid is corrupt. The record's window
 * size is no stream parameter: raw DEFLATE inflates with a 32 KiB window
 * whatever it says (specs/design-erofs.md#configuration).
 */
static fsimg_status_t
deflate_decode(fsimg_reader_t *reader, const erofs_cluster_t *cluster,
    unsigned char *stream, size_t length, unsigned char *out,
    fsimg_error_t *error)
{
	fsimg_codec_memory_t memory;
	z_stream inflater;
	fsimg_status_t status;
	int code;

	bzero(&inflater, sizeof(inflater));
	fsimg_zlib_memory_bind(&memory, reader->ctx, &inflater);
	code = inflateInit2(&inflater, -MAX_WBITS);
	if (code == Z_OK) {
		/* Both lengths are at most the cluster limits, far below a
		 * uInt's range. */
		inflater.next_in = stream;
		inflater.avail_in = (uInt)length;
		inflater.next_out = out;
		inflater.avail_out = (uInt)cluster->logical_length;
		code = inflate(&inflater, Z_FINISH);
		inflateEnd(&inflater);
	}
	switch (code) {
	case Z_STREAM_END:
		if (inflater.avail_out != 0)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "DEFLATE stream ends before its extent is full"));
		return (FSIMG_OK);
	case Z_BUF_ERROR:	/* Z_FINISH reports any stop short of the end so */
		if (inflater.avail_out != 0)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "DEFLATE stream is truncated"));
		if (!cluster->partial)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "DEFLATE stream continues past its extent"));
		return (FSIMG_OK);
	case Z_MEM_ERROR:
		status = fsimg_codec_memory_error(&memory, error);
		if (error != NULL && error->path[0] == '\0')
			snprintf(error->path, sizeof(error->path), "zlib");
		return (status);
	case Z_STREAM_ERROR:
	case Z_VERSION_ERROR:
		return (erofs_reject(error, FSIMG_ESTATE,
		    "zlib refused the DEFLATE decode stream"));
	default:
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "DEFLATE stream is corrupt"));
	}
}

/*
 * Decode a MicroLZMA or Zstandard stream through its compression row's
 * block decoder, under the dictionary or window the volume's configuration
 * record declares, which codec.c proved within the record's range. The
 * row's absence was refused by erofs_cluster_readable() first. A fresh
 * MicroLZMA decoder cannot reach back further than it has produced, so its
 * dictionary is the smaller of the record's and the extent, at least the
 * record's minimum of 4096: a volume declaring 8 MiB does not charge 8 MiB
 * for a 4 KiB cluster, and a stream needing more still fails. A corrupt
 * stream's message gains the driver's prefix.
 */
static fsimg_status_t
library_decode(fsimg_reader_t *reader, const erofs_cluster_t *cluster,
    unsigned char *stream, size_t length, unsigned char *out,
    fsimg_error_t *error)
{
	const erofs_config_t *config = &erofs_state(reader)->config;
	const fsimg_compression_ops_t *row = library_row(cluster->algorithm);
	size_t extent = (size_t)cluster->logical_length, bound;
	char what[sizeof(((fsimg_error_t *)NULL)->message)];
	fsimg_status_t status;

	if (cluster->algorithm == EROFS_ALGORITHM_LZMA) {
		bound = config->lzma_dict_size < extent ?
		    config->lzma_dict_size : extent;
		if (bound < EROFS_LZMA_DICT_MIN)
			bound = EROFS_LZMA_DICT_MIN;
	} else
		bound = (size_t)1 << config->zstd_window_log;
	status = row->block_decode(reader->ctx, stream, length, out, extent,
	    cluster->partial, bound, error);
	if (status == FSIMG_ECORRUPT && error != NULL) {
		snprintf(what, sizeof(what), "%s", error->message);
		return (erofs_reject(error, status, what));
	}
	return (status);
}

/* The compression row that block-decodes algorithm, or NULL without one. */
static const fsimg_compression_ops_t *
library_row(unsigned algorithm)
{
	const fsimg_layer_t *layer;

	if (algorithms[algorithm].layer == NULL)
		return (NULL);
	layer = fsimg_layer_find(algorithms[algorithm].layer);
	if (layer == NULL || layer->compression == NULL ||
	    layer->compression->block_decode == NULL)
		return (NULL);
	return (layer->compression);
}

/*
 * Make *buffer hold at least bytes, replacing it with a larger allocation
 * charged to the context when it is smaller. Its contents are not kept. A
 * budget refusal keeps the context's status and names the buffer's size,
 * so a volume whose clusters exceed the memory budget says why.
 */
static fsimg_status_t
buffer_reserve(fsimg_reader_t *reader, unsigned char **buffer,
    size_t *capacity, size_t bytes, fsimg_error_t *error)
{
	unsigned char *grown;
	fsimg_error_t detail;
	size_t reason;

	if (bytes <= *capacity && *buffer != NULL)
		return (FSIMG_OK);
	fsimg_dealloc(*buffer);
	*buffer = NULL;
	*capacity = 0;
	grown = fsimg_alloc(reader->ctx, bytes == 0 ? 1 : bytes, &detail);
	if (grown == NULL) {
		reason = strlen(detail.message);
		snprintf(detail.message + reason, sizeof(detail.message) - reason,
		    " (EROFS decode window of %zu bytes)", bytes);
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	*buffer = grown;
	*capacity = bytes;
	return (FSIMG_OK);
}
