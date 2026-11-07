/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "erofs.h"

/*
 * The metadata reader: the one place a NID becomes an address and a
 * metadata address becomes bytes, so the inode address rule and the space
 * bounds exist once. Nothing here keeps what it reads: an inode is decoded
 * from a stack buffer, and later readers of its body ask for the ranges
 * they need, so a body's length costs reads, never memory.
 *
 * Open refuses the metadata box, so an opened volume has one metadata
 * space, the primary one. The box space is part of the interface, and a
 * read of it is refused as unsupported.
 */

static fsimg_status_t	nid_locate(const fsimg_erofs_reader_t *, uint64_t,
			    erofs_space_t *, uint64_t *, fsimg_error_t *);
static fsimg_status_t	tail_check(const fsimg_erofs_reader_t *, uint64_t,
			    const erofs_inode_t *, fsimg_error_t *);
static fsimg_status_t	zheader_load(fsimg_reader_t *, erofs_space_t, uint64_t,
			    erofs_inode_t *, fsimg_error_t *);

/*
 * length bytes at offset of one metadata space into out. A primary-space
 * range must lie inside the address domain; inside it, a range past the
 * input's end is a truncation that bounded I/O reports as corrupt.
 */
fsimg_status_t
erofs_meta_read(fsimg_reader_t *reader, erofs_space_t space, uint64_t offset,
    size_t length, void *out, fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	size_t transferred;
	fsimg_status_t status;

	if (space != EROFS_SPACE_PRIMARY)
		return (erofs_reject(error, FSIMG_EUNSUPPORTED,
		    "metadata box read"));
	if (!fsimg_range_fits(offset, length, erofs->domain_bytes))
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "metadata outside the address domain"));

	status = fsimg_reader_bytes(reader, offset, out, length, &transferred,
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (transferred != length)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "metadata read truncated"));
	return (FSIMG_OK);
}

/*
 * Load the inode that nid names: find it, read and decode its fixed part, and
 * check the rules that depend on where it is. The fixed part is read in
 * the compact size first and the rest only for an extended inode, so a
 * compact inode in the last 32 bytes of the domain loads. The body's span,
 * [offset, offset + body_bytes) of its space, must lie in the space, and an
 * inline tail inside one block of it. Of the body only a compressed
 * inode's map header is read here, since the body's length depends on it.
 */
fsimg_status_t
erofs_inode_load(fsimg_reader_t *reader, uint64_t nid, erofs_inode_t *out,
    fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	unsigned char raw[EROFS_INODE_EXTENDED_BYTES];
	erofs_inode_t inode;
	erofs_space_t space;
	uint64_t offset;
	const char *what;
	size_t fixed_bytes;
	fsimg_status_t status;

	status = nid_locate(erofs, nid, &space, &offset, error);
	if (status != FSIMG_OK)
		return (status);
	status = erofs_meta_read(reader, space, offset,
	    EROFS_INODE_COMPACT_BYTES, raw, error);
	if (status != FSIMG_OK)
		return (status);
	fixed_bytes = erofs_inode_fixed_bytes(raw);
	if (fixed_bytes > EROFS_INODE_COMPACT_BYTES) {
		/* The compact read proved offset + 32 inside the domain. */
		status = erofs_meta_read(reader, space,
		    offset + EROFS_INODE_COMPACT_BYTES,
		    fixed_bytes - EROFS_INODE_COMPACT_BYTES,
		    raw + EROFS_INODE_COMPACT_BYTES, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = erofs_inode_decode(raw, fixed_bytes, &erofs->super, &inode,
	    &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));

	inode.tail_offset = inode.layout == EROFS_LAYOUT_FLAT_INLINE ?
	    inode.layout_offset : inode.body_bytes;
	if (inode.layout == EROFS_LAYOUT_COMPRESSED_FULL ||
	    inode.layout == EROFS_LAYOUT_COMPRESSED_COMPACT) {
		status = zheader_load(reader, space, offset, &inode, error);
		if (status != FSIMG_OK)
			return (status);
	}

	/*
	 * Only the primary space reaches this point, since a box read is
	 * refused above, so the domain bounds the body.
	 */
	if (!fsimg_range_fits(offset, inode.body_bytes, erofs->domain_bytes))
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "inode body outside the address domain"));
	status = tail_check(erofs, offset, &inode, error);
	if (status != FSIMG_OK)
		return (status);

	inode.nid = nid;
	inode.space = space;
	inode.offset = offset;
	*out = inode;
	return (FSIMG_OK);
}

/*
 * length bytes at offset of an inode erofs_inode_load() returned, within
 * its body [0, body_bytes) from its first byte: the attribute region, an
 * inline tail or an index, each read in the pieces its reader needs. Its
 * readers derive ranges from on-disk fields, so a range outside the body
 * is corrupt rather than a caller's error.
 */
fsimg_status_t
erofs_body_read(fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint64_t offset, size_t length, void *out, fsimg_error_t *error)
{
	uint64_t at;

	if (!fsimg_range_fits(offset, length, inode->body_bytes) ||
	    !fsimg_add_u64(inode->offset, offset, &at))
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "read outside the inode's body"));
	return (erofs_meta_read(reader, inode->space, at, length, out,
	    error));
}

/*
 * Decode the map header of the compressed inode at offset of space and set
 * the body's end by it. erofs_inode_decode() measured the body as the
 * header and a full index of one record per block; a whole-file fragment
 * has no index, so its body ends at the header, and an inline tail's
 * h_idata_size stored bytes follow the index: the byte after the last full
 * record, or the end of the compact group holding the last record, which
 * erofs_compact_layout() places at the index's end (E15,
 * e15-c-l2b-tail-at-L). The metadata reader bounds the header's read; the
 * caller checks the whole body's span once its end is known.
 */
static fsimg_status_t
zheader_load(fsimg_reader_t *reader, erofs_space_t space, uint64_t offset,
    erofs_inode_t *inode, fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	unsigned char raw[EROFS_ZHEADER_BYTES];
	uint64_t at;
	const char *what;
	fsimg_status_t status;

	if (!fsimg_add_u64(offset, inode->layout_offset, &at))
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "inode body outside the address domain"));
	status = erofs_meta_read(reader, space, at, sizeof(raw), raw, error);
	if (status != FSIMG_OK)
		return (status);
	status = erofs_zheader_decode(raw, &erofs->super, inode->layout,
	    &inode->zheader, &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));

	if (inode->zheader.whole_fragment)
		inode->body_bytes = inode->layout_offset + EROFS_ZHEADER_BYTES;
	inode->tail_offset = inode->body_bytes;
	if (!fsimg_add_u64(inode->body_bytes, inode->zheader.tail_bytes,
	    &inode->body_bytes))
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "compressed index length"));
	return (FSIMG_OK);
}

/*
 * An inline tail, [tail_offset, body_bytes) of the body at offset, lies
 * inside one block: the kernel's rule for a flat-inline tail and for a
 * compressed one alike. A tail ending exactly at a block's end stays
 * inside that block, and an empty tail is no tail.
 */
static fsimg_status_t
tail_check(const fsimg_erofs_reader_t *erofs, uint64_t offset,
    const erofs_inode_t *inode, fsimg_error_t *error)
{
	uint64_t first, last;

	if (inode->tail_offset >= inode->body_bytes)
		return (FSIMG_OK);
	/* erofs_inode_load() proved offset + body_bytes inside the domain. */
	first = offset + inode->tail_offset;
	last = offset + inode->body_bytes - 1;
	if (first >> erofs->super.blkszbits != last >> erofs->super.blkszbits)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "inline tail crosses a block boundary"));
	return (FSIMG_OK);
}

/*
 * The space and offset of inode nid. A NID counts 32-byte slots from the
 * metadata block, meta_blkaddr * B, of the primary space; with bit 63 set
 * it counts them from the start of the metadata box's content instead,
 * which a volume without the box does not have. The slot product can
 * overflow 64 bits, so both steps are checked; the metadata block's byte
 * offset is below 2^44 and cannot.
 */
static fsimg_status_t
nid_locate(const fsimg_erofs_reader_t *erofs, uint64_t nid,
    erofs_space_t *space, uint64_t *offset, fsimg_error_t *error)
{
	uint64_t base, slots;

	if ((nid & EROFS_NID_BOX) != 0)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "NID in a metadata box the volume does not have"));
	base = (uint64_t)erofs->super.meta_blkaddr << erofs->super.blkszbits;
	if (!fsimg_mul_u64(nid, EROFS_NID_SLOT_BYTES, &slots) ||
	    !fsimg_add_u64(base, slots, offset))
		return (erofs_reject(error, FSIMG_ECORRUPT, "inode NID"));
	*space = EROFS_SPACE_PRIMARY;
	return (FSIMG_OK);
}
