/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "erofs.h"
#include "fsimg-endian.h"
#include "internal.h"

/*
 * Compressed lookups: the physical cluster of a compressed inode that holds
 * one logical offset, read from the inode's map header and index.
 *
 * The index has one record per logical cluster of B bytes, ceil(size / B)
 * of them. A head or plain record starts a physical cluster at byte
 * clusterofs of its logical cluster; the cluster's extent runs to the start
 * of the next head or plain record, or to the file's size when none
 * follows. A non-head record lies inside the cluster of the nearest head
 * before it, and its backward distance leads back to that head. For
 * B = 4096, records head(0), non-head, head(100) and a size of 10000 give
 * clusters [0, 8292) and [8292, 10000).
 *
 * A lookup of offset o reads record floor(o / B) and walks back to the head
 * of o's cluster: a head starting above o sends it one record back, a
 * non-head back by its backward distance. Every step moves back at least
 * one record, a walk past record 0 is corrupt, and a walk further back than
 * the largest decoded cluster is unsupported, so a walk ends. The cluster's
 * block count comes from the record after its head, which is a count marker
 * when the cluster's type permits big clusters. Its end is the next head or
 * plain record, found by stepping ahead one record at a time, again no
 * further than the largest decoded cluster; every non-head that step meets
 * must lead back into this cluster, so the backward distances agree with
 * the record types too. Records also carry forward
 * distances, which the reader ignores: a forward distance that skipped a
 * head would give the bytes after it one meaning for a read starting before
 * the head and another for a read starting after it, while the record types
 * give every offset one cluster by construction. Each record visited is one
 * unit of work.
 *
 * The file's last cluster may be stored elsewhere than in blocks. Under
 * the inline-tail advice its stored bytes are the inode's inline tail, and
 * under the fragment advice its bytes are a fragment, a range of the packed
 * inode's content; either way its head record's block field names no block
 * and no count marker follows it. A whole-file fragment has no index at
 * all: the whole file is one fragment.
 *
 * Only the records a lookup needs are read. erofs_inode_load() decoded the
 * map header, and the reader keeps the last index unit read, a compact
 * group or up to 16 full records (erofs_zcache_t), so consecutive lookups
 * through one file read each index byte about once.
 *
 * An encoded-extent inode of a 48-bit volume has a list of extents in place
 * of an index, one record per extent, each naming its stored bytes, its
 * format and, by width, its physical and logical byte offsets. A 16- or
 * 32-byte record carries its logical offset, and a lookup binary-searches
 * them: the offsets start at 0, increase strictly and lie below the size,
 * and each record a search reads must lie strictly between the offsets of
 * the records it already read on either side. That makes two lookups agree
 * by construction: where their searches part, at a record of offset x, one
 * continues below x and the other above it, so their extents cannot overlap
 * whatever the records they did not read hold. A 4- or 8-byte record covers
 * one logical cluster, so the record holding o is o's cluster number. An
 * 8-byte record names its physical offset; 4-byte records follow a 64-bit
 * physical start and each extent starts where the previous one ends, so a
 * lookup sums the stored lengths before it, from the last sum the reader
 * kept (erofs_zcache_t), and a sequential read sums each record once. Under
 * the fragment advice the last extent is a fragment of the packed inode.
 * Verification reads every record once, erofs_extents_check().
 */

/* One lookup's view of an inode's index. */
typedef struct zindex {
	fsimg_reader_t	*reader;
	const erofs_inode_t *inode;
	erofs_compact_layout_t compact;		/* a compact index */
	erofs_zheader_t	 header;
	uint64_t	 records;
	uint64_t	 records_offset;	/* in the body */
	unsigned	 blkszbits;
	bool		 big;			/* either big-cluster bit */
} zindex_t;

/* One lookup's view of an encoded-extent inode. */
typedef struct xindex {
	fsimg_reader_t	*reader;
	const erofs_inode_t *inode;
	uint64_t	 records_offset;	/* in the body; record 0 */
	uint64_t	 count;
	unsigned	 extent_bytes;
	erofs_extents_form_t form;
} xindex_t;

static fsimg_status_t	cluster_blocks(zindex_t *, uint64_t,
			    const erofs_zrecord_t *, const erofs_cluster_t *,
			    uint32_t *, fsimg_error_t *);
static fsimg_status_t	cluster_end(zindex_t *, uint64_t, uint64_t,
			    uint64_t *, fsimg_error_t *);
static fsimg_status_t	cluster_kind(const zindex_t *,
			    const erofs_zrecord_t *, erofs_cluster_t *,
			    fsimg_error_t *);
static fsimg_status_t	extent_advance(const erofs_xrecord_t *, uint64_t *,
			    fsimg_error_t *);
static fsimg_status_t	extent_cluster(fsimg_reader_t *, const erofs_inode_t *,
			    uint64_t, erofs_cluster_t *, fsimg_error_t *);
static bool		extent_is_fragment(const xindex_t *, uint64_t);
static fsimg_status_t	extent_load(const xindex_t *, uint64_t,
			    erofs_xrecord_t *, fsimg_error_t *);
static bool		extent_ordered(uint64_t, uint64_t, uint64_t, uint64_t);
static fsimg_status_t	extent_run(const xindex_t *, uint64_t,
			    erofs_xrecord_t *, fsimg_error_t *);
static fsimg_status_t	extent_search(const xindex_t *, uint64_t, uint64_t *,
			    erofs_xrecord_t *, uint64_t *, uint64_t *,
			    fsimg_error_t *);
static fsimg_status_t	extent_start(const xindex_t *, uint64_t *,
			    fsimg_error_t *);
static fsimg_status_t	extent_storage(const xindex_t *, uint64_t,
			    const erofs_xrecord_t *, erofs_cluster_t *,
			    fsimg_error_t *);
static fsimg_status_t	head_find(zindex_t *, uint64_t, uint64_t *,
			    erofs_zrecord_t *, fsimg_error_t *);
static fsimg_status_t	index_cluster(fsimg_reader_t *, const erofs_inode_t *,
			    uint64_t, erofs_cluster_t *, fsimg_error_t *);
static void		index_open(fsimg_reader_t *, const erofs_inode_t *,
			    zindex_t *);
static fsimg_status_t	last_cluster(const zindex_t *, const erofs_zrecord_t *,
			    erofs_cluster_t *, fsimg_error_t *);
static fsimg_status_t	record_load(zindex_t *, uint64_t, erofs_zrecord_t *,
			    fsimg_error_t *);
static uint64_t		record_start(const zindex_t *, uint64_t,
			    const erofs_zrecord_t *);
static fsimg_status_t	unit_load(zindex_t *, const erofs_index_unit_t *,
			    fsimg_error_t *);
static xindex_t		xindex_open(fsimg_reader_t *, const erofs_inode_t *);
static void		zcache_select(fsimg_reader_t *, const erofs_inode_t *);

/*
 * The physical cluster of compressed inode holding logical offset, which
 * erofs_map() proved below the size: its logical extent, where its stored
 * bytes lie and how they decode, or, for a fragment, where in the packed
 * inode its bytes start. The extent contains offset. The caller checks the
 * cluster against its source and the cluster limits.
 */
fsimg_status_t
erofs_zmap(fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint64_t offset, erofs_cluster_t *out, fsimg_error_t *error)
{
	erofs_cluster_t cluster;
	uint64_t end;
	fsimg_status_t status;

	bzero(&cluster, sizeof(cluster));
	if (inode->zheader.whole_fragment) {
		cluster.logical_length = inode->size;
		cluster.source = EROFS_SOURCE_PACKED;
		cluster.storage_offset = inode->zheader.fragment_offset;
		*out = cluster;
		return (FSIMG_OK);
	}

	zcache_select(reader, inode);
	if (inode->zheader.extents_form != EROFS_EXTENTS_NONE)
		status = extent_cluster(reader, inode, offset, &cluster, error);
	else
		status = index_cluster(reader, inode, offset, &cluster, error);
	if (status != FSIMG_OK)
		return (status);
	if (cluster.source == EROFS_SOURCE_PACKED) {
		*out = cluster;
		return (FSIMG_OK);
	}

	end = cluster.logical_offset + cluster.logical_length;
	if (cluster.mode != EROFS_CLUSTER_COMPRESSED &&
	    cluster.logical_length > cluster.stored_bytes)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "plain cluster longer than its blocks"));
	if (cluster.mode == EROFS_CLUSTER_COMPRESSED && !cluster.partial &&
	    cluster.logical_length < cluster.stored_bytes &&
	    end != inode->size)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "compressed cluster stores more than it decodes"));
	*out = cluster;
	return (FSIMG_OK);
}

/*
 * Make the reader's lookup cache inode's: a cache holding another inode's
 * index unit or running sum forgets it.
 */
static void
zcache_select(fsimg_reader_t *reader, const erofs_inode_t *inode)
{
	erofs_zcache_t *cache = &erofs_state(reader)->zcache;

	if (cache->nid != inode->nid) {
		cache->unit_valid = false;
		cache->run_valid = false;
		cache->nid = inode->nid;
	}
}

/*
 * The cluster of an indexed inode holding offset: the head record of its
 * logical cluster, the cluster's end, and where its stored bytes lie.
 */
static fsimg_status_t
index_cluster(fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint64_t offset, erofs_cluster_t *cluster, fsimg_error_t *error)
{
	erofs_zrecord_t head;
	zindex_t index;
	uint64_t end, head_record, start;
	uint32_t blocks;
	fsimg_status_t status;

	index_open(reader, inode, &index);
	status = head_find(&index, offset, &head_record, &head, error);
	if (status != FSIMG_OK)
		return (status);
	start = record_start(&index, head_record, &head);
	status = cluster_end(&index, head_record, start, &end, error);
	if (status != FSIMG_OK)
		return (status);
	if (end <= offset)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "index distances skip a head"));
	cluster->logical_offset = start;
	cluster->logical_length = end - start;

	if (end == inode->size && (inode->zheader.advice &
	    (EROFS_ADVICE_INLINE_TAIL | EROFS_ADVICE_FRAGMENT)) != 0)
		return (last_cluster(&index, &head, cluster, error));

	status = cluster_kind(&index, &head, cluster, error);
	if (status == FSIMG_OK)
		status = cluster_blocks(&index, head_record, &head, cluster,
		    &blocks, error);
	if (status != FSIMG_OK)
		return (status);
	/*
	 * A block number has 32 bits and a count 11, and B is at most 2^12,
	 * so neither product wraps; erofs_map() bounds both.
	 */
	cluster->storage_offset = head.block << index.blkszbits;
	cluster->stored_bytes = blocks << index.blkszbits;
	return (FSIMG_OK);
}

/* Prepare a lookup of inode: where its records lie and how many there are. */
static void
index_open(fsimg_reader_t *reader, const erofs_inode_t *inode,
    zindex_t *index)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	uint64_t header_end;

	/* erofs_inode_decode() measured the body from these same values. */
	*index = (zindex_t){
		.reader = reader,
		.inode = inode,
		.header = inode->zheader,
		.blkszbits = erofs->super.blkszbits,
		.big = (inode->zheader.advice & (EROFS_ADVICE_BIG_HEAD1 |
		    EROFS_ADVICE_BIG_HEAD2)) != 0
	};
	index->records = inode->size >> index->blkszbits;
	if ((inode->size & (erofs->block_bytes - 1)) != 0)
		index->records++;
	header_end = inode->layout_offset + EROFS_ZHEADER_BYTES;
	if (inode->layout == EROFS_LAYOUT_COMPRESSED_FULL) {
		index->records_offset = inode->layout_offset +
		    EROFS_FULL_INDEX_OFFSET;
	} else {
		index->records_offset = header_end;
		erofs_compact_layout(header_end, index->records,
		    &index->compact);
	}
}

/*
 * The file's last cluster, head's, when the inode stores it elsewhere: a
 * fragment of the packed inode at the header's offset, which a full index
 * extends with the head's block field as bits 32 to 63 (source compress.c
 * and zmap.c); or the inline tail, decoded by its record's type like any
 * other cluster. A plain tail on an interlaced inode has no agreed meaning
 * (E15, e15-c-il-*) and is unsupported.
 */
static fsimg_status_t
last_cluster(const zindex_t *index, const erofs_zrecord_t *head,
    erofs_cluster_t *cluster, fsimg_error_t *error)
{
	const erofs_inode_t *inode = index->inode;
	fsimg_status_t status;

	if ((index->header.advice & EROFS_ADVICE_FRAGMENT) != 0) {
		cluster->source = EROFS_SOURCE_PACKED;
		cluster->storage_offset = index->header.fragment_offset;
		if (inode->layout == EROFS_LAYOUT_COMPRESSED_FULL)
			cluster->storage_offset |= head->block << 32;
		return (FSIMG_OK);
	}

	status = cluster_kind(index, head, cluster, error);
	if (status != FSIMG_OK)
		return (status);
	if (cluster->mode == EROFS_CLUSTER_INTERLACED)
		return (erofs_reject(error, FSIMG_EUNSUPPORTED,
		    "plain inline tail on an interlaced inode"));
	/* erofs_inode_load() proved the tail inside the body and a block. */
	cluster->source = EROFS_SOURCE_INLINE;
	cluster->storage_offset = inode->offset + inode->tail_offset;
	cluster->stored_bytes = inode->zheader.tail_bytes;
	return (FSIMG_OK);
}

/*
 * Walk back from the record of offset to the head or plain record whose
 * cluster holds it, returning that record and its number.
 */
static fsimg_status_t
head_find(zindex_t *index, uint64_t offset, uint64_t *head_record,
    erofs_zrecord_t *head, fsimg_error_t *error)
{
	erofs_zrecord_t record;
	uint64_t number = offset >> index->blkszbits;
	uint64_t head_reach = (uint64_t)EROFS_CLUSTER_DECODED_MAX +
	    erofs_state(index->reader)->block_bytes;
	fsimg_status_t status;

	for (;;) {
		/*
		 * A cluster starts less than a block into its head's logical
		 * cluster and decodes to at most the limit, so the head of
		 * offset's cluster lies less than the limit plus a block
		 * before it.
		 */
		if (offset - (number << index->blkszbits) >= head_reach)
			return (erofs_reject(error, FSIMG_EUNSUPPORTED,
			    "decoded cluster above 12 MiB"));
		status = record_load(index, number, &record, error);
		if (status != FSIMG_OK)
			return (status);
		if (record.type != EROFS_RECORD_NONHEAD) {
			if (record_start(index, number, &record) <= offset)
				break;
			if (number == 0)
				return (erofs_reject(error, FSIMG_ECORRUPT,
				    "first index record starts past its "
				    "offset"));
			number--;
			continue;
		}
		if (record.backward > number)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "index backward distance passes record 0"));
		number -= record.backward;
	}
	*head_record = number;
	*head = record;
	return (FSIMG_OK);
}

/*
 * How head's cluster decodes: a plain cluster stored shifted, or rotated
 * by its clusterofs when the inode's plain clusters are interlaced, or a
 * compressed stream of the algorithm the header names for the head's type.
 * A head-2 record needs COMPR_HEAD2, and an algorithm must be configured.
 */
static fsimg_status_t
cluster_kind(const zindex_t *index, const erofs_zrecord_t *head,
    erofs_cluster_t *cluster, fsimg_error_t *error)
{
	const fsimg_erofs_reader_t *erofs = erofs_state(index->reader);
	unsigned algorithm;

	switch (head->type) {
	case EROFS_RECORD_PLAIN:
		if ((index->header.advice & EROFS_ADVICE_INTERLACED) != 0) {
			cluster->mode = EROFS_CLUSTER_INTERLACED;
			cluster->rotation = head->clusterofs;
		} else
			cluster->mode = EROFS_CLUSTER_SHIFTED;
		return (FSIMG_OK);
	case EROFS_RECORD_HEAD1:
		algorithm = index->header.head1_algorithm;
		break;
	case EROFS_RECORD_HEAD2:
		if ((erofs->super.incompat & EROFS_INCOMPAT_DEVICE_TABLE) == 0)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "head-2 record without COMPR_HEAD2"));
		algorithm = index->header.head2_algorithm;
		break;
	case EROFS_RECORD_NONHEAD:
	default:
		return (erofs_reject(error, FSIMG_EINVAL, "index record type"));
	}
	if (algorithm >= EROFS_ALGORITHM_COUNT ||
	    (erofs->config.algorithms & 1u << algorithm) == 0)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "compression algorithm not configured"));
	cluster->mode = EROFS_CLUSTER_COMPRESSED;
	cluster->algorithm = (uint8_t)algorithm;
	cluster->partial = head->partial;
	return (FSIMG_OK);
}

/*
 * The blocks of head's cluster. A cluster is one block unless the advice
 * permits big clusters for its type: head-1 under 0x02, and head-2 and
 * plain under 0x04, as the reference reader pairs them; producers set both
 * bits or 0x02 alone. Then the record after the head, when it is a
 * non-head, must be a count marker, and a head followed by a head, a plain
 * record or no record is one block. A marker where big clusters are not
 * permitted is corrupt, and so is an LZ4 count above the configured
 * maximum, which a configuration without the record or with zero leaves
 * at one block. A marker of zero blocks is corrupt here; it is legal only
 * after the head of an inline tail or a fragment, whose marker nothing
 * reads, since the producer writes the stored length, zero for a fragment,
 * there (mkfs.erofs 1.9.4 with -C16384 -E fragments, e8-lz4-big16k-frag).
 */
static fsimg_status_t
cluster_blocks(zindex_t *index, uint64_t head_record,
    const erofs_zrecord_t *head, const erofs_cluster_t *cluster,
    uint32_t *out, fsimg_error_t *error)
{
	const fsimg_erofs_reader_t *erofs = erofs_state(index->reader);
	erofs_zrecord_t next;
	uint16_t big_advice, lz4_max;
	fsimg_status_t status;

	*out = 1;
	if (head_record + 1 >= index->records)
		return (FSIMG_OK);
	status = record_load(index, head_record + 1, &next, error);
	if (status != FSIMG_OK)
		return (status);
	if (next.type != EROFS_RECORD_NONHEAD)
		return (FSIMG_OK);

	big_advice = head->type == EROFS_RECORD_HEAD1 ?
	    EROFS_ADVICE_BIG_HEAD1 : EROFS_ADVICE_BIG_HEAD2;
	if ((index->header.advice & big_advice) == 0) {
		if (next.marker)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "count marker without big clusters"));
		return (FSIMG_OK);
	}
	if (!next.marker)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "big cluster without a count marker"));
	if (next.marker_blocks == 0)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "count marker of zero blocks"));

	if (cluster->mode == EROFS_CLUSTER_COMPRESSED &&
	    cluster->algorithm == EROFS_ALGORITHM_LZ4) {
		lz4_max = erofs->config.lz4_max_pcluster_blocks;
		if (next.marker_blocks > (lz4_max == 0 ? 1 : lz4_max))
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "cluster blocks above the LZ4 maximum"));
	}
	/* Interlacing rotates within one block; no reader defines more. */
	if (cluster->mode == EROFS_CLUSTER_INTERLACED && next.marker_blocks > 1)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "interlaced plain cluster of several blocks"));
	*out = next.marker_blocks;
	return (FSIMG_OK);
}

/*
 * Where the cluster of the head at head_record, starting at start, ends:
 * at the start of the next head or plain record, or at the size when none
 * follows. A walk already past the decoded limit stops, unsupported, so it
 * visits at most the limit's blocks of records, 3073 for B = 4096.
 *
 * Each non-head on the way must lead back into this cluster: to the head,
 * or to an earlier non-head of the cluster, which leads on to the head in
 * turn, a chain an extent longer than a distance field holds, 2047
 * records, needs. A distance reaching before the head would let
 * a lookup starting at that record find another cluster than this one, so
 * a file's bytes would depend on where a read starts.
 */
static fsimg_status_t
cluster_end(zindex_t *index, uint64_t head_record, uint64_t start,
    uint64_t *end, fsimg_error_t *error)
{
	erofs_zrecord_t record;
	uint64_t number;
	fsimg_status_t status;

	for (number = head_record + 1; number < index->records; number++) {
		if ((number << index->blkszbits) - start >
		    EROFS_CLUSTER_DECODED_MAX)
			return (erofs_reject(error, FSIMG_EUNSUPPORTED,
			    "decoded cluster above 12 MiB"));
		status = record_load(index, number, &record, error);
		if (status != FSIMG_OK)
			return (status);
		if (record.type != EROFS_RECORD_NONHEAD) {
			*end = record_start(index, number, &record);
			return (FSIMG_OK);
		}
		/* number > head_record, so a wrap also fails the test. */
		if (record.backward > number ||
		    number - record.backward < head_record)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "index backward distance leaves its cluster"));
	}
	*end = index->inode->size;
	return (FSIMG_OK);
}

/* The logical byte where head or plain record number starts its cluster. */
static uint64_t
record_start(const zindex_t *index, uint64_t number,
    const erofs_zrecord_t *record)
{
	return ((number << index->blkszbits) + record->clusterofs);
}

/*
 * Record number of the index, below the record count. A head or plain
 * record's clusterofs lies inside its logical cluster, and its cluster
 * starts at or below the size: one starting at the size is an empty
 * cluster ending the previous one, which no lookup reads.
 */
static fsimg_status_t
record_load(zindex_t *index, uint64_t number, erofs_zrecord_t *out,
    fsimg_error_t *error)
{
	erofs_zcache_t *cache = &erofs_state(index->reader)->zcache;
	erofs_index_unit_t unit;
	erofs_zrecord_t record;
	uint64_t first;
	fsimg_status_t status;

	status = fsimg_session_charge(&index->reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);

	if (index->inode->layout == EROFS_LAYOUT_COMPRESSED_FULL) {
		unit = (erofs_index_unit_t){
			.slot = (unsigned)(number % EROFS_FULL_UNIT_RECORDS)
		};
		first = number - unit.slot;
		unit.valid = index->records - first < EROFS_FULL_UNIT_RECORDS ?
		    (unsigned)(index->records - first) :
		    EROFS_FULL_UNIT_RECORDS;
		unit.bytes = unit.valid * EROFS_FULL_RECORD_BYTES;
		unit.offset = first * EROFS_FULL_RECORD_BYTES;
	} else
		erofs_compact_locate(&index->compact, number, &unit);
	status = unit_load(index, &unit, error);
	if (status != FSIMG_OK)
		return (status);
	record = cache->records[unit.slot];

	if (record.type != EROFS_RECORD_NONHEAD) {
		if (record.clusterofs >= erofs_state(index->reader)->block_bytes)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "index clusterofs past its logical cluster"));
		if (record_start(index, number, &record) > index->inode->size)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "index record starts past the size"));
	}
	*out = record;
	return (FSIMG_OK);
}

/*
 * Make the index unit holding a record the cache's unit, decoded: unit
 * describes it, its offset from the first record and its valid records.
 * A full index's unit is up to 16 consecutive records, a compact index's
 * one group. Every record of a unit is validated, so a cache hit for any
 * record gives the verdict a fresh decode would. The cache's unit is
 * cleared before a read and set only once its decode has passed, so a
 * failed load leaves no unit behind.
 */
static fsimg_status_t
unit_load(zindex_t *index, const erofs_index_unit_t *unit,
    fsimg_error_t *error)
{
	erofs_zcache_t *cache = &erofs_state(index->reader)->zcache;
	unsigned char raw[EROFS_FULL_UNIT_RECORDS * EROFS_FULL_RECORD_BYTES];
	uint64_t offset = index->records_offset + unit->offset;
	const char *what;
	fsimg_status_t status;

	if (cache->unit_valid && cache->unit_offset == offset)
		return (FSIMG_OK);
	cache->unit_valid = false;
	status = erofs_body_read(index->reader, index->inode, offset,
	    unit->bytes, raw, error);
	if (status != FSIMG_OK)
		return (status);
	if (index->inode->layout == EROFS_LAYOUT_COMPRESSED_FULL) {
		for (unsigned i = 0; i < unit->valid; i++) {
			status = erofs_full_record_decode(
			    raw + i * EROFS_FULL_RECORD_BYTES, index->big,
			    &cache->records[i], &what);
			if (status != FSIMG_OK)
				break;
		}
	} else
		status = erofs_compact_group_decode(raw, unit, index->big,
		    cache->records, &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));
	cache->unit_offset = offset;
	cache->unit_valid = true;
	return (FSIMG_OK);
}

/*
 * Where the records of an encoded-extent inode, never an indexed one,
 * start in its body: at the map header's end rounded up to the record
 * size. 4-byte records follow a 64-bit physical start placed there.
 */
uint64_t
erofs_extents_offset(const erofs_inode_t *inode)
{
	uint64_t unit = inode->zheader.extent_bytes;

	return ((inode->layout_offset + EROFS_ZHEADER_BYTES + unit - 1) /
	    unit * unit);
}

/*
 * Every record of an encoded-extent inode, read once in order, for full
 * verification: what a lookup checks of the records it reads, applied to
 * all of them, so a record no lookup reaches is checked too. Explicit
 * logical offsets keep extent_ordered() against their neighbours, and
 * 4-byte records' running physical offsets advance as extent_run() advances
 * them. Each record is a unit of work. Nothing to do for an indexed inode.
 */
fsimg_status_t
erofs_extents_check(fsimg_reader_t *reader, const erofs_inode_t *inode,
    fsimg_error_t *error)
{
	erofs_cluster_t cluster;
	erofs_xrecord_t record;
	xindex_t index;
	uint64_t number, physical, previous;
	fsimg_status_t status;

	if (inode->zheader.extents_form == EROFS_EXTENTS_NONE)
		return (FSIMG_OK);
	index = xindex_open(reader, inode);
	physical = 0;
	if (index.form == EROFS_EXTENTS_RUNNING) {
		status = extent_start(&index, &physical, error);
		if (status != FSIMG_OK)
			return (status);
	}
	previous = 0;
	for (number = 0; number < index.count; number++) {
		status = extent_load(&index, number, &record, error);
		if (status != FSIMG_OK)
			return (status);
		bzero(&cluster, sizeof(cluster));
		switch (index.form) {
		case EROFS_EXTENTS_SEARCHED:
			if (!extent_ordered(number, record.logical, previous,
			    inode->size))
				return (erofs_reject(error, FSIMG_ECORRUPT,
				    "encoded extent logical offsets out of "
				    "order"));
			previous = record.logical;
			cluster.logical_offset = record.logical;
			break;
		case EROFS_EXTENTS_RUNNING:
			record.physical = physical;
			if (!extent_is_fragment(&index, number)) {
				status = extent_advance(&record, &physical,
				    error);
				if (status != FSIMG_OK)
					return (status);
			}
			cluster.logical_offset = number <<
			    inode->zheader.lclusterbits;
			break;
		case EROFS_EXTENTS_INDEXED:
			cluster.logical_offset = number <<
			    inode->zheader.lclusterbits;
			break;
		case EROFS_EXTENTS_NONE:
		default:
			return (erofs_reject(error, FSIMG_EINVAL,
			    "encoded-extent form"));
		}
		status = extent_storage(&index, number, &record, &cluster,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* Prepare a lookup of an encoded-extent inode. */
static xindex_t
xindex_open(fsimg_reader_t *reader, const erofs_inode_t *inode)
{
	xindex_t index = {
		.reader = reader,
		.inode = inode,
		.records_offset = erofs_extents_offset(inode),
		.count = inode->zheader.extent_count,
		.extent_bytes = inode->zheader.extent_bytes,
		.form = inode->zheader.extents_form
	};

	if (index.form == EROFS_EXTENTS_RUNNING)
		index.records_offset += EROFS_EXTENT_START_BYTES;
	return (index);
}

/*
 * The cluster of an encoded-extent inode holding offset: the record of its
 * extent, found by a search over explicit logical offsets or as offset's
 * logical cluster, the extent's logical range, and its storage. The last
 * record of a fragment names no volume bytes, so a 4-byte one is loaded
 * without the running sum of its predecessors.
 */
static fsimg_status_t
extent_cluster(fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint64_t offset, erofs_cluster_t *cluster, fsimg_error_t *error)
{
	xindex_t index = xindex_open(reader, inode);
	erofs_xrecord_t record;
	uint64_t end, number, start, unit;
	fsimg_status_t status;

	if (index.form == EROFS_EXTENTS_SEARCHED) {
		status = extent_search(&index, offset, &number, &record,
		    &start, &end, error);
		if (status != FSIMG_OK)
			return (status);
	} else {
		unit = UINT64_C(1) << inode->zheader.lclusterbits;
		number = offset >> inode->zheader.lclusterbits;
		start = number << inode->zheader.lclusterbits;
		end = inode->size - start > unit ? start + unit : inode->size;
		if (index.form == EROFS_EXTENTS_RUNNING &&
		    !extent_is_fragment(&index, number))
			status = extent_run(&index, number, &record, error);
		else
			status = extent_load(&index, number, &record, error);
		if (status != FSIMG_OK)
			return (status);
	}
	cluster->logical_offset = start;
	cluster->logical_length = end - start;
	return (extent_storage(&index, number, &record, cluster, error));
}

/*
 * Whether record number of a file with explicit logical offsets keeps
 * their order given the offsets bracketing it, below and above: only
 * record 0 starts at 0, and every other offset lies strictly between its
 * brackets. The brackets are the neighbouring records' offsets, or 0 below
 * and the size above where none is known.
 */
static bool
extent_ordered(uint64_t number, uint64_t logical, uint64_t below,
    uint64_t above)
{
	if (number == 0)
		return (logical == 0 && logical < above);
	return (logical > below && logical < above);
}

/*
 * The record of explicit logical offsets holding offset, which is below
 * the size, by binary search: its number, its contents, and its extent
 * [start, end), end being the next record's offset or the size. low and
 * high bracket the search, every record before low starting at or below
 * offset and every record from high on above it, with low_logical and
 * high_logical the offsets of the records just outside the bracket, 0 and
 * the size before any is read. A record read must keep extent_ordered()
 * between them. Record 0 starts at 0, at or below offset, so the search
 * ends with low at least 1: the count is at least 1 once the file has
 * content.
 */
static fsimg_status_t
extent_search(const xindex_t *index, uint64_t offset, uint64_t *number,
    erofs_xrecord_t *record, uint64_t *start, uint64_t *end,
    fsimg_error_t *error)
{
	erofs_xrecord_t probe;
	uint64_t high, high_logical, low, low_logical, mid;
	fsimg_status_t status;

	low = 0;
	low_logical = 0;
	high = index->count;
	high_logical = index->inode->size;
	while (low < high) {
		mid = low + (high - low) / 2;
		status = extent_load(index, mid, &probe, error);
		if (status != FSIMG_OK)
			return (status);
		if (!extent_ordered(mid, probe.logical, low_logical,
		    high_logical))
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "encoded extent logical offsets out of order"));
		if (probe.logical <= offset) {
			low = mid + 1;
			low_logical = probe.logical;
			*record = probe;
		} else {
			high = mid;
			high_logical = probe.logical;
		}
	}
	if (low == 0)
		return (erofs_reject(error, FSIMG_EINVAL,
		    "no encoded extent holds the offset"));
	*number = low - 1;
	*start = low_logical;
	*end = high_logical;
	return (FSIMG_OK);
}

/*
 * The 4-byte record number, not a fragment, with its physical offset: the
 * start plus every stored length before it, summed from the reader's last
 * sum for this inode when that lies at or before number, else from record
 * 0. The cache is made this inode's first, so a sum never crosses inodes,
 * and it keeps a sum only once the sum is complete.
 */
static fsimg_status_t
extent_run(const xindex_t *index, uint64_t number, erofs_xrecord_t *record,
    fsimg_error_t *error)
{
	erofs_zcache_t *cache = &erofs_state(index->reader)->zcache;
	erofs_xrecord_t step;
	uint64_t at, physical;
	fsimg_status_t status;

	zcache_select(index->reader, index->inode);
	if (cache->run_valid && cache->run_record <= number) {
		at = cache->run_record;
		physical = cache->run_physical;
	} else {
		at = 0;
		status = extent_start(index, &physical, error);
		if (status != FSIMG_OK)
			return (status);
	}
	cache->run_valid = false;
	for (; at < number; at++) {
		status = extent_load(index, at, &step, error);
		if (status == FSIMG_OK)
			status = extent_advance(&step, &physical, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = extent_load(index, number, record, error);
	if (status != FSIMG_OK)
		return (status);
	record->physical = physical;
	cache->run_record = number;
	cache->run_physical = physical;
	cache->run_valid = true;
	return (FSIMG_OK);
}

/*
 * The physical offset after a 4-byte record that is not a fragment:
 * physical, where the record starts, plus its stored bytes, which may not
 * wrap.
 */
static fsimg_status_t
extent_advance(const erofs_xrecord_t *record, uint64_t *physical,
    fsimg_error_t *error)
{
	erofs_xlength_t length;
	const char *what;
	fsimg_status_t status;

	status = erofs_xlength_decode(record->length_word, &length, &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));
	if (!fsimg_add_u64(*physical, length.stored_bytes, physical))
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "encoded extent physical offsets wrap"));
	return (FSIMG_OK);
}

/* The 64-bit physical start that precedes 4-byte records. */
static fsimg_status_t
extent_start(const xindex_t *index, uint64_t *physical, fsimg_error_t *error)
{
	unsigned char raw[EROFS_EXTENT_START_BYTES];
	fsimg_status_t status;

	status = erofs_body_read(index->reader, index->inode,
	    index->records_offset - EROFS_EXTENT_START_BYTES, sizeof(raw), raw,
	    error);
	if (status == FSIMG_OK)
		*physical = fsimg_load_le64(raw);
	return (status);
}

/*
 * Record number, below the count, read and decoded; each is a unit of
 * work. erofs_inode_load() proved every record inside the body, so the
 * read cannot leave it.
 */
static fsimg_status_t
extent_load(const xindex_t *index, uint64_t number, erofs_xrecord_t *record,
    fsimg_error_t *error)
{
	unsigned char raw[EROFS_EXTENT_BYTES_MAX];
	const char *what;
	fsimg_status_t status;

	status = fsimg_session_charge(&index->reader->session, 0, 0, 1, error);
	if (status == FSIMG_OK)
		status = erofs_body_read(index->reader, index->inode,
		    index->records_offset + number * index->extent_bytes,
		    index->extent_bytes, raw, error);
	if (status != FSIMG_OK)
		return (status);
	status = erofs_xrecord_decode(raw, index->extent_bytes, record, &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));
	return (FSIMG_OK);
}

/* Whether record number is the fragment: the last, under the advice. */
static bool
extent_is_fragment(const xindex_t *index, uint64_t number)
{
	return (number + 1 == index->count &&
	    (index->inode->zheader.advice & EROFS_ADVICE_FRAGMENT) != 0);
}

/*
 * Where record number's extent is stored. A fragment lies in the packed
 * inode, at the offset whose low 32 bits are the length word and whose
 * high 32 bits are the physical offset, which the 4-byte form lacks (E10,
 * e10-48-frag); a physical offset that does not fit 32 bits is corrupt.
 * Any other extent's length word gives its stored bytes from its physical
 * offset in the volume, and its format: a compressed stream of a
 * configured algorithm, or plain. A plain extent is stored shifted, or
 * under the interlaced advice, when its physical offset is a block
 * multiple, rotated by its logical start within a block, as the kernel
 * decides it per extent (source zmap.c; mkfs.erofs sets the advice,
 * e10-48-frag). Interlacing rotates within one block, as on an index, so
 * an interlaced extent of another length is corrupt.
 * cluster->logical_offset is the extent's start.
 */
static fsimg_status_t
extent_storage(const xindex_t *index, uint64_t number,
    const erofs_xrecord_t *record, erofs_cluster_t *cluster,
    fsimg_error_t *error)
{
	const fsimg_erofs_reader_t *erofs = erofs_state(index->reader);
	erofs_xlength_t length;
	uint64_t block_mask, high;
	const char *what;
	unsigned algorithm;
	fsimg_status_t status;

	if (extent_is_fragment(index, number)) {
		high = index->form == EROFS_EXTENTS_RUNNING ? 0 :
		    record->physical;
		if (high > UINT32_MAX)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "encoded fragment offset above 64 bits"));
		cluster->source = EROFS_SOURCE_PACKED;
		cluster->storage_offset = high << 32 | record->length_word;
		return (FSIMG_OK);
	}

	status = erofs_xlength_decode(record->length_word, &length, &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));
	cluster->source = EROFS_SOURCE_BLOCKS;
	cluster->storage_offset = record->physical;
	cluster->stored_bytes = length.stored_bytes;
	block_mask = (uint64_t)erofs->block_bytes - 1;
	if (length.format == 0 &&
	    (index->inode->zheader.advice & EROFS_ADVICE_INTERLACED) != 0 &&
	    (record->physical & block_mask) == 0) {
		if (length.stored_bytes != erofs->block_bytes)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "interlaced plain extent not one block"));
		cluster->mode = EROFS_CLUSTER_INTERLACED;
		cluster->rotation = (uint32_t)(cluster->logical_offset &
		    block_mask);
		return (FSIMG_OK);
	}
	if (length.format == 0) {
		cluster->mode = EROFS_CLUSTER_SHIFTED;
		return (FSIMG_OK);
	}
	algorithm = length.format - 1u;
	if (algorithm >= EROFS_ALGORITHM_COUNT ||
	    (erofs->config.algorithms & 1u << algorithm) == 0)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "compression algorithm not configured"));
	cluster->mode = EROFS_CLUSTER_COMPRESSED;
	cluster->algorithm = (uint8_t)algorithm;
	cluster->partial = length.partial;
	return (FSIMG_OK);
}
