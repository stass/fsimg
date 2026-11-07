/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#include <strings.h>

#include "erofs.h"
#include "fsimg-endian.h"

/*
 * Full verification: the common walk, omitted entries included, with every
 * reachable inode recorded twice in metadata scratch, then two sequential
 * passes over the sorted records. Memory holds one record of each stream,
 * so no part of the tree is kept in RAM.
 *
 * References. Every name records the inode it names; every name of a
 * directory also records its parent, which counts the parent's
 * subdirectories. The root records itself as its one name. Sorted by NID,
 * each inode's records are adjacent, and one pass checks that a directory
 * has one name and two plus its subdirectories as its link count, and that
 * a non-directory's link count is its number of names. A record is the NID
 * (u64), the link count of a name (u32), the role (u8), the flags (u8) and
 * two zero bytes.
 *
 * Claims. Every structure the walk reaches claims its bytes of the volume
 * in one of erofs_claim_class_t's classes: the superblock region and the
 * long-prefix table before the walk, then each inode's body, its
 * directory blocks or file data, and its shared attribute entries. Sorted
 * by start, one sweep applies the ownership rule. A record is the start
 * (u64), the length (u64), the owning NID (u64) and the class (u8).
 *
 * Without a visited set a hard-linked inode is reached once per name and
 * claims its body and data each time. Its body claims are identical
 * records of the body class, which sort adjacent, and an inode has one
 * body, so the sweep accepts a repeated body claim by the same owner as
 * that one body again; data may overlap anyway. A directory reached twice
 * fails the reference check, which runs first, so its blocks never need
 * the same allowance. A hard-linked file's content is read once per name,
 * which the session's work budget bounds.
 */
#define REFERENCE_BYTES		16u
#define CLAIM_BYTES		25u
#define PAYLOAD_BUFFER_BYTES	(64u * 1024)

/* A reference names an inode, or counts a subdirectory of its parent. */
#define ROLE_NAME		0u
#define ROLE_SUBDIRECTORY	1u

/* A name reference's flags: what the named inode is. */
#define FLAG_DIRECTORY		0x01u
#define FLAG_COMPACT		0x02u

/* The parent the root's name record names: no entry names the root. */
#define NO_PARENT		UINT64_MAX

/* A compact inode's link count field is 16 bits. */
#define COMPACT_NLINK_MAX	UINT64_C(65535)

/* One reference record, decoded. */
typedef struct reference {
	uint64_t	 nid;
	uint32_t	 nlink;
	uint8_t		 role;
	uint8_t		 flags;
} reference_t;

/* One claim record, decoded; [start, end) lies in the address domain. */
typedef struct claim {
	uint64_t	 start;
	uint64_t	 end;
	uint64_t	 owner;
	erofs_claim_class_t class;
} claim_t;

static int		claim_compare(const void *, size_t, const void *,
			    size_t, void *);
static fsimg_status_t	claim_next(fsimg_reader_t *, fsimg_stream_t *,
			    uint64_t *, uint64_t, claim_t *, bool *,
			    fsimg_error_t *);
static fsimg_status_t	claims_sweep(fsimg_reader_t *, fsimg_stream_t *,
			    fsimg_error_t *);
static int		compare_u64(uint64_t, uint64_t);
static fsimg_status_t	inode_record(fsimg_reader_t *, erofs_verifier_t *,
			    fsimg_stream_t *, const erofs_inode_t *, uint64_t,
			    fsimg_error_t *);
static fsimg_status_t	inode_references_check(uint64_t, uint64_t, uint64_t,
			    uint32_t, uint8_t, fsimg_error_t *);
static fsimg_status_t	overlap(uint64_t, const char *, fsimg_error_t *);
static fsimg_status_t	reference_add(fsimg_stream_t *, const reference_t *,
			    fsimg_error_t *);
static int		reference_compare(const void *, size_t, const void *,
			    size_t, void *);
static fsimg_status_t	reference_next(fsimg_reader_t *, fsimg_stream_t *,
			    uint64_t *, reference_t *, bool *, fsimg_error_t *);
static fsimg_status_t	references_check(fsimg_reader_t *, fsimg_stream_t *,
			    fsimg_error_t *);
static fsimg_status_t	scope_check(const fsimg_erofs_reader_t *,
			    fsimg_error_t *);
static fsimg_status_t	scratch_corrupt(fsimg_error_t *, const char *);
static fsimg_status_t	storage_claim(fsimg_reader_t *, erofs_verifier_t *,
			    const erofs_inode_t *, erofs_claim_class_t,
			    fsimg_error_t *);
static fsimg_status_t	verifier_new(fsimg_reader_t *, erofs_verifier_t **,
			    fsimg_error_t *);
static void		verifier_free(erofs_verifier_t *);

/*
 * Claim [start, start + length) of the primary space for owner. Every
 * caller claims a range it has proved inside the address domain; an
 * empty range claims nothing.
 */
fsimg_status_t
erofs_claim(erofs_verifier_t *verifier, erofs_claim_class_t class,
    uint64_t start, uint64_t length, uint64_t owner, fsimg_error_t *error)
{
	unsigned char record[CLAIM_BYTES];

	if (length == 0)
		return (FSIMG_OK);
	fsimg_store_le64(record, start);
	fsimg_store_le64(record + 8, length);
	fsimg_store_le64(record + 16, owner);
	record[24] = (unsigned char)class;
	return (fsimg_stream_append(verifier->claims, record, sizeof(record),
	    NULL, error));
}

fsimg_status_t
erofs_verify(fsimg_reader_t *reader, unsigned flags,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	const erofs_super_t *sb = &erofs->super;
	erofs_verifier_t *verifier;
	fsimg_stream_t *references, *sorted;
	unsigned char *buffer;
	erofs_inode_t root;
	fsimg_error_t detail;
	fsimg_status_t status;

	verifier = NULL;
	references = sorted = NULL;
	buffer = NULL;
	reader->session.progress.phase = "EROFS verification";
	status = verifier_new(reader, &verifier, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(&reader->session, REFERENCE_BYTES,
		    &references, error);
	if (status != FSIMG_OK)
		goto done;
	if ((flags & FSIMG_VERIFY_PAYLOAD) != 0) {
		buffer = fsimg_alloc(reader->ctx, PAYLOAD_BUFFER_BYTES, &detail);
		if (buffer == NULL) {
			status = detail.status;
			if (error != NULL)
				*error = detail;
			goto done;
		}
	}

	/*
	 * The superblock region: the superblock and its extension slots,
	 * which domain_set() proved inside the domain.
	 */
	status = erofs_claim(verifier, EROFS_CLAIM_EXCLUSIVE, EROFS_SUPER_OFFSET,
	    EROFS_SUPER_BASE_BYTES +
	    (uint64_t)sb->extension_slots * EROFS_SUPER_SLOT_BYTES,
	    EROFS_CLAIM_NO_INODE, error);
	if (status != FSIMG_OK)
		goto done;

	/* The root is named by no entry; it is its own one name. */
	status = erofs_inode_load(reader, sb->root_nid, &root, error);
	if (status == FSIMG_OK)
		status = inode_record(reader, verifier, references, &root,
		    NO_PARENT, error);
	if (status != FSIMG_OK)
		goto done;

	/*
	 * One traversal. Replay made each entry's inode current, with the
	 * directory listing it as its parent.
	 */
	report->directories = 1;
	for (;;) {
		status = fsimg_walk_next(reader, error);
		if (status == FSIMG_DONE)
			break;
		if (status == FSIMG_OK)
			status = fsimg_verify_account(reader, report, error);
		if (status == FSIMG_OK)
			status = inode_record(reader, verifier, references,
			    &erofs->current, erofs->current_parent, error);
		if (status == FSIMG_OK && buffer != NULL &&
		    erofs->current.kind != FSIMG_ENTRY_DIRECTORY)
			status = fsimg_verify_payload(reader, buffer,
			    PAYLOAD_BUFFER_BYTES, report, error);
		if (status != FSIMG_OK)
			goto done;
	}

	/*
	 * Reconcile: references first, so a directory reached twice fails
	 * on a name or link count before its blocks, claimed twice, could
	 * reach the sweep; then the claims.
	 */
	status = fsimg_stream_seal(references, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(references, reference_compare, NULL,
		    NULL, &sorted, error);
	if (status == FSIMG_OK)
		status = references_check(reader, sorted, error);
	fsimg_stream_free(sorted);
	sorted = NULL;
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(verifier->claims, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(verifier->claims, claim_compare, NULL,
		    NULL, &sorted, error);
	if (status == FSIMG_OK)
		status = claims_sweep(reader, sorted, error);
	if (status == FSIMG_OK)
		status = fsimg_reader_stable(reader, error);
	if (status == FSIMG_OK)
		status = scope_check(erofs, error);
done:
	fsimg_stream_free(sorted);
	fsimg_stream_free(references);
	verifier_free(verifier);
	fsimg_dealloc(buffer);
	return (status);
}

/*
 * The verifier: the claim stream and the long prefixes' hashes, the
 * table claimed. *out is set only once both exist.
 */
static fsimg_status_t
verifier_new(fsimg_reader_t *reader, erofs_verifier_t **out,
    fsimg_error_t *error)
{
	erofs_verifier_t *verifier;
	fsimg_error_t detail;
	fsimg_status_t status;

	*out = NULL;
	verifier = fsimg_alloc(reader->ctx, sizeof(*verifier), &detail);
	if (verifier == NULL)
		goto failed;
	bzero(verifier, sizeof(*verifier));
	status = fsimg_stream_new(&reader->session, CLAIM_BYTES,
	    &verifier->claims, error);
	if (status == FSIMG_OK)
		status = erofs_prefixes_load(reader, verifier, error);
	if (status != FSIMG_OK) {
		verifier_free(verifier);
		return (status);
	}
	*out = verifier;
	return (FSIMG_OK);
failed:
	if (error != NULL)
		*error = detail;
	return (detail.status);
}

static void
verifier_free(erofs_verifier_t *verifier)
{
	if (verifier == NULL)
		return;
	fsimg_stream_free(verifier->claims);
	fsimg_dealloc(verifier);
}

/*
 * Record one reached inode, named by an entry of directory parent, or
 * NO_PARENT for the root: its name, a subdirectory of its parent when it
 * is a directory, its body, its storage and its attributes. A directory's
 * blocks are exclusive metadata, and a regular file's and a symbolic
 * link's blocks data; the other kinds store nothing.
 */
static fsimg_status_t
inode_record(fsimg_reader_t *reader, erofs_verifier_t *verifier,
    fsimg_stream_t *references, const erofs_inode_t *inode, uint64_t parent,
    fsimg_error_t *error)
{
	bool directory = inode->kind == FSIMG_ENTRY_DIRECTORY;
	fsimg_status_t status;

	status = reference_add(references, &(reference_t){
		.nid = inode->nid,
		.nlink = inode->nlink,
		.role = ROLE_NAME,
		.flags = (directory ? FLAG_DIRECTORY : 0) |
		    (inode->extended ? 0 : FLAG_COMPACT)
	}, error);
	if (status == FSIMG_OK && directory && parent != NO_PARENT)
		status = reference_add(references, &(reference_t){
			.nid = parent,
			.role = ROLE_SUBDIRECTORY
		}, error);
	if (status != FSIMG_OK)
		return (status);
	/* erofs_inode_load() proved the body inside the domain. */
	status = erofs_claim(verifier, EROFS_CLAIM_BODY, inode->offset,
	    inode->body_bytes, inode->nid, error);
	if (status != FSIMG_OK)
		return (status);
	switch (inode->kind) {
	case FSIMG_ENTRY_DIRECTORY:
		status = storage_claim(reader, verifier, inode,
		    EROFS_CLAIM_EXCLUSIVE, error);
		break;
	case FSIMG_ENTRY_FILE:
	case FSIMG_ENTRY_SYMLINK:
		status = storage_claim(reader, verifier, inode, EROFS_CLAIM_DATA,
		    error);
		break;
	default:
		break;
	}
	if (status != FSIMG_OK)
		return (status);
	return (erofs_xattr_verify(reader, inode, verifier, error));
}

/*
 * Claim every stored extent of inode's content in class, mapped from
 * offset 0 to its size. An extent occupies whole blocks, which erofs_map()
 * proved inside the content domain, so rounding its length up to a block
 * stays inside it. An inline extent lies in the body, already claimed.
 * Each extent is one unit of work.
 */
static fsimg_status_t
storage_claim(fsimg_reader_t *reader, erofs_verifier_t *verifier,
    const erofs_inode_t *inode, erofs_claim_class_t class,
    fsimg_error_t *error)
{
	erofs_extent_t extent;
	uint64_t offset, block_mask;
	fsimg_status_t status;

	block_mask = (uint64_t)erofs_state(reader)->block_bytes - 1;
	for (offset = 0; offset < inode->size;
	    offset = extent.logical_offset + extent.length) {
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = erofs_map(reader, inode, offset, &extent, error);
		if (status != FSIMG_OK)
			return (status);
		if (extent.kind != EROFS_EXTENT_STORED)
			continue;
		status = erofs_claim(verifier, class, extent.storage_offset,
		    (extent.length + block_mask) & ~block_mask, inode->nid,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

static fsimg_status_t
reference_add(fsimg_stream_t *references, const reference_t *reference,
    fsimg_error_t *error)
{
	unsigned char record[REFERENCE_BYTES];

	bzero(record, sizeof(record));
	fsimg_store_le64(record, reference->nid);
	fsimg_store_le32(record + 8, reference->nlink);
	record[12] = reference->role;
	record[13] = reference->flags;
	return (fsimg_stream_append(references, record, sizeof(record), NULL,
	    error));
}

static int
compare_u64(uint64_t a, uint64_t b)
{
	return ((a > b) - (a < b));
}

/* By NID; a malformed length sorts by length, and decoding refuses it. */
static int
reference_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument FSIMG_UNUSED)
{
	if (alen != REFERENCE_BYTES || blen != REFERENCE_BYTES)
		return ((alen > blen) - (alen < blen));
	return (compare_u64(fsimg_load_le64(a), fsimg_load_le64(b)));
}

/*
 * By start, then length, class and owner, so identical claims are
 * adjacent; a malformed length sorts by length, and decoding refuses it.
 */
static int
claim_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument FSIMG_UNUSED)
{
	const unsigned char *x = a, *y = b;
	int order;

	if (alen != CLAIM_BYTES || blen != CLAIM_BYTES)
		return ((alen > blen) - (alen < blen));
	order = compare_u64(fsimg_load_le64(x), fsimg_load_le64(y));
	if (order == 0)
		order = compare_u64(fsimg_load_le64(x + 8),
		    fsimg_load_le64(y + 8));
	if (order == 0)
		order = (x[24] > y[24]) - (x[24] < y[24]);
	if (order == 0)
		order = compare_u64(fsimg_load_le64(x + 16),
		    fsimg_load_le64(y + 16));
	return (order);
}

/* Scratch is untrusted: a record no append wrote fails as corrupt. */
static fsimg_status_t
scratch_corrupt(fsimg_error_t *error, const char *what)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_VERIFY,
	    what));
}

/*
 * The next reference, or *more false at the stream's end. Each record is
 * a unit of work, so a long pass answers cancellation and the time limit.
 */
static fsimg_status_t
reference_next(fsimg_reader_t *reader, fsimg_stream_t *sorted,
    uint64_t *cursor, reference_t *out, bool *more, fsimg_error_t *error)
{
	unsigned char record[REFERENCE_BYTES];
	size_t length;
	fsimg_status_t status;

	*more = false;
	status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_stream_next(sorted, cursor, record, sizeof(record),
	    &length, error);
	if (status == FSIMG_DONE)
		return (FSIMG_OK);
	if (status != FSIMG_OK)
		return (status);
	if (length != sizeof(record) || record[12] > ROLE_SUBDIRECTORY ||
	    (record[13] & ~(FLAG_DIRECTORY | FLAG_COMPACT)) != 0 ||
	    record[14] != 0 || record[15] != 0)
		return (scratch_corrupt(error, "invalid EROFS reference record"));
	*out = (reference_t){
		.nid = fsimg_load_le64(record),
		.nlink = fsimg_load_le32(record + 8),
		.role = record[12],
		.flags = record[13]
	};
	*more = true;
	return (FSIMG_OK);
}

/*
 * One pass over the references sorted by NID: each inode's names and the
 * subdirectories it parents, then its link count checked against them.
 * Every name of one inode records the same count and flags, which scratch
 * read back must show.
 */
static fsimg_status_t
references_check(fsimg_reader_t *reader, fsimg_stream_t *sorted,
    fsimg_error_t *error)
{
	reference_t record;
	uint64_t cursor, nid, names, subdirectories;
	uint32_t nlink;
	uint8_t flags;
	bool more;
	fsimg_status_t status;

	cursor = 0;
	status = reference_next(reader, sorted, &cursor, &record, &more,
	    error);
	while (status == FSIMG_OK && more) {
		nid = record.nid;
		names = subdirectories = 0;
		nlink = 0;
		flags = 0;
		while (status == FSIMG_OK && more && record.nid == nid) {
			if (record.role == ROLE_SUBDIRECTORY) {
				subdirectories++;
			} else if (names == 0) {
				nlink = record.nlink;
				flags = record.flags;
				names = 1;
			} else if (record.nlink != nlink ||
			    record.flags != flags) {
				status = scratch_corrupt(error,
				    "EROFS reference records disagree");
			} else {
				names++;
			}
			if (status != FSIMG_OK)
				break;
			status = reference_next(reader, sorted, &cursor,
			    &record, &more, error);
		}
		if (status == FSIMG_OK && more && record.nid < nid)
			status = scratch_corrupt(error,
			    "EROFS reference records out of order");
		if (status == FSIMG_OK)
			status = inode_references_check(nid, names,
			    subdirectories, nlink, flags, error);
	}
	return (status);
}

/*
 * One inode's references. A directory has exactly one name, the root's
 * being its own, and two plus its subdirectories as its link count:
 * its name, its "." and each child's "..". A compact directory whose count
 * would exceed the 16-bit field records 1 instead, as mkfs.erofs writes
 * and the kernel reports. A non-directory's link count is its number of
 * names, and it parents nothing. Only a named inode is a parent, since
 * the parent of a subdirectory was reached first.
 */
static fsimg_status_t
inode_references_check(uint64_t nid, uint64_t names, uint64_t subdirectories,
    uint32_t nlink, uint8_t flags, fsimg_error_t *error)
{
	char phrase[sizeof(((fsimg_error_t *)NULL)->message)];
	uint64_t expected;

	if (names == 0)
		return (scratch_corrupt(error,
		    "EROFS subdirectory reference without a parent"));
	if ((flags & FLAG_DIRECTORY) == 0) {
		if (subdirectories != 0)
			return (scratch_corrupt(error,
			    "EROFS subdirectory reference to a file"));
		if (nlink == names)
			return (FSIMG_OK);
		snprintf(phrase, sizeof(phrase),
		    "link count %" PRIu32 " of inode %" PRIu64 " for %" PRIu64
		    " names", nlink, nid, names);
		return (erofs_reject(error, FSIMG_ECORRUPT, phrase));
	}
	if (names != 1) {
		snprintf(phrase, sizeof(phrase),
		    "directory inode %" PRIu64 " with %" PRIu64 " names", nid,
		    names);
		return (erofs_reject(error, FSIMG_ECORRUPT, phrase));
	}
	/* The subdirectories are records in the stream, far below 2^64. */
	expected = subdirectories + 2;
	if (nlink == expected ||
	    ((flags & FLAG_COMPACT) != 0 && expected > COMPACT_NLINK_MAX &&
	    nlink == 1))
		return (FSIMG_OK);
	snprintf(phrase, sizeof(phrase),
	    "directory link count %" PRIu32 " of inode %" PRIu64 " for %" PRIu64
	    " subdirectories", nlink, nid, subdirectories);
	return (erofs_reject(error, FSIMG_ECORRUPT, phrase));
}

/*
 * The next claim, or *more false at the stream's end. A record must have
 * a known class, a nonempty range inside the address domain and a start
 * not below previous, the start of the claim before it. Each record is a
 * unit of work, as references are.
 */
static fsimg_status_t
claim_next(fsimg_reader_t *reader, fsimg_stream_t *sorted, uint64_t *cursor,
    uint64_t previous, claim_t *out, bool *more, fsimg_error_t *error)
{
	unsigned char record[CLAIM_BYTES];
	uint64_t start, length;
	size_t transferred;
	fsimg_status_t status;

	*more = false;
	status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_stream_next(sorted, cursor, record, sizeof(record),
	    &transferred, error);
	if (status == FSIMG_DONE)
		return (FSIMG_OK);
	if (status != FSIMG_OK)
		return (status);
	start = fsimg_load_le64(record);
	length = fsimg_load_le64(record + 8);
	if (transferred != sizeof(record) || record[24] > EROFS_CLAIM_DATA ||
	    length == 0 ||
	    !fsimg_range_fits(start, length, erofs_state(reader)->domain_bytes))
		return (scratch_corrupt(error, "invalid EROFS claim record"));
	if (start < previous)
		return (scratch_corrupt(error,
		    "EROFS claim records out of order"));
	*out = (claim_t){
		.start = start,
		.end = start + length,
		.owner = fsimg_load_le64(record + 16),
		.class = (erofs_claim_class_t)record[24]
	};
	*more = true;
	return (FSIMG_OK);
}

static fsimg_status_t
overlap(uint64_t start, const char *what, fsimg_error_t *error)
{
	char phrase[sizeof(((fsimg_error_t *)NULL)->message)];

	snprintf(phrase, sizeof(phrase), "%s at byte %" PRIu64
	    " overlaps other storage", what, start);
	return (erofs_reject(error, FSIMG_ECORRUPT, phrase));
}

/*
 * The ownership rule in one pass over the claims sorted by start. Every
 * claim of a pair that overlaps starts no later than the other, so a
 * claim overlaps an earlier one exactly when it starts below the furthest
 * end the earlier claims of that class reached. Exclusive and body claims
 * share one end, since neither may overlap the other:
 *
 *	exclusive	overlaps no claim of any class
 *	body		the same, except that a claim identical to the
 *			previous body claim, owner included, is that inode's
 *			one body reached by another name
 *	shared		overlaps no exclusive, body or data claim, and a
 *			shared claim only when the two are the same range;
 *			accepted shared claims are disjoint or identical, so
 *			the last one is the only one a later claim can reach
 *	data		overlaps no exclusive, body or shared claim
 */
static fsimg_status_t
claims_sweep(fsimg_reader_t *reader, fsimg_stream_t *sorted,
    fsimg_error_t *error)
{
	claim_t claim, body, shared;
	uint64_t cursor, exclusive_end, shared_end, data_end;
	bool more, have_body;
	fsimg_status_t status;

	cursor = 0;
	exclusive_end = shared_end = data_end = 0;
	bzero(&claim, sizeof(claim));
	bzero(&body, sizeof(body));
	bzero(&shared, sizeof(shared));
	have_body = false;
	for (;;) {
		status = claim_next(reader, sorted, &cursor, claim.start, &claim,
		    &more, error);
		if (status != FSIMG_OK || !more)
			return (status);
		switch (claim.class) {
		case EROFS_CLAIM_BODY:
			if (have_body && claim.start == body.start &&
			    claim.end == body.end && claim.owner == body.owner)
				continue;
			body = claim;
			have_body = true;
			/* FALLTHROUGH */
		case EROFS_CLAIM_EXCLUSIVE:
			if (claim.start < exclusive_end ||
			    claim.start < shared_end || claim.start < data_end)
				return (overlap(claim.start, "metadata", error));
			exclusive_end = claim.end;
			break;
		case EROFS_CLAIM_SHARED:
			if (claim.start < exclusive_end)
				return (overlap(claim.start, "metadata", error));
			if (claim.start < data_end ||
			    (claim.start < shared_end &&
			    (claim.start != shared.start ||
			    claim.end != shared.end)))
				return (overlap(claim.start, "shared attribute",
				    error));
			shared = claim;
			if (claim.end > shared_end)
				shared_end = claim.end;
			break;
		case EROFS_CLAIM_DATA:
			if (claim.start < exclusive_end)
				return (overlap(claim.start, "metadata", error));
			if (claim.start < shared_end)
				return (overlap(claim.start, "shared attribute",
				    error));
			if (claim.end > data_end)
				data_end = claim.end;
			break;
		}
	}
}

/*
 * What verification cannot vouch for, checked once every structure has
 * passed, so a volume that is corrupt as well reports its corruption: a
 * nonzero reserved byte of the superblock region, whose meaning a newer
 * producer may have given it; a compatible bit outside the format's table;
 * and name filters a nonzero superblock filter byte turns off. Each leaves
 * the report incomplete with the reason.
 */
static fsimg_status_t
scope_check(const fsimg_erofs_reader_t *erofs, fsimg_error_t *error)
{
	if (erofs->super.reserved_nonzero || erofs->config.reserved_nonzero)
		return (erofs_reject(error, FSIMG_EUNSUPPORTED,
		    "reserved superblock byte; verification is incomplete"));
	if (erofs->features.unknown_compat)
		return (erofs_reject(error, FSIMG_EUNSUPPORTED,
		    "compatible feature; verification is incomplete"));
	if (erofs->features.xattr_filter && erofs->super.filter_disabled)
		return (erofs_reject(error, FSIMG_EUNSUPPORTED,
		    "disabled name filter; verification is incomplete"));
	return (FSIMG_OK);
}
