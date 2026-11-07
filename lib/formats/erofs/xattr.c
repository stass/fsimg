/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "erofs.h"

/*
 * Extended attributes. The reader delivers no attribute, so at read time it
 * checks only what an entry's own inode holds: the inline region's header
 * and the chain of inline entries up to the region's end, each entry's name
 * index included. An entry that holds any attribute, shared or inline,
 * carries the extra-data mark.
 *
 * Verification checks the structures the region points at as well: every
 * shared index names an entry inside the volume, which it claims as shared
 * metadata; the long-prefix table is well formed and claimed as exclusive
 * metadata; and, when the volume carries name filters, no attribute's name
 * hashes to a bit the inode's filter sets, since a wrong filter makes the
 * kernel hide that attribute.
 *
 * The region follows the fixed inode and is 12 + 4 * (c - 1) bytes for an
 * attribute count c > 0, at most 262148 bytes. It is read in the pieces the
 * walk needs, never whole: the 12-byte header with the shared indexes, then
 * the 4-byte head of each inline entry, which gives the entry's padded
 * length, and in verification each entry's name suffix.
 */

/* The longest name suffix an entry can hold: its length is one byte. */
#define SUFFIX_MAX		255u
/* The header and the most shared indexes a region can hold. */
#define SHARED_INDEXES_BYTES	(EROFS_XATTR_HEADER_BYTES + 4u * 255u)

/* The bytes of an infix absorbed per read and unit of work. */
#define INFIX_CHUNK_BYTES	512u

/*
 * Verification's walk of one region: the verifier the shared entries are
 * claimed in and the long prefixes' hashes come from, whether names are
 * hashed, which they are only on a volume whose filters are checked, and
 * the filter bits the names hashed to.
 */
typedef struct region_scan {
	erofs_verifier_t *verifier;
	uint32_t	 bits;
	bool		 hash;
} region_scan_t;

static bool		filter_checked(const fsimg_erofs_reader_t *);
static fsimg_status_t	infix_absorb(fsimg_reader_t *, uint64_t, uint16_t,
			    erofs_name_hash_t *, fsimg_error_t *);
static fsimg_status_t	inline_entries_walk(fsimg_reader_t *,
			    const erofs_inode_t *, uint32_t, region_scan_t *,
			    bool *, fsimg_error_t *);
static void		name_bit_add(region_scan_t *, const erofs_xattr_t *,
			    const unsigned char *);
static fsimg_status_t	prefix_read(fsimg_reader_t *, uint64_t, size_t, void *,
			    fsimg_error_t *);
static fsimg_status_t	shared_entry_verify(fsimg_reader_t *,
			    const erofs_inode_t *, uint32_t, region_scan_t *,
			    fsimg_error_t *);

/*
 * Validate inode's attribute region and report in *present whether the
 * inode holds an attribute. An inode without a region holds none.
 */
fsimg_status_t
erofs_xattr_check(fsimg_reader_t *reader, const erofs_inode_t *inode,
    bool *present, fsimg_error_t *error)
{
	unsigned char raw[EROFS_XATTR_HEADER_BYTES];
	erofs_xattr_header_t header;
	const char *what;
	bool inline_present;
	fsimg_status_t status;

	*present = false;
	if (inode->xattr_bytes == 0)
		return (FSIMG_OK);

	/*
	 * The header decoder reads the 12 header bytes and checks the
	 * shared indexes against the region's length, which it is given;
	 * the indexes themselves are not read here.
	 */
	status = erofs_body_read(reader, inode, inode->fixed_bytes, sizeof(raw),
	    raw, error);
	if (status != FSIMG_OK)
		return (status);
	status = erofs_xattr_header_decode(raw, inode->xattr_bytes, &header,
	    &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));

	status = inline_entries_walk(reader, inode, header.entries_offset, NULL,
	    &inline_present, error);
	if (status != FSIMG_OK)
		return (status);

	*present = header.shared_count > 0 || inline_present;
	return (FSIMG_OK);
}

/*
 * Verify inode's attributes: the region as erofs_xattr_check() validates
 * it, each shared index's entry inside the volume and claimed, and, when
 * the volume's filters are checked, the inode's filter against the name of
 * every attribute it holds.
 */
fsimg_status_t
erofs_xattr_verify(fsimg_reader_t *reader, const erofs_inode_t *inode,
    erofs_verifier_t *verifier, fsimg_error_t *error)
{
	unsigned char region[SHARED_INDEXES_BYTES];
	erofs_xattr_header_t header;
	region_scan_t scan;
	const char *what;
	uint32_t id;
	bool inline_present;
	fsimg_status_t status;

	if (inode->xattr_bytes == 0)
		return (FSIMG_OK);

	/* The header first, then the shared indexes it proved in the region. */
	status = erofs_body_read(reader, inode, inode->fixed_bytes,
	    EROFS_XATTR_HEADER_BYTES, region, error);
	if (status != FSIMG_OK)
		return (status);
	status = erofs_xattr_header_decode(region, inode->xattr_bytes, &header,
	    &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));
	status = erofs_body_read(reader, inode,
	    (uint64_t)inode->fixed_bytes + EROFS_XATTR_HEADER_BYTES,
	    header.entries_offset - EROFS_XATTR_HEADER_BYTES,
	    region + EROFS_XATTR_HEADER_BYTES, error);
	if (status != FSIMG_OK)
		return (status);

	scan = (region_scan_t){
		.verifier = verifier,
		.hash = filter_checked(erofs_state(reader))
	};
	for (unsigned i = 0; erofs_xattr_shared_id(region, &header, i, &id);
	    i++) {
		status = shared_entry_verify(reader, inode, id, &scan, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = inline_entries_walk(reader, inode, header.entries_offset,
	    &scan, &inline_present, error);
	if (status != FSIMG_OK)
		return (status);

	/* Without hashing no bit is found, so an unchecked filter passes. */
	if ((header.filter & scan.bits) != 0)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "attribute name filter excludes a present attribute"));
	return (FSIMG_OK);
}

/*
 * Load the long-prefix table into the verifier and claim it. The table
 * starts at byte xattr_prefix_start * 4 of its region and holds
 * xattr_prefix_count records, each at least 4 bytes, padded to 4. Its
 * region is the volume under PLAIN_XATTR_PFX, where the claim runs to the
 * last record's end, and otherwise the packed inode's content, which open
 * proved present and whose storage the packed inode's own claims cover.
 * Every record is validated; each of the first 128, the ones an index can
 * name, leaves its name hash in the verifier with its infix absorbed, so
 * an attribute's hash only adds its suffix.
 */
fsimg_status_t
erofs_prefixes_load(fsimg_reader_t *reader, erofs_verifier_t *verifier,
    fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	const erofs_super_t *sb = &erofs->super;
	unsigned char head[4];
	erofs_prefix_t prefix;
	const char *what;
	uint64_t start, offset, remaining, region_bytes;
	fsimg_status_t status;

	if (!erofs->features.long_prefixes || sb->xattr_prefix_count == 0)
		return (FSIMG_OK);
	region_bytes = erofs->features.plain_prefixes ? erofs->domain_bytes :
	    erofs->packed.size;
	/* A 32-bit count of 4-byte units: the start fits 34 bits. */
	start = offset = (uint64_t)sb->xattr_prefix_start * 4;
	for (unsigned k = 0; k < sb->xattr_prefix_count; k++) {
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		remaining = offset < region_bytes ? region_bytes - offset : 0;
		if (remaining < sizeof(head))
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "long prefix truncated"));
		status = prefix_read(reader, offset, sizeof(head), head, error);
		if (status != FSIMG_OK)
			return (status);
		status = erofs_prefix_decode(head,
		    remaining > UINT32_MAX ? UINT32_MAX : (size_t)remaining,
		    &prefix, &what);
		if (status != FSIMG_OK)
			return (erofs_reject(error, status, what));
		if (k < EROFS_LONG_PREFIX_MAX) {
			erofs_name_hash_init(&verifier->prefixes[k],
			    prefix.base_index);
			status = infix_absorb(reader, offset + 3,
			    prefix.infix_length, &verifier->prefixes[k], error);
			if (status != FSIMG_OK)
				return (status);
		}
		/* The decoder proved the record inside the region. */
		offset += prefix.record_bytes;
	}
	if (!erofs->features.plain_prefixes)
		return (FSIMG_OK);
	return (erofs_claim(verifier, EROFS_CLAIM_EXCLUSIVE, start,
	    offset - start, EROFS_CLAIM_NO_INODE, error));
}

/*
 * length bytes at offset of the long-prefix table's region, a range the
 * caller proved inside it: the volume's primary space for a plain table,
 * or the packed inode's content.
 */
static fsimg_status_t
prefix_read(fsimg_reader_t *reader, uint64_t offset, size_t length,
    void *out, fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);

	if (erofs->features.plain_prefixes)
		return (erofs_meta_read(reader, EROFS_SPACE_PRIMARY, offset,
		    length, out, error));
	return (erofs_content_read(reader, &erofs->packed, offset, length,
	    out, NULL, error));
}

/*
 * Absorb length infix bytes at offset of the table's region into hash, a
 * chunk per read and unit of work; the record holding them lies inside the
 * region.
 */
static fsimg_status_t
infix_absorb(fsimg_reader_t *reader, uint64_t offset, uint16_t length,
    erofs_name_hash_t *hash, fsimg_error_t *error)
{
	unsigned char chunk[INFIX_CHUNK_BYTES];
	size_t take;
	fsimg_status_t status;

	for (size_t done = 0; done < length; done += take) {
		take = length - done > sizeof(chunk) ? sizeof(chunk) :
		    length - done;
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = prefix_read(reader, offset + done, take,
			    chunk, error);
		if (status != FSIMG_OK)
			return (status);
		erofs_name_hash_absorb(hash, chunk, take);
	}
	return (FSIMG_OK);
}

/*
 * Whether inodes carry a name filter to check: XATTR_FILTER with the
 * superblock's filter byte zero. A nonzero byte turns the filters off, and
 * verification then reports itself incomplete.
 */
static bool
filter_checked(const fsimg_erofs_reader_t *erofs)
{
	return (erofs->features.xattr_filter && !erofs->super.filter_disabled);
}

/*
 * Walk the inline entries from region offset first to the region's end.
 * Each entry, padded to 4 bytes, must end inside the region, so the walk
 * ends exactly at it; the entry decoder reads the entry's 4-byte head and
 * checks the padded length against what remains of the region. A
 * verification scan that hashes reads and hashes each entry's name too.
 * Each entry is one unit of work, so a region of many tiny entries is
 * charged.
 */
static fsimg_status_t
inline_entries_walk(fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint32_t first, region_scan_t *scan, bool *present, fsimg_error_t *error)
{
	unsigned char head[EROFS_XATTR_ENTRY_BYTES], suffix[SUFFIX_MAX];
	erofs_xattr_t xattr;
	const char *what;
	uint64_t entry;
	uint32_t at, remaining;
	fsimg_status_t status;

	*present = false;
	for (at = first; at < inode->xattr_bytes; at += xattr.entry_bytes) {
		remaining = inode->xattr_bytes - at;
		if (remaining < sizeof(head))
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "attribute entry truncated"));
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		entry = (uint64_t)inode->fixed_bytes + at;
		status = erofs_body_read(reader, inode, entry, sizeof(head),
		    head, error);
		if (status != FSIMG_OK)
			return (status);
		status = erofs_xattr_decode(head, remaining,
		    &erofs_state(reader)->super, &xattr, &what);
		if (status != FSIMG_OK)
			return (erofs_reject(error, status, what));
		*present = true;
		if (scan == NULL || !scan->hash)
			continue;

		/* The decoder proved the suffix inside the region. */
		status = erofs_body_read(reader, inode, entry + sizeof(head),
		    xattr.suffix_length, suffix, error);
		if (status != FSIMG_OK)
			return (status);
		name_bit_add(scan, &xattr, suffix);
	}
	return (FSIMG_OK);
}

/*
 * The shared entry index id names: inside the volume, decoded like an
 * inline entry against what remains of the volume, claimed as shared
 * metadata by inode, and its name hashed when the scan hashes.
 */
static fsimg_status_t
shared_entry_verify(fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint32_t id, region_scan_t *scan, fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	unsigned char head[EROFS_XATTR_ENTRY_BYTES], suffix[SUFFIX_MAX];
	erofs_xattr_t xattr;
	const char *what;
	uint64_t offset, remaining;
	fsimg_status_t status;

	status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	offset = erofs_xattr_shared_offset(&erofs->super, id);
	remaining = offset < erofs->domain_bytes ?
	    erofs->domain_bytes - offset : 0;
	if (remaining < sizeof(head))
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "shared attribute outside the volume"));
	status = erofs_meta_read(reader, EROFS_SPACE_PRIMARY, offset,
	    sizeof(head), head, error);
	if (status != FSIMG_OK)
		return (status);
	status = erofs_xattr_decode(head,
	    remaining > UINT32_MAX ? UINT32_MAX : (size_t)remaining,
	    &erofs->super, &xattr, &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));
	status = erofs_claim(scan->verifier, EROFS_CLAIM_SHARED, offset,
	    xattr.entry_bytes, inode->nid, error);
	if (status != FSIMG_OK || !scan->hash)
		return (status);

	status = erofs_meta_read(reader, EROFS_SPACE_PRIMARY,
	    offset + sizeof(head), xattr.suffix_length, suffix, error);
	if (status != FSIMG_OK)
		return (status);
	name_bit_add(scan, &xattr, suffix);
	return (FSIMG_OK);
}

/*
 * Add the filter bit of an attribute whose stored suffix is suffix: the
 * hash of its long prefix's infix, which the verifier holds, or a fresh
 * hash for a built-in index, continued with the suffix. The entry decoder
 * proved a long-prefix index below the table's count, and its seven bits
 * keep it below EROFS_LONG_PREFIX_MAX.
 */
static void
name_bit_add(region_scan_t *scan, const erofs_xattr_t *xattr,
    const unsigned char *suffix)
{
	erofs_name_hash_t hash;

	if ((xattr->name_index & EROFS_XATTR_LONG_PREFIX) != 0)
		hash = scan->verifier->prefixes[xattr->name_index &
		    ~EROFS_XATTR_LONG_PREFIX];
	else
		erofs_name_hash_init(&hash, xattr->name_index);
	erofs_name_hash_absorb(&hash, suffix, xattr->suffix_length);
	scan->bits |= UINT32_C(1) << erofs_name_hash_bit(&hash);
}
