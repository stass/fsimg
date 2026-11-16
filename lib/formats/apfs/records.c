/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "apfs.h"
#include "fsimg-endian.h"

/*
 * Record reconciliation (specs/design-apfs-driver.md, "Verification"; the
 * rules are specs/design-apfs.md, "Verification").
 *
 * One cursor scans a volume's filesystem tree in key order. Records share
 * their owner's OID prefix, so one object's records arrive together: an
 * inode (kind 3) with its attributes (4) and sibling links (5), the
 * data-stream ID (6) and file extents (8) of the stream an OID names, and a
 * directory's records (9). Checks local to one owner run as its records
 * arrive and when the next owner begins.
 *
 * A sealed volume's filesystem tree is read through its seal, every node
 * checked against its parent's digest, and holds no file extents: they lie
 * in the sealed file-extent tree, keyed by private ID and offset, the order
 * of the filesystem tree's (owner, kind 8). A second cursor walks that tree
 * beside the first, and each extent is checked where a file-extent record
 * of its owner would stand, so the owner's checks are those of an unsealed
 * volume.
 *
 * Facts that join different owners go to streams in metadata scratch, each
 * sorted once after the scan and merged with its partner:
 *
 *   names	each directory record's (child, parent, sibling ID, type)
 *		and name, merged with inodes, each inode's (number, parent,
 *		count, sibling links, type) and name field, appended in scan
 *		order and so sorted;
 *   links	each sibling link's (sibling ID, inode, parent, name), merged
 *		with tags, the same of each directory record carrying a
 *		sibling ID, and with maps, each sibling map's (sibling ID,
 *		inode);
 *   uses	each data-stream field's and stream attribute's (stream, user,
 *		size, allocated bytes), merged with streams, each stream
 *		found's (stream, has an ID record, reference count, end of its
 *		extents), appended in scan order;
 *   forms	each directory record's (parent, comparison form): adjacent
 *		equal records are names the volume treats as equal.
 *
 * Facts of fixed shape are fact_t records, and a names or inodes fact is
 * followed by its name, empty for an inode without a name field; links,
 * tags and forms carry a name and are byte strings led by big-endian
 * integers, so that they sort as bytes and two of them are equal exactly
 * when their bytes are.
 *
 * Every stream record comes from a record read, so the streams are bounded
 * by the scratch budget, and every join is a sort and a merge: an
 * adversarial directory costs a sort, never a comparison of every pair.
 */
#define ROLE_TREE	"filesystem tree"
#define ROLE_INODE	"inode"
#define ROLE_ATTRIBUTE	"attribute"
#define ROLE_DREC	"directory record"
#define ROLE_SIBLING	"sibling link"
#define ROLE_STREAM	"data stream"
#define ROLE_VOLUME	"volume superblock"

/* Who uses a stream, the second value of a uses record. */
#define USE_INODE		1u
#define USE_ATTRIBUTE		2u

/*
 * A fixed-shape fact: four integers, named per stream, and the type of the
 * names and inodes streams. Facts sort by the integers in order, then the
 * type, so each stream sorts by its first member, its join key.
 */
typedef struct fact {
	union {
		uint64_t value[4];	/* the codec's view */
		struct {
			uint64_t child, parent, sibling, unused;
		} name;
		struct {
			uint64_t number, parent, count, links;
		} inode;
		struct {
			uint64_t stream, user, size, alloced;
		} use;
		struct {
			uint64_t stream, has_record, refcnt, end;
		} found;
		struct {
			uint64_t sibling, inode, unused[2];
		} map;
	};
	uint8_t		type;
} fact_t;

/* A names or inodes record as read: the fact and the name after it. */
typedef struct named {
	fact_t		 fact;
	const unsigned char *name;
	size_t		 name_len;
} named_t;

#define FACT_BYTES	33u
/* A names or inodes record: the fact, then a name of the reader's length. */
#define NAMED_BYTES	(FACT_BYTES + APFS_NAME_MAX_BYTES)
/* A sibling record: sibling ID, inode and parent, then the name. */
#define SIBLING_HEAD	24u
/* The longest byte record: a forms record, the parent and the comparison
 * form's UTF-8; a sibling, names or inodes record is shorter.
 */
#define RECORD_BYTES_MAX (8u + APFS_NAME_KEY_BYTES)
_Static_assert(NAMED_BYTES <= RECORD_BYTES_MAX,
    "a names or inodes record fits a byte record buffer");

/* The records of one owner seen so far. */
typedef struct object {
	uint64_t	oid;
	uint64_t	parent;		/* the inode's */
	uint64_t	links;		/* sibling links under the inode */
	uint64_t	children;	/* directory records under the owner */
	uint64_t	extent_end;	/* logical end of the stream's extents */
	uint32_t	refcnt;		/* the data-stream ID record's */
	int32_t		count;		/* the inode's child or link count */
	uint16_t	name_field_len;	/* in scan_t name_field */
	uint8_t		type;		/* the inode's mode type */
	bool		inode;
	bool		stream_id;
	bool		extents;
} object_t;

/* The inode kinds the volume superblock counts. */
typedef enum {
	COUNT_FILES,
	COUNT_DIRECTORIES,
	COUNT_SYMLINKS,
	COUNT_OTHER,
	COUNTS
} count_kind_t;

typedef struct scan {
	apfs_container_t *container;
	const apfs_fs_t	*fs;
	apfs_records_t	*records;
	apfs_name_form_t *form;
	/* Two byte records of RECORD_BYTES_MAX: the one being built or read
	 * and the one a join compares it with.
	 */
	unsigned char	*record;
	unsigned char	*other;
	/* The current inode's name field, APFS_NAME_MAX_BYTES. */
	unsigned char	*name_field;
	fsimg_stream_t	*names;
	fsimg_stream_t	*inodes;
	fsimg_stream_t	*links;
	fsimg_stream_t	*tags;
	fsimg_stream_t	*maps;
	fsimg_stream_t	*uses;
	fsimg_stream_t	*streams;
	fsimg_stream_t	*forms;
	apfs_map_t	 map;
	apfs_cursor_t	 cursor;
	/* A sealed volume's sealed file-extent tree, whether it stands on a
	 * record, and the records it has visited.
	 */
	apfs_cursor_t	 fext_cursor;
	bool		 fext_found;
	uint64_t	 fext_keys;
	object_t	 object;
	uint64_t	 counts[COUNTS];
	uint64_t	 inode_min;	/* the least ordinary inode number */
	apfs_name_rule_t rule;
	uint32_t	 volume;
	bool		 hashed;
	bool		 sealed;
	bool		 map_open;
	bool		 root;		/* the root directory's inode seen */
} scan_t;

static int		fact_compare(const void *, size_t, const void *, size_t,
			    void *);
static int		bytes_compare(const void *, size_t, const void *, size_t,
			    void *);
static void		fact_encode(unsigned char *, const fact_t *);
static void		fact_decode(const unsigned char *, fact_t *);
static fsimg_status_t	fact_add(fsimg_stream_t *, const fact_t *,
			    fsimg_error_t *);
static fsimg_status_t	fact_next(fsimg_stream_t *, uint64_t *, fact_t *,
			    bool *, fsimg_error_t *);
static fsimg_status_t	bytes_next(fsimg_stream_t *, uint64_t *,
			    unsigned char *, size_t *, bool *, fsimg_error_t *);
static fsimg_status_t	named_add(fsimg_stream_t *, unsigned char *,
			    const fact_t *, const void *, size_t,
			    fsimg_error_t *);
static fsimg_status_t	named_next(fsimg_stream_t *, uint64_t *,
			    unsigned char *, named_t *, bool *, fsimg_error_t *);
static fsimg_status_t	sibling_add(fsimg_stream_t *, unsigned char *,
			    uint64_t, uint64_t, uint64_t, const char *, uint16_t,
			    fsimg_error_t *);
static fsimg_status_t	sort_replace(fsimg_stream_t **, fsimg_compare_fn_t,
			    fsimg_error_t *);
static fsimg_status_t	name_judge(fsimg_status_t, const char *,
			    fsimg_error_t *);
static fsimg_status_t	drec_sibling(const apfs_drec_val_t *, uint64_t *,
			    fsimg_error_t *);
static fsimg_status_t	inode_record(scan_t *, const apfs_jkey_t *,
			    const apfs_record_t *, fsimg_error_t *);
static fsimg_status_t	attribute_record(scan_t *, const apfs_record_t *,
			    fsimg_error_t *);
static fsimg_status_t	sibling_record(scan_t *, const apfs_record_t *,
			    fsimg_error_t *);
static fsimg_status_t	stream_id_record(scan_t *, const apfs_record_t *,
			    fsimg_error_t *);
static fsimg_status_t	extent_check(scan_t *, const apfs_extent_key_t *,
			    const apfs_extent_val_t *, fsimg_error_t *);
static bool		extent_follows(const apfs_extent_key_t *,
			    const apfs_jkey_t *);
static fsimg_status_t	sealed_extents(scan_t *, const apfs_jkey_t *,
			    fsimg_error_t *);
static fsimg_status_t	extent_record(scan_t *, const apfs_record_t *,
			    fsimg_error_t *);
static fsimg_status_t	drec_record(scan_t *, const apfs_record_t *,
			    fsimg_error_t *);
static fsimg_status_t	file_info_record(scan_t *, const apfs_record_t *,
			    fsimg_error_t *);
static fsimg_status_t	map_record(scan_t *, const apfs_jkey_t *,
			    const apfs_record_t *, fsimg_error_t *);
static fsimg_status_t	record_check(scan_t *, const apfs_jkey_t *,
			    const apfs_record_t *, fsimg_error_t *);
static fsimg_status_t	object_end(scan_t *, fsimg_error_t *);
static fsimg_status_t	owner_begin(scan_t *, uint64_t, fsimg_error_t *);
static fsimg_status_t	scan_records(scan_t *, fsimg_error_t *);
static fsimg_status_t	inode_names_check(const fact_t *, uint64_t,
			    uint64_t, fsimg_error_t *);
static bool		inode_identifies(const named_t *, const named_t *);
static fsimg_status_t	names_join(scan_t *, fsimg_error_t *);
static fsimg_status_t	links_join(scan_t *, fsimg_error_t *);
static fsimg_status_t	stream_check(const fact_t *, uint64_t, uint64_t,
			    uint64_t, fsimg_error_t *);
static fsimg_status_t	uses_join(scan_t *, fsimg_error_t *);
static fsimg_status_t	forms_check(scan_t *, fsimg_error_t *);
static fsimg_status_t	counts_check(const scan_t *, fsimg_error_t *);
static fsimg_status_t	scan_open(scan_t *, unsigned char *,
			    fsimg_error_t *);
static void		scan_close(scan_t *);

void
apfs_fext_claim_encode(unsigned char *raw, const apfs_fext_claim_t *claim)
{
	fsimg_store_le64(raw, claim->first);
	fsimg_store_le64(raw + 8, claim->count);
	fsimg_store_le64(raw + 16, claim->stream);
	fsimg_store_le32(raw + 24, claim->volume);
}

void
apfs_fext_claim_decode(const unsigned char *raw, apfs_fext_claim_t *claim)
{
	claim->first = fsimg_load_le64(raw);
	claim->count = fsimg_load_le64(raw + 8);
	claim->stream = fsimg_load_le64(raw + 16);
	claim->volume = fsimg_load_le32(raw + 24);
}

/* Facts order by their values in turn, then kind. */
static int
fact_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument)
{
	const unsigned char *x = a, *y = b;
	uint64_t u, w;

	(void)alen;
	(void)blen;
	(void)argument;
	for (size_t i = 0; i < 4; i++) {
		u = fsimg_load_le64(x + 8 * i);
		w = fsimg_load_le64(y + 8 * i);
		if (u != w)
			return (u < w ? -1 : 1);
	}
	return (x[32] < y[32] ? -1 : x[32] > y[32]);
}

/* Byte records order by their bytes, a prefix first. */
static int
bytes_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument)
{
	int order;

	(void)argument;
	order = memcmp(a, b, alen < blen ? alen : blen);
	if (order != 0)
		return (order);
	return (alen < blen ? -1 : alen > blen);
}

static void
fact_encode(unsigned char *raw, const fact_t *fact)
{
	for (size_t i = 0; i < 4; i++)
		fsimg_store_le64(raw + 8 * i, fact->value[i]);
	raw[32] = fact->type;
}

static void
fact_decode(const unsigned char *raw, fact_t *fact)
{
	for (size_t i = 0; i < 4; i++)
		fact->value[i] = fsimg_load_le64(raw + 8 * i);
	fact->type = raw[32];
}

static fsimg_status_t
fact_add(fsimg_stream_t *stream, const fact_t *fact, fsimg_error_t *error)
{
	unsigned char raw[FACT_BYTES];

	fact_encode(raw, fact);
	return (fsimg_stream_append(stream, raw, sizeof(raw), NULL, error));
}

/* Read the fact at offset into fact, or report through read the end. */
static fsimg_status_t
fact_next(fsimg_stream_t *stream, uint64_t *offset, fact_t *fact, bool *read,
    fsimg_error_t *error)
{
	unsigned char raw[FACT_BYTES];
	size_t length;
	fsimg_status_t status;

	*read = false;
	status = fsimg_stream_next(stream, offset, raw, sizeof(raw), &length,
	    error);
	if (status == FSIMG_DONE)
		return (FSIMG_OK);
	if (status != FSIMG_OK)
		return (status);
	if (length != FACT_BYTES)
		return (apfs_corrupt(error, ROLE_TREE,
		    "scratch record has the wrong size"));
	fact_decode(raw, fact);
	*read = true;
	return (FSIMG_OK);
}

/* Read the byte record at offset into record, or report the end. */
static fsimg_status_t
bytes_next(fsimg_stream_t *stream, uint64_t *offset, unsigned char *record,
    size_t *length, bool *read, fsimg_error_t *error)
{
	fsimg_status_t status;

	*read = false;
	status = fsimg_stream_next(stream, offset, record, RECORD_BYTES_MAX,
	    length, error);
	if (status == FSIMG_DONE)
		return (FSIMG_OK);
	if (status != FSIMG_OK)
		return (status);
	if (*length < 8)
		return (apfs_corrupt(error, ROLE_TREE,
		    "scratch record has the wrong size"));
	*read = true;
	return (FSIMG_OK);
}

/* Append fact followed by name_len bytes of name, built in raw. */
static fsimg_status_t
named_add(fsimg_stream_t *stream, unsigned char *raw, const fact_t *fact,
    const void *name, size_t name_len, fsimg_error_t *error)
{
	fact_encode(raw, fact);
	if (name_len != 0)
		bcopy(name, raw + FACT_BYTES, name_len);
	return (fsimg_stream_append(stream, raw, FACT_BYTES + name_len, NULL,
	    error));
}

/*
 * Read the names or inodes record at offset into raw, decoded into named,
 * whose name points into raw.
 */
static fsimg_status_t
named_next(fsimg_stream_t *stream, uint64_t *offset, unsigned char *raw,
    named_t *named, bool *read, fsimg_error_t *error)
{
	size_t length;
	fsimg_status_t status;

	status = bytes_next(stream, offset, raw, &length, read, error);
	if (status != FSIMG_OK || !*read)
		return (status);
	if (length < FACT_BYTES)
		return (apfs_corrupt(error, ROLE_TREE,
		    "scratch record has the wrong size"));
	fact_decode(raw, &named->fact);
	named->name = raw + FACT_BYTES;
	named->name_len = length - FACT_BYTES;
	return (FSIMG_OK);
}

/* Append the sibling record (sibling, inode, parent, name), built in raw. */
static fsimg_status_t
sibling_add(fsimg_stream_t *stream, unsigned char *raw, uint64_t sibling,
    uint64_t inode, uint64_t parent, const char *name, uint16_t name_len,
    fsimg_error_t *error)
{
	fsimg_store_be64(raw, sibling);
	fsimg_store_be64(raw + 8, inode);
	fsimg_store_be64(raw + 16, parent);
	bcopy(name, raw + SIBLING_HEAD, name_len);
	return (fsimg_stream_append(stream, raw, SIBLING_HEAD + name_len,
	    NULL, error));
}

/* Seal and sort a stream; the sorted copy replaces it. */
static fsimg_status_t
sort_replace(fsimg_stream_t **stream, fsimg_compare_fn_t compare,
    fsimg_error_t *error)
{
	fsimg_stream_t *out = NULL;
	fsimg_status_t status;

	status = fsimg_stream_seal(*stream, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(*stream, compare, NULL, NULL, &out,
		    error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_stream_free(*stream);
	*stream = out;
	return (FSIMG_OK);
}

/* The status apfs_name_form() gives a stored name, with its detail. */
static fsimg_status_t
name_judge(fsimg_status_t status, const char *role, fsimg_error_t *error)
{
	if (status == FSIMG_OK)
		return (FSIMG_OK);
	if (status == FSIMG_EUNSUPPORTED)
		return (apfs_unsupported(error, role,
		    "has a name longer than the reader accepts"));
	return (apfs_corrupt(error, role, "has an invalid name"));
}

/*
 * A directory record's extended fields: the sibling ID, zero without one,
 * at its size; an unknown field is ignored unless it is data-dependent.
 */
static fsimg_status_t
drec_sibling(const apfs_drec_val_t *val, uint64_t *sibling,
    fsimg_error_t *error)
{
	apfs_xfield_t field;

	*sibling = 0;
	for (uint16_t i = 0; i < val->xfields.count; i++) {
		if (!apfs_xfield_get(&val->xfields, i, &field))
			return (apfs_corrupt(error, ROLE_DREC,
			    "extended fields are malformed"));
		if (field.type == APFS_XF_DREC_SIBLING_ID) {
			if (!apfs_xfield_u64(&field, sibling) || *sibling == 0)
				return (apfs_corrupt(error, ROLE_DREC,
				    "sibling ID is malformed"));
		} else if ((field.flags & APFS_XF_DATA_DEPENDENT) != 0)
			return (apfs_unsupported(error, ROLE_DREC,
			    "has an unknown data-dependent field"));
	}
	return (FSIMG_OK);
}

/*
 * An inode opens its owner: its rules as the reader applies them, its kind
 * counted for the superblock, and its data stream's use. A non-directory
 * of several names is listed for the walk of the selected volume.
 */
static fsimg_status_t
inode_record(scan_t *s, const apfs_jkey_t *key, const apfs_record_t *record,
    fsimg_error_t *error)
{
	object_t *o = &s->object;
	apfs_inode_t inode;
	const char *name;
	uint16_t name_len;
	unsigned char raw[8];
	fsimg_status_t status;

	status = apfs_inode_decode(record, &inode, &name, &name_len, error);
	if (status != FSIMG_OK)
		return (status);
	if (inode.val.count < 0)
		return (apfs_corrupt(error, ROLE_INODE, "has a negative count"));
	o->inode = true;
	o->parent = inode.val.parent;
	o->count = inode.val.count;
	o->type = (uint8_t)((inode.val.mode & APFS_MODE_TYPE_MASK) >> 12);
	/* The name field must be one of the inode's names, which the
	 * reader bounds; names_join() compares the two.
	 */
	if (name_len > APFS_NAME_MAX_BYTES)
		return (apfs_unsupported(error, ROLE_INODE,
		    "name field is longer than the reader accepts"));
	if (name_len != 0)
		bcopy(name, s->name_field, name_len);
	o->name_field_len = name_len;
	/* Numbers 0 and 1 are never inodes; the root and the private
	 * directory are directories under inode 1.
	 */
	if (key->oid <= APFS_INO_ROOT_PARENT)
		return (apfs_corrupt(error, ROLE_INODE, "number is reserved"));
	if ((key->oid == APFS_INO_ROOT || key->oid == APFS_INO_PRIVATE) &&
	    (inode.kind != FSIMG_ENTRY_DIRECTORY ||
	    inode.val.parent != APFS_INO_ROOT_PARENT))
		return (apfs_corrupt(error, ROLE_INODE,
		    "root or private directory is not a directory under "
		    "inode 1"));
	if (key->oid == APFS_INO_ROOT)
		s->root = true;
	switch (inode.kind) {
	case FSIMG_ENTRY_FILE:
		s->counts[COUNT_FILES]++;
		break;
	case FSIMG_ENTRY_DIRECTORY:
		/* The superblock counts neither the root nor the private
		 * directory, nor any reserved number.
		 */
		if (key->oid >= APFS_INO_FIRST)
			s->counts[COUNT_DIRECTORIES]++;
		break;
	case FSIMG_ENTRY_SYMLINK:
		s->counts[COUNT_SYMLINKS]++;
		break;
	default:
		s->counts[COUNT_OTHER]++;
		break;
	}
	if (inode.has_dstream) {
		status = fact_add(s->uses, &(fact_t){ .use = {
		    .stream = inode.val.private_id, .user = USE_INODE,
		    .size = inode.size, .alloced = inode.alloced } }, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (s->records->shared == NULL ||
	    inode.kind == FSIMG_ENTRY_DIRECTORY || inode.val.count <= 1)
		return (FSIMG_OK);
	fsimg_store_le64(raw, key->oid);
	return (fsimg_stream_append(s->records->shared, raw, sizeof(raw), NULL,
	    error));
}

/*
 * An attribute of the inode before it: well formed, and a stream
 * attribute's use.
 */
static fsimg_status_t
attribute_record(scan_t *s, const apfs_record_t *record, fsimg_error_t *error)
{
	object_t *o = &s->object;
	apfs_name_key_t key;
	apfs_xattr_val_t val;

	if (!o->inode)
		return (apfs_corrupt(error, ROLE_ATTRIBUTE, "has no inode"));
	if (record->ghost ||
	    !apfs_xattr_key_decode(record->key, record->key_len, &key) ||
	    !apfs_xattr_val_decode(record->val, record->val_len, &val))
		return (apfs_corrupt(error, ROLE_ATTRIBUTE, "is malformed"));
	if ((val.flags & ~APFS_XATTR_FLAGS_KNOWN) != 0)
		return (apfs_unsupported(error, ROLE_ATTRIBUTE,
		    "has unknown flags"));
	if (key.name_len == 0)
		return (apfs_corrupt(error, ROLE_ATTRIBUTE, "has no name"));
	if ((val.flags & APFS_XATTR_DATA_EMBEDDED) != 0 &&
	    val.data_len > APFS_XATTR_INLINE_MAX)
		return (apfs_corrupt(error, ROLE_ATTRIBUTE,
		    "inline data is too long"));
	if ((val.flags & APFS_XATTR_DATA_STREAM) == 0)
		return (FSIMG_OK);
	return (fact_add(s->uses, &(fact_t){ .use = {
	    .stream = val.stream_oid, .user = USE_ATTRIBUTE,
	    .size = val.dstream.size, .alloced = val.dstream.alloced_size } },
	    error));
}

/*
 * A sibling link of the file inode before it, with a name the volume
 * accepts; links_join() finds the directory record it describes.
 */
static fsimg_status_t
sibling_record(scan_t *s, const apfs_record_t *record, fsimg_error_t *error)
{
	object_t *o = &s->object;
	apfs_sibling_key_t key;
	apfs_sibling_val_t val;
	fsimg_status_t status;

	if (!o->inode || o->type == APFS_DT_DIRECTORY)
		return (apfs_corrupt(error, ROLE_SIBLING,
		    "belongs to no file inode"));
	if (record->ghost ||
	    !apfs_sibling_key_decode(record->key, record->key_len, &key) ||
	    !apfs_sibling_val_decode(record->val, record->val_len, &val) ||
	    key.sibling_id == 0)
		return (apfs_corrupt(error, ROLE_SIBLING, "is malformed"));
	status = name_judge(apfs_name_form(s->rule, val.name, val.name_len,
	    s->form), ROLE_SIBLING, error);
	if (status != FSIMG_OK)
		return (status);
	o->links++;
	return (sibling_add(s->links, s->record, key.sibling_id, key.inode,
	    val.parent, val.name, val.name_len, error));
}

static fsimg_status_t
stream_id_record(scan_t *s, const apfs_record_t *record,
    fsimg_error_t *error)
{
	object_t *o = &s->object;
	uint64_t oid;

	if (record->ghost || !apfs_bare_key_decode(record->key,
	    record->key_len, APFS_J_DSTREAM_ID, &oid) ||
	    !apfs_dstream_id_val_decode(record->val, record->val_len,
	    &o->refcnt))
		return (apfs_corrupt(error, ROLE_STREAM,
		    "reference record is malformed"));
	o->stream_id = true;
	return (FSIMG_OK);
}

/*
 * A file extent of the stream the owner names, from either tree: extents
 * tile the stream from offset zero in whole blocks, and a mapped one, whose
 * blocks lie in [1, N), is appended for the allocation merge.
 */
static fsimg_status_t
extent_check(scan_t *s, const apfs_extent_key_t *key,
    const apfs_extent_val_t *val, fsimg_error_t *error)
{
	object_t *o = &s->object;
	uint32_t block_size = s->container->block_size;
	uint64_t block_count = s->container->block_count;
	unsigned char raw[APFS_FEXT_CLAIM_BYTES];
	apfs_fext_claim_t claim;

	if (key->logical != o->extent_end)
		return (apfs_corrupt(error, ROLE_STREAM,
		    "extents do not tile their stream"));
	if (val->length == 0 || val->length % block_size != 0 ||
	    val->length > UINT64_MAX - o->extent_end)
		return (apfs_corrupt(error, ROLE_STREAM,
		    "file extent has an invalid length"));
	if (val->flags != 0)
		return (apfs_unsupported(error, ROLE_STREAM,
		    "file extent has unsupported flags"));
	if (val->crypto_id != 0)
		return (apfs_unsupported(error, ROLE_STREAM,
		    "file extent may be encrypted"));
	o->extent_end += val->length;
	o->extents = true;
	if (val->phys_block == 0)
		return (FSIMG_OK);
	claim = (apfs_fext_claim_t){ .first = val->phys_block,
	    .count = val->length / block_size, .stream = key->private_id,
	    .volume = s->volume };
	if (claim.first >= block_count || claim.count > block_count -
	    claim.first)
		return (apfs_corrupt(error, ROLE_STREAM,
		    "file extent maps blocks outside the container"));
	apfs_fext_claim_encode(raw, &claim);
	return (fsimg_stream_append(s->records->extents, raw, sizeof(raw),
	    NULL, error));
}

/* A file-extent record of the filesystem tree; a sealed one holds none. */
static fsimg_status_t
extent_record(scan_t *s, const apfs_record_t *record, fsimg_error_t *error)
{
	apfs_extent_key_t key;
	apfs_extent_val_t val;

	if (s->sealed)
		return (apfs_corrupt(error, ROLE_TREE,
		    "is sealed and holds a file extent"));
	if (record->ghost ||
	    !apfs_extent_key_decode(record->key, record->key_len, &key) ||
	    !apfs_extent_val_decode(record->val, record->val_len, &val))
		return (apfs_corrupt(error, ROLE_STREAM,
		    "file extent is malformed"));
	return (extent_check(s, &key, &val, error));
}

/*
 * Whether a sealed extent, standing as its owner's (private ID, kind 8)
 * record, sorts after the filesystem tree's record key. Both owners are
 * 60-bit, so the order is the filesystem tree's own; a filesystem tree of a
 * sealed volume holds no kind 8, so the two are never equal.
 */
static bool
extent_follows(const apfs_extent_key_t *extent, const apfs_jkey_t *key)
{
	if (extent->private_id != key->oid)
		return (extent->private_id > key->oid);
	return (key->kind < APFS_J_FILE_EXTENT);
}

/*
 * The sealed file-extent tree's records that sort before the filesystem
 * tree's record key, or every remaining one when key is NULL: each takes
 * the place of its owner's (private ID, kind 8) record, ending the current
 * owner when its private ID differs.
 */
static fsimg_status_t
sealed_extents(scan_t *s, const apfs_jkey_t *key, fsimg_error_t *error)
{
	apfs_record_t record;
	apfs_extent_key_t extent;
	apfs_extent_val_t val;
	fsimg_status_t status = FSIMG_OK;

	while (status == FSIMG_OK && s->fext_found) {
		apfs_cursor_record(&s->fext_cursor, &record);
		if (record.ghost || !apfs_sealed_extent_key_decode(record.key,
		    record.key_len, &extent) ||
		    extent.private_id > APFS_JKEY_OID_MASK ||
		    !apfs_sealed_extent_val_decode(record.val, record.val_len,
		    &val))
			return (apfs_corrupt(error, ROLE_STREAM,
			    "sealed file extent is malformed"));
		if (key != NULL && extent_follows(&extent, key))
			return (FSIMG_OK);
		s->fext_keys++;
		status = owner_begin(s, extent.private_id, error);
		if (status == FSIMG_OK)
			status = extent_check(s, &extent, &val, error);
		if (status == FSIMG_OK)
			status = apfs_cursor_next(&s->fext_cursor,
			    &s->fext_found, error);
	}
	return (status);
}

/*
 * A directory record of the owner: its type, its name's comparison form,
 * a hashed key's stored hash, the reserved-inode rule and its sibling ID;
 * its name reference and its form go to their streams.
 */
static fsimg_status_t
drec_record(scan_t *s, const apfs_record_t *record, fsimg_error_t *error)
{
	object_t *o = &s->object;
	apfs_drec_key_t key;
	apfs_drec_val_t val;
	uint64_t sibling;
	size_t length;
	uint8_t type;
	fsimg_status_t status;

	if (record->ghost ||
	    !apfs_drec_key_decode(record->key, record->key_len, s->hashed,
	    &key) || !apfs_drec_val_decode(record->val, record->val_len, &val))
		return (apfs_corrupt(error, ROLE_DREC, "is malformed"));
	o->children++;
	type = (uint8_t)(val.flags & APFS_DREC_TYPE_MASK);
	if (!apfs_type_kind(type, NULL))
		return (apfs_corrupt(error, ROLE_DREC, "has an invalid type"));
	status = name_judge(apfs_name_form(s->rule, key.name, key.name_len,
	    s->form), ROLE_DREC, error);
	if (status != FSIMG_OK)
		return (status);
	if (s->hashed && key.hash != apfs_name_hash(s->form)) {
		if (!apfs_name_assigned(key.name, key.name_len))
			return (apfs_unsupported(error, ROLE_DREC,
			    "hash of a name unassigned in the reader's Unicode "
			    "differs"));
		return (apfs_corrupt(error, ROLE_DREC,
		    "stored hash differs from its name"));
	}
	/* Inode 1 names the root and the private directory alone, and every
	 * other record names an ordinary inode.
	 */
	if (key.parent == APFS_INO_ROOT_PARENT ? val.child != APFS_INO_ROOT &&
	    val.child != APFS_INO_PRIVATE : val.child < s->inode_min)
		return (apfs_corrupt(error, ROLE_DREC, "names a reserved inode"));
	status = drec_sibling(&val, &sibling, error);
	if (status == FSIMG_OK)
		status = named_add(s->names, s->record, &(fact_t){ .name = {
		    .child = val.child, .parent = key.parent,
		    .sibling = sibling }, .type = type }, key.name,
		    key.name_len, error);
	if (status == FSIMG_OK && sibling != 0)
		status = sibling_add(s->tags, s->record, sibling, val.child,
		    key.parent, key.name, key.name_len, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_store_be64(s->record, key.parent);
	status = apfs_name_key(s->form, (char *)s->record + 8,
	    RECORD_BYTES_MAX - 8, &length);
	if (status != FSIMG_OK)
		return (apfs_corrupt(error, ROLE_DREC, "has an invalid name"));
	return (fsimg_stream_append(s->forms, s->record, 8 + length, NULL,
	    error));
}

static fsimg_status_t
map_record(scan_t *s, const apfs_jkey_t *key, const apfs_record_t *record,
    fsimg_error_t *error)
{
	uint64_t inode;

	if (record->ghost || record->key_len != APFS_JKEY_BYTES ||
	    !apfs_sibling_map_val_decode(record->val, record->val_len, &inode))
		return (apfs_corrupt(error, ROLE_SIBLING,
		    "sibling map is malformed"));
	return (fact_add(s->maps, &(fact_t){ .map = { .sibling = key->oid,
	    .inode = inode } }, error));
}

/*
 * A file-information record, which only a sealed volume holds: it must be
 * well formed, and its hash of the file's content is not checked
 * (specs/design-apfs.md, "Sealed volumes").
 */
static fsimg_status_t
file_info_record(scan_t *s, const apfs_record_t *record,
    fsimg_error_t *error)
{
	apfs_file_info_key_t key;
	apfs_file_info_val_t val;

	if (!s->sealed)
		return (apfs_unsupported(error, ROLE_TREE,
		    "holds records verification does not read"));
	if (record->ghost ||
	    !apfs_file_info_key_decode(record->key, record->key_len, &key) ||
	    !apfs_file_info_val_decode(record->val, record->val_len, &val))
		return (apfs_corrupt(error, ROLE_TREE,
		    "has a malformed file-information record"));
	return (FSIMG_OK);
}

static fsimg_status_t
record_check(scan_t *s, const apfs_jkey_t *key, const apfs_record_t *record,
    fsimg_error_t *error)
{
	apfs_dir_stats_t stats;

	switch (key->kind) {
	case APFS_J_INODE:
		return (inode_record(s, key, record, error));
	case APFS_J_XATTR:
		return (attribute_record(s, record, error));
	case APFS_J_SIBLING_LINK:
		return (sibling_record(s, record, error));
	case APFS_J_DSTREAM_ID:
		return (stream_id_record(s, record, error));
	case APFS_J_FILE_EXTENT:
		return (extent_record(s, record, error));
	case APFS_J_DREC:
		return (drec_record(s, record, error));
	case APFS_J_DIR_STATS:
		if (record->ghost || !apfs_dir_stats_decode(record->val,
		    record->val_len, &stats))
			return (apfs_corrupt(error, ROLE_TREE,
			    "has malformed directory statistics"));
		return (FSIMG_OK);
	case APFS_J_SIBLING_MAP:
		return (map_record(s, key, record, error));
	case APFS_J_FILE_INFO:
		return (file_info_record(s, record, error));
	case APFS_J_CRYPTO_STATE:
		return (apfs_unsupported(error, ROLE_TREE,
		    "holds encryption state"));
	default:
		return (apfs_unsupported(error, ROLE_TREE,
		    "holds records verification does not read"));
	}
}

/*
 * The owner's records are all seen: a directory's child count equals its
 * records, the inode goes to its stream in scan order, and so does the
 * stream the owner's ID and extent records describe. Inode 1, the root's
 * parent, is no inode, yet holds the records naming the root and the
 * private directory.
 */
static fsimg_status_t
object_end(scan_t *s, fsimg_error_t *error)
{
	const object_t *o = &s->object;
	fsimg_status_t status;

	if (o->inode) {
		/* A non-directory's count is its links, and it has no
		 * children.
		 */
		if ((o->type == APFS_DT_DIRECTORY &&
		    (uint64_t)o->count != o->children) ||
		    (o->type != APFS_DT_DIRECTORY && o->children != 0))
			return (apfs_corrupt(error, ROLE_INODE,
			    "child count differs from its directory records"));
		status = named_add(s->inodes, s->record, &(fact_t){ .inode = {
		    .number = o->oid, .parent = o->parent,
		    .count = (uint64_t)o->count, .links = o->links },
		    .type = o->type }, s->name_field, o->name_field_len, error);
		if (status != FSIMG_OK)
			return (status);
	} else if (o->children != 0 && o->oid != APFS_INO_ROOT_PARENT)
		return (apfs_corrupt(error, ROLE_DREC,
		    "lies in no directory"));
	if (!o->stream_id && !o->extents)
		return (FSIMG_OK);
	return (fact_add(s->streams, &(fact_t){ .found = { .stream = o->oid,
	    .has_record = o->stream_id, .refcnt = o->refcnt,
	    .end = o->extent_end } }, error));
}

/* A record of owner oid: when it is a new owner, the current one ends. */
static fsimg_status_t
owner_begin(scan_t *s, uint64_t oid, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (oid == s->object.oid)
		return (FSIMG_OK);
	status = object_end(s, error);
	if (status == FSIMG_OK)
		s->object = (object_t){ .oid = oid };
	return (status);
}

/* The scan: every record in key order, owner by owner. */
static fsimg_status_t
scan_records(scan_t *s, fsimg_error_t *error)
{
	apfs_record_t record;
	apfs_jkey_t key;
	uint64_t keys = 0;
	bool found;
	fsimg_status_t status;

	status = apfs_cursor_first(&s->cursor, &found, error);
	if (status == FSIMG_OK && s->sealed)
		status = apfs_cursor_first(&s->fext_cursor, &s->fext_found,
		    error);
	while (status == FSIMG_OK && found) {
		keys++;
		apfs_cursor_record(&s->cursor, &record);
		if (!apfs_jkey_decode(record.key, record.key_len, &key))
			return (apfs_corrupt(error, ROLE_TREE,
			    "has a malformed key"));
		status = sealed_extents(s, &key, error);
		if (status == FSIMG_OK)
			status = owner_begin(s, key.oid, error);
		if (status == FSIMG_OK)
			status = record_check(s, &key, &record, error);
		if (status == FSIMG_OK)
			status = apfs_cursor_next(&s->cursor, &found, error);
	}
	if (status == FSIMG_OK)
		status = sealed_extents(s, NULL, error);
	if (status == FSIMG_OK)
		status = object_end(s, error);
	if (status == FSIMG_OK)
		status = apfs_cursor_counts_check(&s->cursor, keys, error);
	if (status == FSIMG_OK && s->sealed)
		status = apfs_cursor_counts_check(&s->fext_cursor, s->fext_keys,
		    error);
	return (status);
}

/*
 * One inode against its names: names directory records carrying a sibling
 * ID. Only the root, the private directory and non-directories in the
 * private directory may have none. Every other directory has one name; a
 * file's link count equals its names, and once it has several names or
 * any sibling link, each name has a sibling link and carries a sibling ID;
 * otherwise none does.
 */
static fsimg_status_t
inode_names_check(const fact_t *inode, uint64_t names, uint64_t tagged,
    fsimg_error_t *error)
{
	uint64_t number = inode->inode.number, links = inode->inode.links;

	if (names == 0) {
		if (number == APFS_INO_ROOT || number == APFS_INO_PRIVATE ||
		    (inode->inode.parent == APFS_INO_PRIVATE &&
		    inode->type != APFS_DT_DIRECTORY))
			return (FSIMG_OK);
		return (apfs_corrupt(error, ROLE_INODE,
		    "is named by no directory record"));
	}
	if (inode->type == APFS_DT_DIRECTORY) {
		if (names != 1)
			return (apfs_corrupt(error, ROLE_INODE,
			    "directory has several names"));
		return (FSIMG_OK);
	}
	if (inode->inode.count != names)
		return (apfs_corrupt(error, ROLE_INODE,
		    "link count differs from its names"));
	if (links != 0 || names > 1) {
		if (links != names || tagged != names)
			return (apfs_corrupt(error, ROLE_SIBLING,
			    "links differ from the inode's names"));
	} else if (tagged != 0)
		return (apfs_corrupt(error, ROLE_DREC,
		    "carries a sibling ID its inode has no link for"));
	return (FSIMG_OK);
}

/*
 * Whether the inode's parent and name field identify the name, a directory
 * record of it: the parents are equal, and so are the names unless the
 * inode has no name field.
 */
static bool
inode_identifies(const named_t *inode, const named_t *name)
{
	if (name->fact.name.parent != inode->fact.inode.parent)
		return (false);
	return (inode->name_len == 0 || (inode->name_len == name->name_len &&
	    bcmp(inode->name, name->name, name->name_len) == 0));
}

/*
 * Names sorted by child, merged with the inodes in scan order: every name
 * names an inode of its type, a directory has a name field, and an inode's
 * parent and name field identify one of its names, so a directory's
 * identify its one name. The walk from the root reaches an inode only
 * through a name outside the private directory, so an inode without one,
 * other than the root and the private directory themselves, is recorded as
 * unreached. The inode is read into s->record and each name into s->other.
 */
static fsimg_status_t
names_join(scan_t *s, fsimg_error_t *error)
{
	named_t inode, name;
	uint64_t inodes_at = 0, names_at = 0, names, tagged, number;
	bool have_inode, have_name, identified, reachable;
	fsimg_status_t status;

	status = sort_replace(&s->names, fact_compare, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(s->inodes, error);
	if (status == FSIMG_OK)
		status = named_next(s->names, &names_at, s->other, &name,
		    &have_name, error);
	while (status == FSIMG_OK) {
		status = named_next(s->inodes, &inodes_at, s->record, &inode,
		    &have_inode, error);
		if (status != FSIMG_OK || !have_inode)
			break;
		number = inode.fact.inode.number;
		/* A name before this inode names none: the check after
		 * the loop reports it.
		 */
		if (have_name && name.fact.name.child < number)
			break;
		if (inode.fact.type == APFS_DT_DIRECTORY && inode.name_len == 0)
			return (apfs_corrupt(error, ROLE_INODE,
			    "directory has no name field"));
		names = tagged = 0;
		identified = reachable = false;
		while (status == FSIMG_OK && have_name &&
		    name.fact.name.child == number) {
			if (name.fact.type != inode.fact.type)
				return (apfs_corrupt(error, ROLE_DREC,
				    "type differs from its inode"));
			names++;
			tagged += name.fact.name.sibling != 0;
			identified = identified ||
			    inode_identifies(&inode, &name);
			reachable = reachable ||
			    name.fact.name.parent != APFS_INO_PRIVATE;
			status = named_next(s->names, &names_at, s->other,
			    &name, &have_name, error);
		}
		if (status == FSIMG_OK)
			status = inode_names_check(&inode.fact, names, tagged,
			    error);
		if (status != FSIMG_OK)
			break;
		if (names != 0 && !identified)
			return (apfs_corrupt(error, ROLE_INODE,
			    "parent or name field differs from its names"));
		if (!reachable && number != APFS_INO_ROOT &&
		    number != APFS_INO_PRIVATE) {
			s->records->unreached = true;
			if (inode.fact.type == APFS_DT_DIRECTORY)
				s->records->unreached_directory = true;
		}
	}
	if (status == FSIMG_OK && have_name)
		return (apfs_corrupt(error, ROLE_DREC,
		    "names a missing inode"));
	return (status);
}

/*
 * Sibling links, sorted, merged with the tags of the directory records and
 * with the sibling maps: sibling IDs are distinct, each link equals the
 * tag of its ID, which is the directory record it describes, every tag
 * belongs to a link, each map names its link's inode, and with the
 * hard-link map feature each link has a map. The byte records lead with
 * the sibling ID in big-endian order, so they sort by it.
 */
static fsimg_status_t
links_join(scan_t *s, fsimg_error_t *error)
{
	bool maps_required =
	    (s->fs->features & APFS_FS_FEATURE_HARDLINK_MAPS) != 0;
	fact_t map;
	uint64_t links_at = 0, tags_at = 0, maps_at = 0, sibling;
	uint64_t previous = 0;
	size_t link_len, tag_len;
	bool have_link, have_tag, have_map;
	fsimg_status_t status;

	status = sort_replace(&s->links, bytes_compare, error);
	if (status == FSIMG_OK)
		status = sort_replace(&s->tags, bytes_compare, error);
	if (status == FSIMG_OK)
		status = sort_replace(&s->maps, fact_compare, error);
	if (status == FSIMG_OK)
		status = bytes_next(s->tags, &tags_at, s->other, &tag_len,
		    &have_tag, error);
	if (status == FSIMG_OK)
		status = fact_next(s->maps, &maps_at, &map, &have_map, error);
	while (status == FSIMG_OK) {
		status = bytes_next(s->links, &links_at, s->record, &link_len,
		    &have_link, error);
		if (status != FSIMG_OK || !have_link)
			break;
		/* Sibling IDs are nonzero, so the first link has none
		 * before it.
		 */
		sibling = fsimg_load_be64(s->record);
		if (sibling == previous)
			return (apfs_corrupt(error, ROLE_SIBLING,
			    "sibling ID is repeated"));
		previous = sibling;
		if (have_tag && fsimg_load_be64(s->other) < sibling)
			break;
		if (!have_tag || fsimg_load_be64(s->other) != sibling)
			return (apfs_corrupt(error, ROLE_SIBLING,
			    "names no directory record"));
		if (tag_len != link_len ||
		    bcmp(s->other, s->record, link_len) != 0)
			return (apfs_corrupt(error, ROLE_SIBLING,
			    "differs from its directory record"));
		status = bytes_next(s->tags, &tags_at, s->other, &tag_len,
		    &have_tag, error);
		if (status != FSIMG_OK)
			break;
		if (have_map && map.map.sibling < sibling)
			return (apfs_corrupt(error, ROLE_SIBLING,
			    "sibling map has no sibling link"));
		if (have_map && map.map.sibling == sibling) {
			if (map.map.inode != fsimg_load_be64(s->record + 8))
				return (apfs_corrupt(error, ROLE_SIBLING,
				    "sibling map names another inode"));
			status = fact_next(s->maps, &maps_at, &map, &have_map,
			    error);
		} else if (maps_required)
			return (apfs_corrupt(error, ROLE_SIBLING,
			    "has no sibling map"));
	}
	if (status == FSIMG_OK && have_tag)
		return (apfs_corrupt(error, ROLE_DREC,
		    "carries a sibling ID no sibling link has"));
	if (status == FSIMG_OK && have_map)
		return (apfs_corrupt(error, ROLE_SIBLING,
		    "sibling map has no sibling link"));
	return (status);
}

/*
 * One stream ID's uses against the stream found under it, NULL when the
 * scan found none. An inode stream has a reference record counting its
 * inode users; an attribute stream has one user and no record; a stream
 * found has a user; its extents end within limit, the least of its users'
 * sizes rounded up to blocks or allocated bytes, whichever is larger.
 */
static fsimg_status_t
stream_check(const fact_t *stream, uint64_t inode_uses,
    uint64_t attribute_uses, uint64_t limit, fsimg_error_t *error)
{
	if (inode_uses == 0 && attribute_uses == 0)
		return (apfs_corrupt(error, ROLE_STREAM, "has no user"));
	if (inode_uses != 0 && attribute_uses != 0)
		return (apfs_corrupt(error, ROLE_STREAM,
		    "is used by an inode and an attribute"));
	if (attribute_uses > 1)
		return (apfs_corrupt(error, ROLE_STREAM,
		    "attribute stream has several users"));
	if (attribute_uses == 1 && stream != NULL && stream->found.has_record)
		return (apfs_corrupt(error, ROLE_STREAM,
		    "attribute stream has a reference record"));
	if (inode_uses != 0 && (stream == NULL || !stream->found.has_record ||
	    stream->found.refcnt != inode_uses))
		return (apfs_corrupt(error, ROLE_STREAM,
		    "reference count differs from its inodes"));
	if (stream != NULL && stream->found.end > limit)
		return (apfs_corrupt(error, ROLE_STREAM,
		    "extents end past its size"));
	return (FSIMG_OK);
}

/* Uses sorted by stream, merged with the streams found in scan order. */
static fsimg_status_t
uses_join(scan_t *s, fsimg_error_t *error)
{
	uint64_t block_size = s->container->block_size;
	fact_t use, stream;
	uint64_t uses_at = 0, streams_at = 0, id, inode_uses, attribute_uses;
	uint64_t limit, rounded;
	bool have_use, have_stream, matched;
	fsimg_status_t status;

	status = sort_replace(&s->uses, fact_compare, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(s->streams, error);
	if (status == FSIMG_OK)
		status = fact_next(s->uses, &uses_at, &use, &have_use, error);
	if (status == FSIMG_OK)
		status = fact_next(s->streams, &streams_at, &stream,
		    &have_stream, error);
	while (status == FSIMG_OK && (have_use || have_stream)) {
		if (!have_stream || (have_use &&
		    use.use.stream < stream.found.stream))
			id = use.use.stream;
		else
			id = stream.found.stream;
		inode_uses = attribute_uses = 0;
		limit = UINT64_MAX;
		while (status == FSIMG_OK && have_use && use.use.stream == id) {
			if (use.use.user == USE_INODE)
				inode_uses++;
			else
				attribute_uses++;
			rounded = use.use.size > UINT64_MAX - (block_size - 1) ?
			    UINT64_MAX : (use.use.size + block_size - 1) /
			    block_size * block_size;
			if (use.use.alloced > rounded)
				rounded = use.use.alloced;
			if (rounded < limit)
				limit = rounded;
			status = fact_next(s->uses, &uses_at, &use, &have_use,
			    error);
		}
		if (status != FSIMG_OK)
			break;
		matched = have_stream && stream.found.stream == id;
		status = stream_check(matched ? &stream : NULL, inode_uses,
		    attribute_uses, limit, error);
		if (status == FSIMG_OK && matched)
			status = fact_next(s->streams, &streams_at, &stream,
			    &have_stream, error);
	}
	return (status);
}

/* Forms sorted by parent and form: no two names of a parent are equal. */
static fsimg_status_t
forms_check(scan_t *s, fsimg_error_t *error)
{
	uint64_t at = 0;
	size_t length, previous = 0;
	fsimg_status_t status;

	status = sort_replace(&s->forms, bytes_compare, error);
	while (status == FSIMG_OK) {
		status = fsimg_stream_next(s->forms, &at, s->record,
		    RECORD_BYTES_MAX, &length, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status != FSIMG_OK)
			break;
		if (length < 8)
			return (apfs_corrupt(error, ROLE_TREE,
			    "scratch record has the wrong size"));
		if (previous != 0 && bytes_compare(s->other, previous,
		    s->record, length, NULL) == 0)
			return (apfs_corrupt(error, ROLE_DREC,
			    "directory holds names the volume treats as "
			    "equal"));
		bcopy(s->record, s->other, length);
		previous = length;
	}
	return (status);
}

/* The root exists, and the superblock counts the inodes by kind. */
static fsimg_status_t
counts_check(const scan_t *s, fsimg_error_t *error)
{
	const apfs_fs_t *fs = s->fs;

	if (!s->root)
		return (apfs_corrupt(error, ROLE_TREE, "has no root directory"));
	if (s->counts[COUNT_FILES] != fs->files ||
	    s->counts[COUNT_DIRECTORIES] != fs->directories ||
	    s->counts[COUNT_SYMLINKS] != fs->symlinks ||
	    s->counts[COUNT_OTHER] != fs->other_objects)
		return (apfs_corrupt(error, ROLE_VOLUME,
		    "object counts differ from its inodes"));
	return (FSIMG_OK);
}

/*
 * Open the volume's object map, at the checkpoint's XID like every other
 * structure verification reads, a cursor on the filesystem tree, bound in a
 * sealed volume to its integrity object's root hash, with one on the sealed
 * file-extent tree, the scratch streams and the record buffers.
 */
static fsimg_status_t
scan_open(scan_t *s, unsigned char *window, fsimg_error_t *error)
{
	apfs_container_t *container = s->container;
	fsimg_session_t *session = &container->reader->session;
	fsimg_stream_t **facts[] = { &s->maps, &s->uses, &s->streams };
	fsimg_stream_t **named[] = { &s->names, &s->inodes };
	fsimg_stream_t **bytes[] = { &s->links, &s->tags, &s->forms };
	const apfs_tree_profile_t *profile;
	unsigned char root_hash[APFS_SEAL_HASH_BYTES];
	fsimg_error_t detail;
	fsimg_status_t status;

	s->rule = apfs_volume_rule(s->fs);
	s->sealed = (s->fs->incompat_features & APFS_FS_INCOMPAT_SEALED) != 0;
	s->inode_min = apfs_inode_min(s->fs);
	s->hashed = s->rule != APFS_NAMES_BYTES;
	profile = apfs_fs_tree_profile(s->hashed);
	status = apfs_map_open(container, APFS_MAP_VOLUME, s->fs->omap_oid,
	    container->xid, window, &s->map, error);
	if (status != FSIMG_OK)
		return (status);
	s->map_open = true;
	apfs_cursor_init(&s->cursor, container, profile, APFS_OBJ_VIRTUAL,
	    s->fs->root_tree_oid, container->xid, &s->map);
	if (s->sealed) {
		apfs_cursor_init(&s->fext_cursor, container,
		    apfs_sealed_extent_tree_profile(), APFS_OBJ_PHYSICAL,
		    s->fs->fext_tree_oid, container->xid, NULL);
		status = apfs_integrity_read(container, &s->map, s->fs,
		    container->xid, window, root_hash, error);
		if (status != FSIMG_OK)
			return (status);
		apfs_cursor_seal(&s->cursor, root_hash);
	}
	s->form = fsimg_alloc(container->reader->ctx, sizeof(*s->form),
	    &detail);
	if (s->form != NULL)
		s->record = fsimg_alloc(container->reader->ctx,
		    2 * RECORD_BYTES_MAX + APFS_NAME_MAX_BYTES, &detail);
	if (s->form == NULL || s->record == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	s->other = s->record + RECORD_BYTES_MAX;
	s->name_field = s->other + RECORD_BYTES_MAX;
	for (size_t i = 0; status == FSIMG_OK &&
	    i < sizeof(facts) / sizeof(facts[0]); i++)
		status = fsimg_stream_new(session, FACT_BYTES, facts[i], error);
	for (size_t i = 0; status == FSIMG_OK &&
	    i < sizeof(named) / sizeof(named[0]); i++)
		status = fsimg_stream_new(session, NAMED_BYTES, named[i], error);
	for (size_t i = 0; status == FSIMG_OK &&
	    i < sizeof(bytes) / sizeof(bytes[0]); i++)
		status = fsimg_stream_new(session, RECORD_BYTES_MAX, bytes[i],
		    error);
	return (status);
}

static void
scan_close(scan_t *s)
{
	fsimg_stream_t *streams[] = { s->names, s->inodes, s->links, s->tags,
	    s->maps, s->uses, s->streams, s->forms };

	for (size_t i = 0; i < sizeof(streams) / sizeof(streams[0]); i++)
		fsimg_stream_free(streams[i]);
	fsimg_dealloc(s->record);
	fsimg_dealloc(s->form);
	if (s->map_open) {
		apfs_cursor_free(&s->cursor);
		apfs_cursor_free(&s->fext_cursor);
		apfs_map_close(&s->map);
	}
}

fsimg_status_t
apfs_records_check(apfs_container_t *container, const apfs_fs_t *fs,
    uint32_t volume, unsigned char *window, apfs_records_t *records,
    fsimg_error_t *error)
{
	scan_t *s;
	fsimg_error_t detail;
	fsimg_status_t status;

	s = fsimg_alloc(container->reader->ctx, sizeof(*s), &detail);
	if (s == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	*s = (scan_t){ .container = container, .fs = fs, .records = records,
	    .volume = volume };
	status = scan_open(s, window, error);
	if (status == FSIMG_OK)
		status = scan_records(s, error);
	if (status == FSIMG_OK)
		status = names_join(s, error);
	if (status == FSIMG_OK)
		status = links_join(s, error);
	if (status == FSIMG_OK)
		status = uses_join(s, error);
	if (status == FSIMG_OK)
		status = forms_check(s, error);
	if (status == FSIMG_OK)
		status = counts_check(s, error);
	scan_close(s);
	fsimg_dealloc(s);
	return (status);
}
