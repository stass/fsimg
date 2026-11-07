/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "erofs.h"

/*
 * The mapper: the one function that turns a logical offset of an inode into
 * storage, so every extent the driver reads, for a directory, a symbolic
 * link or file content, is bounded here once.
 *
 * A flat inode stores its size bytes contiguously from its start block, the
 * layout word. A flat-inline inode stores its first floor(size / B) blocks
 * the same way and its last size mod B bytes, the tail, inline at the end of
 * its body. For B = 4096 and size 10000 the stored extent is [0, 8192) from
 * the start block and the inline extent [8192, 10000) from the body's layout
 * offset. The start word addresses something only when a stored extent
 * exists, so it is read only when the requested offset falls in one: never
 * in an empty inode, and never in a flat-inline inode shorter than a block,
 * where producers leave 0xFFFFFFFF.
 *
 * A chunk-based inode divides its content into chunks of B << chunkbits
 * bytes, the last one short, and its body ends in an index of one entry per
 * chunk. For B = 4096, 8192-byte chunks and size 20000, offset 17000 falls
 * in chunk 2, [16384, 20000), and only entry 2 is read: a hole is a zero
 * extent, any other entry a stored extent from its block. Chunks may share
 * blocks, so nothing here or in the verifier's data claims rejects two
 * entries naming one block.
 *
 * A compressed inode's offset maps to the physical cluster holding it,
 * which zmap.c finds; the cluster's stored blocks must lie inside the
 * content domain, and its stored and decoded lengths inside the cluster
 * limits, 1 MiB and 12 MiB. A larger cluster is unsupported rather than
 * corrupt: the limits are the reference reader's, not the format's. An
 * inline tail, flat or compressed, is a cluster too, stored in the body.
 *
 * A fragment maps through the packed inode. For a file of 10000 bytes
 * whose last cluster, [6000, 10000), is a fragment at packed offset 50000,
 * offset 7000 is packed offset 51000; if the packed inode's cluster holding
 * it is [49152, 53248), the extent returned is the file's [6000, 9248), the
 * part of the fragment that cluster holds, read from the cluster's decoded
 * bytes 848 on. The packed inode uses no fragment of its own, which the
 * mapping of it checks, so the mapping nests exactly once.
 */

static fsimg_status_t	chunk_extent(fsimg_reader_t *, const erofs_inode_t *,
			    uint64_t, erofs_extent_t *, fsimg_error_t *);
static fsimg_status_t	cluster_extent(fsimg_reader_t *,
			    const erofs_inode_t *, uint64_t, bool, erofs_extent_t *,
			    fsimg_error_t *);
static fsimg_status_t	extent_find(fsimg_reader_t *, const erofs_inode_t *,
			    uint64_t, bool, erofs_extent_t *, fsimg_error_t *);
static fsimg_status_t	flat_extent(const fsimg_reader_t *,
			    const erofs_inode_t *, uint64_t, erofs_extent_t *,
			    fsimg_error_t *);
static fsimg_status_t	fragment_extent(fsimg_reader_t *,
			    const erofs_cluster_t *, uint64_t, erofs_extent_t *,
			    fsimg_error_t *);
static fsimg_status_t	stored_extent(const fsimg_reader_t *, uint64_t,
			    uint64_t, uint64_t, erofs_extent_t *,
			    fsimg_error_t *);

/*
 * The extent of inode holding logical byte offset, which must be below the
 * inode's size: a caller deriving an offset past it is a driver defect,
 * FSIMG_EINVAL. Every extent returned lies inside its storage, which each
 * mapper ensures, and holds offset, which the check below ensures
 * whichever mapper produced it, so a caller stepping from offset to the
 * extent's end always moves forward.
 */
fsimg_status_t
erofs_map(fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint64_t offset, erofs_extent_t *out, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = extent_find(reader, inode, offset, false, out, error);
	if (status == FSIMG_OK && (offset < out->logical_offset ||
	    offset - out->logical_offset >= out->length))
		return (erofs_reject(error, FSIMG_EINVAL,
		    "extent does not hold the mapped offset"));
	return (status);
}

/*
 * The extent of inode holding offset; packed says inode is the packed
 * inode, mapped for a fragment, which may not use a fragment itself.
 */
static fsimg_status_t
extent_find(fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint64_t offset, bool packed, erofs_extent_t *out, fsimg_error_t *error)
{
	const fsimg_erofs_reader_t *erofs = erofs_state(reader);
	uint64_t stored_bytes;

	if (offset >= inode->size)
		return (erofs_reject(error, FSIMG_EINVAL,
		    "mapping offset past the inode's size"));

	switch (inode->layout) {
	case EROFS_LAYOUT_FLAT:
		return (flat_extent(reader, inode, inode->size, out, error));
	case EROFS_LAYOUT_FLAT_INLINE:
		/* The tail is shorter than a block: the inode decoder refused
		 * a flat-inline size that is a whole number of blocks. */
		stored_bytes = inode->size - inode->size % erofs->block_bytes;
		if (offset < stored_bytes)
			return (flat_extent(reader, inode, stored_bytes, out,
			    error));
		/* erofs_inode_load() proved the body inside the domain. */
		*out = (erofs_extent_t){
			.logical_offset = stored_bytes,
			.length = inode->size - stored_bytes,
			.kind = EROFS_EXTENT_CLUSTER,
			.cluster = {
				.logical_offset = stored_bytes,
				.logical_length = inode->size - stored_bytes,
				.storage_offset = inode->offset +
				    inode->tail_offset,
				.stored_bytes = (uint32_t)(inode->size -
				    stored_bytes),
				.source = EROFS_SOURCE_INLINE,
				.mode = EROFS_CLUSTER_SHIFTED
			}
		};
		return (FSIMG_OK);
	case EROFS_LAYOUT_CHUNK_BASED:
		return (chunk_extent(reader, inode, offset, out, error));
	case EROFS_LAYOUT_COMPRESSED_FULL:
	case EROFS_LAYOUT_COMPRESSED_COMPACT:
		return (cluster_extent(reader, inode, offset, packed, out,
		    error));
	}
	return (erofs_reject(error, FSIMG_ECORRUPT, "inode data layout"));
}

/*
 * The cluster extent of compressed inode holding offset, or the fragment
 * extent when the cluster is a fragment, which the packed inode may not
 * use. A cluster past the size limits is unsupported and one outside the
 * volume corrupt; erofs_zmap() returns a cluster containing offset inside
 * the inode's size, so one that does not is a driver defect, FSIMG_EINVAL.
 * An inline tail lies in the body, which erofs_inode_load() proved inside
 * the domain and a block.
 */
static fsimg_status_t
cluster_extent(fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint64_t offset, bool packed, erofs_extent_t *out, fsimg_error_t *error)
{
	erofs_cluster_t cluster;
	fsimg_status_t status;

	status = erofs_zmap(reader, inode, offset, &cluster, error);
	if (status != FSIMG_OK)
		return (status);
	if (cluster.logical_offset > offset ||
	    !fsimg_range_fits(cluster.logical_offset, cluster.logical_length,
	    inode->size))
		return (erofs_reject(error, FSIMG_EINVAL,
		    "cluster outside the inode's content"));
	if (cluster.source == EROFS_SOURCE_PACKED) {
		if (packed)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "the packed inode uses a fragment"));
		return (fragment_extent(reader, &cluster, offset, out, error));
	}

	if (cluster.stored_bytes > EROFS_CLUSTER_STORED_MAX)
		return (erofs_reject(error, FSIMG_EUNSUPPORTED,
		    "physical cluster above 1 MiB"));
	if (cluster.logical_length > EROFS_CLUSTER_DECODED_MAX)
		return (erofs_reject(error, FSIMG_EUNSUPPORTED,
		    "decoded cluster above 12 MiB"));
	if (cluster.source == EROFS_SOURCE_BLOCKS &&
	    !fsimg_range_fits(cluster.storage_offset, cluster.stored_bytes,
	    reader->content_domain.length))
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "physical cluster outside the volume"));

	*out = (erofs_extent_t){
		.logical_offset = cluster.logical_offset,
		.storage_offset = cluster.storage_offset,
		.length = cluster.logical_length,
		.kind = EROFS_EXTENT_CLUSTER,
		.cluster = cluster
	};
	return (FSIMG_OK);
}

/*
 * The extent of a file's fragment cluster holding offset: the
 * fragment's whole range must lie inside the packed inode's content, and
 * the packed inode's extent holding offset's byte there, clipped to the
 * fragment, becomes the file's extent. Every operand is below the packed
 * inode's size once the range fits it, so no sum wraps.
 */
static fsimg_status_t
fragment_extent(fsimg_reader_t *reader, const erofs_cluster_t *cluster,
    uint64_t offset, erofs_extent_t *out, fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	uint64_t base = cluster->storage_offset;
	uint64_t length = cluster->logical_length;
	uint64_t first, end, inner_end;
	erofs_extent_t inner;
	fsimg_status_t status;

	if (!erofs->has_packed)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "fragment without a packed inode"));
	if (!fsimg_range_fits(base, length, erofs->packed.size))
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "fragment past the packed inode's size"));
	status = extent_find(reader, &erofs->packed,
	    base + (offset - cluster->logical_offset), true, &inner, error);
	if (status != FSIMG_OK)
		return (status);

	first = inner.logical_offset > base ? inner.logical_offset : base;
	inner_end = inner.logical_offset + inner.length;
	end = inner_end < base + length ? inner_end : base + length;
	*out = inner;
	out->logical_offset = cluster->logical_offset + (first - base);
	out->length = end - first;
	out->fragment = true;
	if (inner.kind == EROFS_EXTENT_STORED)
		out->storage_offset += first - inner.logical_offset;
	else if (inner.kind == EROFS_EXTENT_CLUSTER)
		out->cluster_skip = first - inner.logical_offset;
	return (FSIMG_OK);
}

/*
 * The chunk of a chunk-based inode holding offset, from its one index
 * entry. erofs_inode_load() decoded the chunk format and proved the whole
 * index it implies, ceil(size / chunk) entries, inside the body, so the
 * entry's body offset cannot overflow and the read stays in the body. The
 * chunk starts at index * chunk, at most offset, and covers
 * min(chunk, size - start) bytes, so neither can overflow.
 */
static fsimg_status_t
chunk_extent(fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint64_t offset, erofs_extent_t *out, fsimg_error_t *error)
{
	const erofs_chunk_format_t *format = &inode->chunk_format;
	unsigned char raw[8];
	erofs_chunk_t chunk;
	uint64_t index, start, length;
	const char *what;
	fsimg_status_t status;

	index = offset / format->chunk_bytes;
	status = erofs_body_read(reader, inode,
	    inode->layout_offset + index * format->entry_bytes,
	    format->entry_bytes, raw, error);
	if (status != FSIMG_OK)
		return (status);
	status = erofs_chunk_decode(raw, format->entry_bytes, format, &chunk,
	    &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));

	start = index * format->chunk_bytes;
	length = inode->size - start;
	if (length > format->chunk_bytes)
		length = format->chunk_bytes;
	if (chunk.hole) {
		*out = (erofs_extent_t){
			.logical_offset = start,
			.length = length,
			.kind = EROFS_EXTENT_ZERO
		};
		return (FSIMG_OK);
	}
	return (stored_extent(reader, chunk.block, start, length, out, error));
}

/*
 * The stored extent [0, stored_bytes) of a flat or flat-inline inode, from
 * its start block, the layout word with layout_high as bits 32 to 47. A
 * start word of 0xFFFFFFFF with zero high bits, which producers write only
 * where it is not read, is refused outright: a volume declaring no block
 * count has the input's length as its domain, which can reach that block.
 * The 48-bit null, 0xFFFF above it, is refused the same way.
 */
static fsimg_status_t
flat_extent(const fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint64_t stored_bytes, erofs_extent_t *out, fsimg_error_t *error)
{
	uint64_t start_block;

	if (inode->layout_high == 0 && inode->layout_word == UINT32_MAX)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "data start block 0xFFFFFFFF"));
	if (inode->layout_high == UINT16_MAX && inode->layout_word == UINT32_MAX)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "data start block 0xFFFFFFFFFFFF"));
	start_block = (uint64_t)inode->layout_high << 32 | inode->layout_word;
	return (stored_extent(reader, start_block, 0, stored_bytes, out,
	    error));
}

/*
 * The stored extent of logical bytes [logical_offset, logical_offset +
 * length) from block. A block number has at most 48 bits and B at most
 * 2^12, so its byte offset cannot overflow. The extent occupies whole
 * blocks, and all of them must lie inside the content domain, the domain's
 * whole blocks.
 */
static fsimg_status_t
stored_extent(const fsimg_reader_t *reader, uint64_t block,
    uint64_t logical_offset, uint64_t length, erofs_extent_t *out,
    fsimg_error_t *error)
{
	const fsimg_erofs_reader_t *erofs = erofs_state(reader);
	uint64_t block_mask, storage_offset, whole_bytes;

	storage_offset = block << erofs->super.blkszbits;

	/*
	 * The content domain starts at volume byte 0 and is a whole number
	 * of blocks, so once length fits it, rounding it up to whole blocks
	 * stays within the domain's length and cannot overflow.
	 */
	if (length > reader->content_domain.length)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "data blocks outside the volume"));
	block_mask = (uint64_t)erofs->block_bytes - 1;
	whole_bytes = (length + block_mask) & ~block_mask;
	if (!fsimg_range_fits(storage_offset, whole_bytes,
	    reader->content_domain.length))
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "data blocks outside the volume"));

	*out = (erofs_extent_t){
		.logical_offset = logical_offset,
		.storage_offset = storage_offset,
		.length = length,
		.kind = EROFS_EXTENT_STORED
	};
	return (FSIMG_OK);
}
