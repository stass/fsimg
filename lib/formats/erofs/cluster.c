/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "codec.h"
#include "erofs.h"
#include "internal.h"

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
 * Decoding a cluster reads its stored blocks into the input buffer, at most
 * 1 MiB. A plain cluster is copied, rotated when interlaced. A compressed
 * stream is right-aligned in its blocks behind zero padding, so it starts at
 * the first nonzero byte and ends at the cluster's last byte; it is decoded
 * to exactly the extent's length, or under a partial reference to the
 * extent's length without requiring the stream to end there. The decoders
 * never see an index, a header or an address.
 */

static fsimg_status_t	buffer_reserve(fsimg_reader_t *, unsigned char **,
			    size_t *, size_t, fsimg_error_t *);
static fsimg_status_t	lz4_decode(const erofs_cluster_t *,
			    const unsigned char *, size_t, unsigned char *,
			    fsimg_error_t *);
static bool		window_holds(const erofs_window_t *,
			    const erofs_cluster_t *);

static const char *const algorithm_names[EROFS_ALGORITHM_COUNT] = {
	[EROFS_ALGORITHM_LZ4] = "LZ4",
	[EROFS_ALGORITHM_LZMA] = "MicroLZMA",
	[EROFS_ALGORITHM_DEFLATE] = "DEFLATE",
	[EROFS_ALGORITHM_ZSTD] = "Zstandard"
};

/*
 * Whether this build decodes cluster: a plain cluster always, an LZ4
 * stream only on a volume with LZ4_0PADDING, since without it nothing
 * records where a stream starts, and no other algorithm. A cluster it
 * cannot decode is FSIMG_EUNSUPPORTED naming what is missing.
 */
fsimg_status_t
erofs_cluster_readable(const fsimg_reader_t *reader,
    const erofs_cluster_t *cluster, fsimg_error_t *error)
{
	const fsimg_erofs_reader_t *erofs = erofs_state(reader);
	char phrase[sizeof(((fsimg_error_t *)NULL)->message)];

	if (cluster->mode != EROFS_CLUSTER_COMPRESSED)
		return (FSIMG_OK);
	if (cluster->algorithm != EROFS_ALGORITHM_LZ4) {
		snprintf(phrase, sizeof(phrase), "compression algorithm %s",
		    algorithm_names[cluster->algorithm]);
		return (erofs_reject(error, FSIMG_EUNSUPPORTED, phrase));
	}
	if ((erofs->super.incompat & EROFS_INCOMPAT_LZ4_0PADDING) == 0)
		return (erofs_reject(error, FSIMG_EUNSUPPORTED,
		    "LZ4 cluster without LZ4_0PADDING"));
	return (FSIMG_OK);
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
	size_t padding, rotation, transferred, within;
	fsimg_status_t status;

	if (window_holds(window, cluster))
		return (FSIMG_OK);
	window->keyed = false;

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

	status = fsimg_reader_bytes(reader, cluster->storage_offset,
	    window->input, stored, &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	if (transferred != stored)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "physical cluster truncated"));

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
		status = lz4_decode(cluster, window->input + padding,
		    stored - padding, window->output, error);
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
lz4_decode(const erofs_cluster_t *cluster, const unsigned char *stream,
    size_t length, unsigned char *out, fsimg_error_t *error)
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
