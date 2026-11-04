/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "fsimg-endian.h"
#include "hfsplus.h"

/* Pure decoders of the HFS Plus records. A public decoder first checks that
 * the record's span holds every byte it reads, and that the fields that frame
 * the record agree with that span. It then fills its output in full, so a
 * rejected record leaves the output untouched. Static decoders below take a
 * span their caller has already checked.
 *
 * The decoders judge a record's shape, not its values: a block size, a
 * flag word or a fork's extents are the validation of volume.c, catalog.c
 * and fork.c.
 */

static void extent_decode(const unsigned char *, hfsplus_extent_t *);
static void extents_decode(const unsigned char *, hfsplus_extent_t *);
static void fork_decode(const unsigned char *, hfsplus_fork_t *);
static void dates_decode(const unsigned char *, hfsplus_dates_t *);
static void bsd_decode(const unsigned char *, hfsplus_bsd_t *);
static uint32_t journal_load32(const unsigned char *, bool);
static uint64_t journal_load64(const unsigned char *, bool);
static uint32_t journal_checksum(const unsigned char *);

static void
extent_decode(const unsigned char *raw, hfsplus_extent_t *extent)
{
	extent->start = fsimg_load_be32(raw);
	extent->count = fsimg_load_be32(raw + 4);
}

static void
extents_decode(const unsigned char *raw, hfsplus_extent_t *extents)
{
	for (unsigned i = 0; i < HFSPLUS_EXTENTS_PER_RECORD; i++)
		extent_decode(raw + 8 * i, &extents[i]);
}

/* clumpSize, at offset 8, is a hint fsimg ignores. */
static void
fork_decode(const unsigned char *raw, hfsplus_fork_t *fork)
{
	fork->logical_size = fsimg_load_be64(raw);
	fork->total_blocks = fsimg_load_be32(raw + 12);
	extents_decode(raw + 16, fork->extents);
}

static void
dates_decode(const unsigned char *raw, hfsplus_dates_t *dates)
{
	dates->created = fsimg_load_be32(raw);
	dates->content_modified = fsimg_load_be32(raw + 4);
	dates->attribute_modified = fsimg_load_be32(raw + 8);
	dates->accessed = fsimg_load_be32(raw + 12);
	dates->backed_up = fsimg_load_be32(raw + 16);
}

static void
bsd_decode(const unsigned char *raw, hfsplus_bsd_t *bsd)
{
	bsd->owner_id = fsimg_load_be32(raw);
	bsd->group_id = fsimg_load_be32(raw + 4);
	bsd->admin_flags = raw[8];
	bsd->owner_flags = raw[9];
	bsd->file_mode = fsimg_load_be16(raw + 10);
	bsd->special = fsimg_load_be32(raw + 12);
}

bool
hfsplus_header_decode(const unsigned char *raw, size_t length,
    hfsplus_header_t *header)
{
	if (length < HFSPLUS_HEADER_BYTES)
		return (false);

	header->signature = fsimg_load_be16(raw);
	header->version = fsimg_load_be16(raw + 2);
	header->attributes = fsimg_load_be32(raw + 4);
	header->last_mounted_version = fsimg_load_be32(raw + 8);
	header->journal_info_block = fsimg_load_be32(raw + 12);
	header->create_date = fsimg_load_be32(raw + 16);
	header->modify_date = fsimg_load_be32(raw + 20);
	header->backup_date = fsimg_load_be32(raw + 24);
	header->checked_date = fsimg_load_be32(raw + 28);
	header->file_count = fsimg_load_be32(raw + 32);
	header->folder_count = fsimg_load_be32(raw + 36);
	header->block_size = fsimg_load_be32(raw + 40);
	header->total_blocks = fsimg_load_be32(raw + 44);
	header->free_blocks = fsimg_load_be32(raw + 48);
	header->next_allocation = fsimg_load_be32(raw + 52);
	header->rsrc_clump_size = fsimg_load_be32(raw + 56);
	header->data_clump_size = fsimg_load_be32(raw + 60);
	header->next_catalog_id = fsimg_load_be32(raw + 64);
	header->write_count = fsimg_load_be32(raw + 68);
	header->encodings_bitmap = fsimg_load_be64(raw + 72);
	for (unsigned i = 0; i < 8; i++)
		header->finder_info[i] = fsimg_load_be32(raw + 80 + 4 * i);
	fork_decode(raw + 112, &header->allocation_file);
	fork_decode(raw + 192, &header->extents_file);
	fork_decode(raw + 272, &header->catalog_file);
	fork_decode(raw + 352, &header->attributes_file);
	fork_decode(raw + 432, &header->startup_file);
	return (true);
}

bool
hfsplus_fork_decode(const unsigned char *raw, size_t length,
    hfsplus_fork_t *fork)
{
	if (length < HFSPLUS_FORK_BYTES)
		return (false);

	fork_decode(raw, fork);
	return (true);
}

/* Fills HFSPLUS_EXTENTS_PER_RECORD descriptors. */
bool
hfsplus_extents_decode(const unsigned char *raw, size_t length,
    hfsplus_extent_t *extents)
{
	if (length < HFSPLUS_EXTENT_RECORD_BYTES)
		return (false);

	extents_decode(raw, extents);
	return (true);
}

bool
hfsplus_node_decode(const unsigned char *raw, size_t length,
    hfsplus_node_t *node)
{
	if (length < HFSPLUS_NODE_DESCRIPTOR_BYTES)
		return (false);

	node->forward_link = fsimg_load_be32(raw);
	node->backward_link = fsimg_load_be32(raw + 4);
	node->kind = (int8_t)raw[8];
	node->height = raw[9];
	node->records = fsimg_load_be16(raw + 10);
	return (true);
}

bool
hfsplus_btree_decode(const unsigned char *raw, size_t length,
    hfsplus_btree_t *btree)
{
	if (length < HFSPLUS_BTREE_HEADER_BYTES)
		return (false);

	btree->tree_depth = fsimg_load_be16(raw);
	btree->root_node = fsimg_load_be32(raw + 2);
	btree->leaf_records = fsimg_load_be32(raw + 6);
	btree->first_leaf_node = fsimg_load_be32(raw + 10);
	btree->last_leaf_node = fsimg_load_be32(raw + 14);
	btree->node_size = fsimg_load_be16(raw + 18);
	btree->max_key_length = fsimg_load_be16(raw + 20);
	btree->total_nodes = fsimg_load_be32(raw + 22);
	btree->free_nodes = fsimg_load_be32(raw + 26);
	btree->clump_size = fsimg_load_be32(raw + 32);
	btree->btree_type = raw[36];
	btree->key_compare_type = raw[37];
	btree->attributes = fsimg_load_be32(raw + 38);
	return (true);
}

/* keyLength is always 10 and the pad byte zero. */
bool
hfsplus_ekey_decode(const unsigned char *raw, size_t length,
    hfsplus_ekey_t *key)
{
	if (length < HFSPLUS_EKEY_BYTES)
		return (false);
	if (fsimg_load_be16(raw) != HFSPLUS_EKEY_LENGTH || raw[3] != 0)
		return (false);

	key->fork_type = raw[2];
	key->file_id = fsimg_load_be32(raw + 4);
	key->start_block = fsimg_load_be32(raw + 8);
	return (true);
}

/* length is the span the key must fit, normally its whole record. keyLength
 * must equal 6 + 2 * nameLength and the name be at most 255 units; an empty
 * name is a thread's key. The key's units stay in raw.
 */
bool
hfsplus_ckey_decode(const unsigned char *raw, size_t length,
    hfsplus_ckey_t *key)
{
	uint16_t key_length, units;

	if (length < HFSPLUS_CKEY_FIXED_BYTES)
		return (false);

	key_length = fsimg_load_be16(raw);
	units = fsimg_load_be16(raw + 6);
	if (units > HFSPLUS_NAME_UNITS_MAX || key_length != 6u + 2u * units)
		return (false);
	if (length - HFSPLUS_CKEY_FIXED_BYTES < 2u * (size_t)units)
		return (false);

	key->parent = fsimg_load_be32(raw + 2);
	key->units = units;
	key->name = raw + HFSPLUS_CKEY_FIXED_BYTES;
	return (true);
}

bool
hfsplus_catalog_type(const unsigned char *raw, size_t length, uint16_t *type)
{
	if (length < 2)
		return (false);

	*type = fsimg_load_be16(raw);
	return (true);
}

/* A folder record is exactly its 88 bytes. */
bool
hfsplus_folder_decode(const unsigned char *raw, size_t length,
    hfsplus_folder_t *folder)
{
	if (length != HFSPLUS_FOLDER_BYTES ||
	    fsimg_load_be16(raw) != HFSPLUS_RECORD_FOLDER)
		return (false);

	folder->flags = fsimg_load_be16(raw + 2);
	folder->valence = fsimg_load_be32(raw + 4);
	folder->id = fsimg_load_be32(raw + 8);
	dates_decode(raw + 12, &folder->dates);
	bsd_decode(raw + 32, &folder->bsd);
	bcopy(raw + 48, folder->finder_info, sizeof(folder->finder_info));
	folder->text_encoding = fsimg_load_be32(raw + 80);
	folder->folder_count = fsimg_load_be32(raw + 84);
	return (true);
}

/* A file record is exactly its 248 bytes. */
bool
hfsplus_file_decode(const unsigned char *raw, size_t length,
    hfsplus_file_t *file)
{
	if (length != HFSPLUS_FILE_BYTES ||
	    fsimg_load_be16(raw) != HFSPLUS_RECORD_FILE)
		return (false);

	file->flags = fsimg_load_be16(raw + 2);
	file->newest_link = fsimg_load_be32(raw + 4);
	file->id = fsimg_load_be32(raw + 8);
	dates_decode(raw + 12, &file->dates);
	bsd_decode(raw + 32, &file->bsd);
	bcopy(raw + 48, file->finder_info, sizeof(file->finder_info));
	file->text_encoding = fsimg_load_be32(raw + 80);
	fork_decode(raw + 88, &file->data_fork);
	fork_decode(raw + 168, &file->resource_fork);
	return (true);
}

/* A thread record is exactly 10 + 2 * nameLength bytes. The name's units stay
 * in raw.
 */
bool
hfsplus_thread_decode(const unsigned char *raw, size_t length,
    hfsplus_thread_t *thread)
{
	uint16_t type, units;

	if (length < HFSPLUS_THREAD_FIXED_BYTES)
		return (false);

	type = fsimg_load_be16(raw);
	units = fsimg_load_be16(raw + 8);
	if (type != HFSPLUS_RECORD_FOLDER_THREAD &&
	    type != HFSPLUS_RECORD_FILE_THREAD)
		return (false);
	if (units > HFSPLUS_NAME_UNITS_MAX ||
	    length != HFSPLUS_THREAD_FIXED_BYTES + 2u * (size_t)units)
		return (false);

	thread->type = type;
	thread->parent = fsimg_load_be32(raw + 4);
	thread->units = units;
	thread->name = raw + HFSPLUS_THREAD_FIXED_BYTES;
	return (true);
}

/* length is the span the key must fit, as in hfsplus_ckey_decode().
 * keyLength must equal 12 + 2 * nameLength, the name 1 to 127 units.
 */
bool
hfsplus_akey_decode(const unsigned char *raw, size_t length,
    hfsplus_akey_t *key)
{
	uint16_t key_length, units;

	if (length < HFSPLUS_AKEY_FIXED_BYTES)
		return (false);

	key_length = fsimg_load_be16(raw);
	units = fsimg_load_be16(raw + 12);
	if (units == 0 || units > HFSPLUS_ATTR_NAME_UNITS_MAX ||
	    key_length != 12u + 2u * units)
		return (false);
	if (length - HFSPLUS_AKEY_FIXED_BYTES < 2u * (size_t)units)
		return (false);

	key->file_id = fsimg_load_be32(raw + 4);
	key->start_block = fsimg_load_be32(raw + 8);
	key->units = units;
	key->name = raw + HFSPLUS_AKEY_FIXED_BYTES;
	return (true);
}

/* A known type's fields must fit length; an inline record may pad its odd
 * value to the two-byte record alignment. A type this function does not know
 * is returned, with no other field set, for attributes.c to refuse as
 * FSIMG_EUNSUPPORTED, which differs from a malformed record.
 */
bool
hfsplus_attr_decode(const unsigned char *raw, size_t length,
    hfsplus_attr_t *attr)
{
	hfsplus_attr_t decoded;

	if (length < 4)
		return (false);

	bzero(&decoded, sizeof(decoded));
	decoded.type = fsimg_load_be32(raw);
	switch (decoded.type) {
	case HFSPLUS_ATTR_INLINE:
		if (length < HFSPLUS_ATTR_INLINE_FIXED_BYTES)
			return (false);
		decoded.data_size = fsimg_load_be32(raw + 12);
		decoded.data_offset = HFSPLUS_ATTR_INLINE_FIXED_BYTES;
		if (decoded.data_size > length - HFSPLUS_ATTR_INLINE_FIXED_BYTES)
			return (false);
		break;
	case HFSPLUS_ATTR_FORK:
		if (length < HFSPLUS_ATTR_FORK_BYTES)
			return (false);
		fork_decode(raw + 8, &decoded.fork);
		break;
	case HFSPLUS_ATTR_EXTENTS:
		if (length < HFSPLUS_ATTR_EXTENTS_BYTES)
			return (false);
		extents_decode(raw + 8, decoded.extents);
		break;
	default:
		break;
	}
	*attr = decoded;
	return (true);
}

bool
hfsplus_jib_decode(const unsigned char *raw, size_t length, hfsplus_jib_t *jib)
{
	if (length < HFSPLUS_JIB_BYTES)
		return (false);

	jib->flags = fsimg_load_be32(raw);
	jib->offset = fsimg_load_be64(raw + 36);
	jib->size = fsimg_load_be64(raw + 44);
	return (true);
}

static uint32_t
journal_load32(const unsigned char *raw, bool little_endian)
{
	return (little_endian ? fsimg_load_le32(raw) : fsimg_load_be32(raw));
}

static uint64_t
journal_load64(const unsigned char *raw, bool little_endian)
{
	return (little_endian ? fsimg_load_le64(raw) : fsimg_load_be64(raw));
}

/* The sum of the 44 header bytes, the checksum field read as zero: each byte
 * c takes s to (s << 8) ^ (s + c), and the stored value is the complement.
 */
static uint32_t
journal_checksum(const unsigned char *raw)
{
	uint32_t sum;

	sum = 0;
	for (unsigned i = 0; i < HFSPLUS_JOURNAL_BYTES; i++) {
		unsigned char byte;

		byte = i >= 36 && i < 40 ? 0 : raw[i];
		sum = (sum << 8) ^ (sum + byte);
	}
	return (~sum);
}

/* The creator's byte order comes from the magic and endian constants, which
 * must agree; the checksum must match, since fsimg never replays and cannot
 * trust the state fields of a header that fails its own check.
 */
bool
hfsplus_journal_decode(const unsigned char *raw, size_t length,
    hfsplus_journal_t *journal)
{
	const uint32_t magic = 0x4a4e4c78, endian = 0x12345678;
	bool little_endian;

	if (length < HFSPLUS_JOURNAL_BYTES)
		return (false);

	little_endian = fsimg_load_le32(raw) == magic;
	if (journal_load32(raw, little_endian) != magic ||
	    journal_load32(raw + 4, little_endian) != endian)
		return (false);
	if (journal_load32(raw + 36, little_endian) != journal_checksum(raw))
		return (false);

	journal->little_endian = little_endian;
	journal->start = journal_load64(raw + 8, little_endian);
	journal->end = journal_load64(raw + 16, little_endian);
	journal->size = journal_load64(raw + 24, little_endian);
	journal->blhdr_size = journal_load32(raw + 32, little_endian);
	journal->jhdr_size = journal_load32(raw + 40, little_endian);
	return (true);
}

bool
hfsplus_mdb_decode(const unsigned char *raw, size_t length, hfsplus_mdb_t *mdb)
{
	if (length < HFSPLUS_MDB_BYTES)
		return (false);

	mdb->signature = fsimg_load_be16(raw);
	mdb->allocation_block_size = fsimg_load_be32(raw + 0x14);
	mdb->first_block_sector = fsimg_load_be16(raw + 0x1c);
	mdb->embedded_signature = fsimg_load_be16(raw + 0x7c);
	mdb->embedded_start = fsimg_load_be16(raw + 0x7e);
	mdb->embedded_count = fsimg_load_be16(raw + 0x80);
	return (true);
}

/* The bytes 66 70 6d 63, then a little-endian type and uncompressed size,
 * which must fit a signed 64-bit content length. A value may continue past
 * the header with its inline payload.
 */
bool
hfsplus_cmpf_decode(const unsigned char *raw, size_t length,
    hfsplus_cmpf_t *cmpf)
{
	static const unsigned char magic[4] = { 0x66, 0x70, 0x6d, 0x63 };
	uint64_t size;

	if (length < HFSPLUS_CMPF_BYTES || bcmp(raw, magic, sizeof(magic)) != 0)
		return (false);

	size = fsimg_load_le64(raw + 8);
	if (size > (uint64_t)INT64_MAX)
		return (false);

	cmpf->type = fsimg_load_le32(raw + 4);
	cmpf->uncompressed_size = size;
	return (true);
}

bool
hfsplus_rsrc_decode(const unsigned char *raw, size_t length,
    hfsplus_rsrc_t *rsrc)
{
	if (length < HFSPLUS_RSRC_BYTES)
		return (false);

	rsrc->data_offset = fsimg_load_be32(raw);
	rsrc->map_offset = fsimg_load_be32(raw + 4);
	rsrc->data_length = fsimg_load_be32(raw + 8);
	rsrc->map_length = fsimg_load_be32(raw + 12);
	return (true);
}

/* A big-endian resource length, then the little-endian chunk count. */
bool
hfsplus_rsrc_table_decode(const unsigned char *raw, size_t length,
    hfsplus_rsrc_table_t *table)
{
	if (length < HFSPLUS_RSRC_TABLE_BYTES)
		return (false);

	table->resource_length = fsimg_load_be32(raw);
	table->chunk_count = fsimg_load_le32(raw + 4);
	return (true);
}

bool
hfsplus_chunk_pair_decode(const unsigned char *pairs, size_t length,
    uint32_t index, hfsplus_chunk_t *chunk)
{
	uint64_t offset;

	offset = (uint64_t)index * HFSPLUS_CHUNK_PAIR_BYTES;
	if (!fsimg_range_fits(offset, HFSPLUS_CHUNK_PAIR_BYTES, length))
		return (false);

	chunk->offset = fsimg_load_le32(pairs + offset);
	chunk->length = fsimg_load_le32(pairs + offset + 4);
	return (true);
}

bool
hfsplus_chunk_word_decode(const unsigned char *words, size_t length,
    uint32_t index, uint32_t *word)
{
	uint64_t offset;

	offset = (uint64_t)index * HFSPLUS_CHUNK_WORD_BYTES;
	if (!fsimg_range_fits(offset, HFSPLUS_CHUNK_WORD_BYTES, length))
		return (false);

	*word = fsimg_load_le32(words + offset);
	return (true);
}
