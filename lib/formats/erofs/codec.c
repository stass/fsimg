/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "crc32c.h"
#include "erofs.h"
#include "fsimg-endian.h"
#include "internal.h"

/*
 * Pure decoders and encoders of the EROFS on-disk structures. A decoder
 * takes bytes and their length and returns a validated record. It proves
 * the span it reads before reading it, validates every rule that concerns
 * its structure alone, and publishes its result only after every check
 * passed. An encoder is a decoder's exact inverse for the records the
 * writer emits, every reserved byte zero; it refuses a record it cannot
 * represent rather than truncate a field. Nothing here performs I/O or
 * allocates.
 */

/* Superblock field offsets, relative to volume byte 1024. */
enum {
	SB_CHECKSUM = 0x04,
	SB_COMPAT = 0x08,
	SB_BLKSZBITS = 0x0c,
	SB_EXTENSION_SLOTS = 0x0d,
	SB_ROOT_NID = 0x0e,
	SB_INODE_COUNT = 0x10,
	SB_EPOCH = 0x18,
	SB_EPOCH_NSEC = 0x20,
	SB_BLOCK_COUNT = 0x24,
	SB_META_BLKADDR = 0x28,
	SB_XATTR_BLKADDR = 0x2c,
	SB_UUID = 0x30,
	SB_NAME = 0x40,
	SB_INCOMPAT = 0x50,
	SB_COMPRESSION = 0x54,
	SB_EXTRA_DEVICES = 0x56,
	SB_DIRBLKBITS = 0x5a,
	SB_PREFIX_COUNT = 0x5b,
	SB_PREFIX_START = 0x5c,
	SB_PACKED_NID = 0x60,
	SB_FILTER_RESERVED = 0x68,
	SB_ISHARE_PREFIX = 0x69,
	SB_RESERVED_6A = 0x6a,
	SB_BUILD_TIME = 0x6c,
	SB_WIDE_ROOT_NID = 0x70,
	SB_RESERVED_78 = 0x78,
	SB_BOX_NID = 0x80,		/* the first extension slot */
	SB_RESERVED_88 = 0x88
};

/* Compact inode field offsets. */
enum {
	IC_XATTR_COUNT = 0x02,
	IC_MODE = 0x04,
	IC_NLINK = 0x06,
	IC_SIZE = 0x08,
	IC_TIME = 0x0c,
	IC_LAYOUT_WORD = 0x10,
	IC_SERIAL = 0x14,
	IC_UID = 0x18,
	IC_GID = 0x1a,
	IC_RESERVED = 0x1c
};

/* Extended inode field offsets; the first eight bytes are as compact. */
enum {
	IX_HIGH_WORD = 0x06,
	IX_SIZE = 0x08,
	IX_LAYOUT_WORD = 0x10,
	IX_SERIAL = 0x14,
	IX_UID = 0x18,
	IX_GID = 0x1c,
	IX_TIME = 0x20,
	IX_TIME_NSEC = 0x28,
	IX_NLINK = 0x2c,
	IX_RESERVED = 0x30,
	IX_RESERVED_BYTES = 16
};

/*
 * The inode format word: bit 0 selects the extended form, bits 1 to 3 the
 * data layout, and bit 4 has a meaning only under the 48-bit layout. Bits
 * 5 to 15 are reserved.
 */
#define FORMAT_EXTENDED		0x0001u
#define FORMAT_LAYOUT_SHIFT	1
#define FORMAT_LAYOUT_MASK	0x0007u
#define FORMAT_BIT4		0x0010u
#define FORMAT_RESERVED		0xffe0u

#define NSEC_PER_SEC		UINT32_C(1000000000)

/*
 * The chunk format, the low 16 bits of a chunk-based inode's layout word:
 * bits 0 to 4 are chunkbits, bit 5 selects 8-byte index entries, bit 6
 * allows 48-bit block numbers, and bits 7 to 15 are reserved.
 */
#define CHUNK_FORMAT_BITS	0x001fu
#define CHUNK_FORMAT_INDEXES	0x0020u
#define CHUNK_FORMAT_48BIT	0x0040u
#define CHUNK_FORMAT_RESERVED	0xff80u
#define CHUNK_HOLE_32		UINT32_C(0xffffffff)
#define CHUNK_HOLE_48		UINT64_C(0xffffffffffff)

/* The MicroLZMA dictionary bounds and the other records' field maxima. */
#define LZMA_DICT_MIN		EROFS_LZMA_DICT_MIN
#define LZMA_DICT_MAX		UINT32_C(8388608)
#define DEFLATE_WINDOW_BITS_MAX	15u
#define ZSTD_WINDOW_LOG_BASE	10u
#define ZSTD_WINDOW_LOG_EXTRA_MAX 10u

/* The name filter's seed, to which the built-in prefix's index is added. */
#define FILTER_SEED		UINT32_C(0x25bbe08f)

/* XXH32's primes, the constants of the published algorithm. */
#define XXH32_PRIME1		UINT32_C(0x9e3779b1)
#define XXH32_PRIME2		UINT32_C(0x85ebca77)
#define XXH32_PRIME3		UINT32_C(0xc2b2ae3d)
#define XXH32_PRIME4		UINT32_C(0x27d4eb2f)
#define XXH32_PRIME5		UINT32_C(0x165667b1)
#define XXH32_STRIPE_BYTES	EROFS_XXH32_STRIPE_BYTES

typedef erofs_xxh32_state_t xxh32_state_t;

/*
 * Index record fields: the type's two bits, a full record's partial-
 * reference mark, and the count marker a non-head's backward field may hold
 * instead of a distance, with the cluster's blocks in its low 11 bits.
 */
#define RECORD_TYPE_MASK	0x0003u
#define RECORD_PARTIAL		0x8000u
#define RECORD_MARKER		0x0800u
#define RECORD_COUNT_MASK	0x07ffu

static bool		all_zero(const unsigned char *, size_t);
static bool		builtin_prefix(uint8_t);
static fsimg_status_t	compact_decode(const unsigned char *,
			    const erofs_super_t *, bool, erofs_inode_t *,
			    const char **);
static unsigned		compact_field(const unsigned char *, unsigned,
			    unsigned, erofs_record_type_t *);
static fsimg_status_t	config_decode(const unsigned char *, size_t, size_t,
			    const erofs_super_t *, erofs_config_t *,
			    const char **);
static fsimg_status_t	config_payload_decode(unsigned, const unsigned char *,
			    erofs_config_t *, const char **);
static fsimg_status_t	dirent_parse(const unsigned char *, size_t, size_t,
			    size_t, erofs_dirent_t *, const char **);
static fsimg_status_t	extended_decode(const unsigned char *,
			    const erofs_super_t *, bool, erofs_inode_t *,
			    const char **);
static fsimg_status_t	group_blocks_before(const erofs_zrecord_t *,
			    unsigned, bool, uint64_t *, const char **);
static fsimg_status_t	inode_body_measure(const erofs_super_t *,
			    erofs_inode_t *, const char **);
static fsimg_status_t	inode_kind_check(const erofs_inode_t *,
			    const char **);
static bool		kind_decode(uint16_t, fsimg_entry_kind_t *);
static bool		kind_encode(fsimg_entry_kind_t, uint16_t *);
static int64_t		load_le_s64(const unsigned char *);
static fsimg_status_t	reject(fsimg_status_t, const char *, const char **);
static fsimg_status_t	super_parse(const unsigned char *, size_t,
			    erofs_super_t *, const char **);
static uint32_t		rotl32(uint32_t, unsigned);
static void		xxh32_absorb(xxh32_state_t *, const unsigned char *,
			    size_t);
static uint32_t		xxh32_finish(const xxh32_state_t *);
static void		xxh32_init(xxh32_state_t *, uint32_t);
static uint32_t		xxh32_round(uint32_t, uint32_t);

static fsimg_status_t
reject(fsimg_status_t status, const char *phrase, const char **what)
{
	*what = phrase;
	return (status);
}

static bool
all_zero(const unsigned char *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++)
		if (bytes[i] != 0)
			return (false);
	return (true);
}

/*
 * A signed 64-bit field, converted without relying on an
 * implementation-defined conversion of an out-of-range unsigned value.
 */
static int64_t
load_le_s64(const unsigned char *p)
{
	uint64_t value = fsimg_load_le64(p);

	if (value <= INT64_MAX)
		return ((int64_t)value);
	return (-(int64_t)(~value) - 1);
}

/* Whether the four bytes at the superblock's start are the EROFS magic. */
bool
erofs_magic_matches(const unsigned char *sb, size_t length)
{
	return (length >= 4 && fsimg_load_le32(sb) == EROFS_MAGIC);
}

/*
 * The base superblock, the 128 bytes at volume byte 1024; raw points at
 * them. Every base field is decoded, but only the probe's checks apply: a
 * block size below 512 bytes is corrupt; one above 4096 bytes, a nonzero
 * directory block size, extension slots that leave the superblock region
 * and an unknown incompatible feature are unsupported. The result is
 * therefore only partly validated, and only erofs_region_decode(), which
 * applies the remaining rules, publishes it. A reserved byte that is
 * nonzero is recorded, not refused.
 */
static fsimg_status_t
super_parse(const unsigned char *raw, size_t length, erofs_super_t *out,
    const char **what)
{
	erofs_super_t sb;
	const unsigned char *name_end;
	uint32_t block_count_low;
	size_t super_bytes;

	if (length < EROFS_SUPER_BASE_BYTES)
		return (reject(FSIMG_ECORRUPT, "superblock truncated", what));

	bzero(&sb, sizeof(sb));
	sb.blkszbits = raw[SB_BLKSZBITS];
	sb.extension_slots = raw[SB_EXTENSION_SLOTS];
	sb.compat = fsimg_load_le32(raw + SB_COMPAT);
	sb.incompat = fsimg_load_le32(raw + SB_INCOMPAT);
	sb.epoch_nsec = fsimg_load_le32(raw + SB_EPOCH_NSEC);
	super_bytes = EROFS_SUPER_BASE_BYTES +
	    (size_t)sb.extension_slots * EROFS_SUPER_SLOT_BYTES;

	/*
	 * The probe's checks: block size, directory block size, extension
	 * slots and the incompatible-feature table.
	 */
	if (sb.blkszbits < EROFS_BLKSZBITS_MIN)
		return (reject(FSIMG_ECORRUPT, "superblock block size", what));
	if (sb.blkszbits > EROFS_BLKSZBITS_MAX)
		return (reject(FSIMG_EUNSUPPORTED, "superblock block size",
		    what));
	if (raw[SB_DIRBLKBITS] != 0)
		return (reject(FSIMG_EUNSUPPORTED,
		    "superblock directory block size", what));
	if (super_bytes > EROFS_SUPER_REGION_BYTES)
		return (reject(FSIMG_EUNSUPPORTED,
		    "superblock extension slots", what));
	if ((sb.incompat & ~EROFS_INCOMPAT_KNOWN) != 0)
		return (reject(FSIMG_EUNSUPPORTED, "incompatible feature",
		    what));

	/*
	 * Under the 48-bit layout the root NID moves to its wide field and
	 * the 16-bit field holds the block count's bits 32 to 47; otherwise
	 * the wide field is reserved.
	 */
	block_count_low = fsimg_load_le32(raw + SB_BLOCK_COUNT);
	if ((sb.incompat & EROFS_INCOMPAT_48BIT) != 0) {
		sb.root_nid = fsimg_load_le64(raw + SB_WIDE_ROOT_NID);
		sb.block_count = (uint64_t)fsimg_load_le16(raw + SB_ROOT_NID) <<
		    32 | block_count_low;
	} else {
		sb.root_nid = fsimg_load_le16(raw + SB_ROOT_NID);
		sb.block_count = block_count_low;
		if (!all_zero(raw + SB_WIDE_ROOT_NID, 8))
			sb.reserved_nonzero = true;
	}
	if (!all_zero(raw + SB_RESERVED_6A, 2) ||
	    !all_zero(raw + SB_RESERVED_78, 8))
		sb.reserved_nonzero = true;

	sb.checksum = fsimg_load_le32(raw + SB_CHECKSUM);
	sb.inode_count = fsimg_load_le64(raw + SB_INODE_COUNT);
	sb.epoch = load_le_s64(raw + SB_EPOCH);
	sb.meta_blkaddr = fsimg_load_le32(raw + SB_META_BLKADDR);
	sb.xattr_blkaddr = fsimg_load_le32(raw + SB_XATTR_BLKADDR);
	bcopy(raw + SB_UUID, sb.uuid, sizeof(sb.uuid));
	bcopy(raw + SB_NAME, sb.name, sizeof(sb.name));
	name_end = memchr(sb.name, '\0', sizeof(sb.name));
	sb.name_length = name_end == NULL ? sizeof(sb.name) :
	    (uint8_t)(name_end - sb.name);
	sb.compression_word = fsimg_load_le16(raw + SB_COMPRESSION);
	sb.xattr_prefix_count = raw[SB_PREFIX_COUNT];
	sb.xattr_prefix_start = fsimg_load_le32(raw + SB_PREFIX_START);
	sb.packed_nid = fsimg_load_le64(raw + SB_PACKED_NID);
	sb.filter_disabled = raw[SB_FILTER_RESERVED] != 0;
	sb.ishare_prefix = raw[SB_ISHARE_PREFIX];
	sb.build_time = fsimg_load_le32(raw + SB_BUILD_TIME);

	*out = sb;
	return (FSIMG_OK);
}

/*
 * The probe: whether the base superblock at raw, the 128 bytes at volume
 * byte 1024, passes the checks super_parse() states. The magic is the
 * caller's to check, since a forced open waives it.
 */
fsimg_status_t
erofs_super_probe(const unsigned char *raw, size_t length, const char **what)
{
	erofs_super_t sb;

	return (super_parse(raw, length, &sb, what));
}

/*
 * The whole superblock region as open reads it: region holds the bytes
 * from volume byte 1024 to byte 4096 or the volume's end, whichever comes
 * first, so length is below 3072 only for a volume shorter than 4096
 * bytes. A structure that would leave the region is unsupported; one that
 * fits the region but not the volume is a truncation, which is corrupt.
 * The checks run in this order: the probe's, then the extension slots
 * against length, the extra-device refusal, the checksum, the remaining
 * base-field rules, the metadata-box slot and the configuration records.
 * This is the only function that publishes an erofs_super_t.
 */
fsimg_status_t
erofs_region_decode(const unsigned char *region, size_t length,
    erofs_super_t *sb_out, erofs_config_t *config_out, const char **what)
{
	erofs_config_t config;
	erofs_super_t sb;
	fsimg_status_t status;
	uint32_t checksum;
	size_t super_bytes;

	status = super_parse(region, length, &sb, what);
	if (status != FSIMG_OK)
		return (status);
	super_bytes = EROFS_SUPER_BASE_BYTES +
	    (size_t)sb.extension_slots * EROFS_SUPER_SLOT_BYTES;
	if (super_bytes > length)
		return (reject(FSIMG_ECORRUPT,
		    "superblock extension slots truncated", what));

	/*
	 * Extra devices are refused first: the count is the one field every
	 * producer of such a volume sets, whatever features it also claims.
	 */
	if (fsimg_load_le16(region + SB_EXTRA_DEVICES) != 0)
		return (reject(FSIMG_EUNSUPPORTED, "extra devices", what));

	if ((sb.compat & EROFS_COMPAT_SB_CHKSUM) != 0) {
		if (!erofs_super_checksum(region, length, sb.blkszbits,
		    &checksum))
			return (reject(FSIMG_ECORRUPT,
			    "superblock checksum range truncated", what));
		if (checksum != sb.checksum)
			return (reject(FSIMG_ECORRUPT, "superblock checksum",
			    what));
	}

	if (sb.epoch_nsec >= NSEC_PER_SEC)
		return (reject(FSIMG_ECORRUPT, "superblock epoch nanoseconds",
		    what));
	if ((sb.compat & EROFS_COMPAT_SHARED_EA_IN_METABOX) != 0 &&
	    (sb.incompat & EROFS_INCOMPAT_METABOX) == 0)
		return (reject(FSIMG_ECORRUPT,
		    "shared attributes in a metadata box without METABOX",
		    what));
	if ((sb.incompat & EROFS_INCOMPAT_METABOX) != 0 &&
	    sb.extension_slots == 0)
		return (reject(FSIMG_ECORRUPT,
		    "metadata box without an extension slot", what));

	/* Only the metadata box interprets an extension slot's bytes. */
	if ((sb.incompat & EROFS_INCOMPAT_METABOX) != 0) {
		sb.box_nid = fsimg_load_le64(region + SB_BOX_NID);
		if ((sb.box_nid & EROFS_NID_BOX) != 0)
			return (reject(FSIMG_ECORRUPT, "metadata box NID",
			    what));
		if (!all_zero(region + SB_RESERVED_88, 8))
			sb.reserved_nonzero = true;
	}

	status = config_decode(region, length, super_bytes, &sb, &config,
	    what);
	if (status != FSIMG_OK)
		return (status);

	*sb_out = sb;
	*config_out = config;
	return (FSIMG_OK);
}

/*
 * The length of the superblock checksum's covered range, which starts at
 * volume byte 1024: B - 1024 when the block size B exceeds 1024 bytes, and
 * B otherwise, so never more than EROFS_SUPER_REGION_BYTES. Zero for a
 * block shift outside the format's range.
 */
size_t
erofs_super_covered_bytes(unsigned blkszbits)
{
	size_t block_bytes;

	if (blkszbits < EROFS_BLKSZBITS_MIN || blkszbits > EROFS_BLKSZBITS_MAX)
		return (0);
	block_bytes = (size_t)1 << blkszbits;
	return (block_bytes > EROFS_SUPER_OFFSET ?
	    block_bytes - EROFS_SUPER_OFFSET : block_bytes);
}

/*
 * The superblock checksum: CRC-32C over the covered range,
 * erofs_super_covered_bytes() from volume byte 1024, with the checksum
 * field taken as zero. The register starts at all ones
 * and is stored without the final complement. region starts at volume
 * byte 1024; false means it is shorter than the covered range. The writer
 * calls this once every byte of the range is final.
 */
bool
erofs_super_checksum(const unsigned char *region, size_t length,
    unsigned blkszbits, uint32_t *checksum)
{
	static const unsigned char zero_field[4];
	size_t covered_bytes;
	uint32_t crc;

	covered_bytes = erofs_super_covered_bytes(blkszbits);
	if (covered_bytes == 0 || length < covered_bytes)
		return (false);

	crc = fsimg_crc32c_update(FSIMG_CRC32C_SEED, region, SB_CHECKSUM);
	crc = fsimg_crc32c_update(crc, zero_field, sizeof(zero_field));
	crc = fsimg_crc32c_update(crc, region + SB_CHECKSUM + 4,
	    covered_bytes - SB_CHECKSUM - 4);
	*checksum = crc;
	return (true);
}

/*
 * The base superblock into the EROFS_SUPER_BASE_BYTES at raw, the inverse
 * of super_parse() for a superblock without the 48-bit layout and without
 * extension slots, which is every superblock the writer emits: its
 * features are at most SB_CHKSUM and MTIME, and LZ4_0PADDING. The volume
 * name is its first name_length bytes, NUL-padded. The extra device count
 * and every reserved byte are zero. false means sb is not of that shape, a
 * field exceeds its width, the block shift or nanoseconds are outside
 * their range, or the name holds a NUL; raw is then unspecified.
 */
bool
erofs_super_encode(const erofs_super_t *sb, unsigned char *raw)
{
	if ((sb->compat &
	    ~(EROFS_COMPAT_SB_CHKSUM | EROFS_COMPAT_MTIME)) != 0 ||
	    (sb->incompat & ~EROFS_INCOMPAT_LZ4_0PADDING) != 0 ||
	    sb->extension_slots != 0 || sb->box_nid != 0 ||
	    sb->root_nid > UINT16_MAX || sb->block_count > UINT32_MAX ||
	    sb->blkszbits < EROFS_BLKSZBITS_MIN ||
	    sb->blkszbits > EROFS_BLKSZBITS_MAX ||
	    sb->epoch_nsec >= NSEC_PER_SEC ||
	    sb->name_length > sizeof(sb->name) ||
	    memchr(sb->name, '\0', sb->name_length) != NULL)
		return (false);

	bzero(raw, EROFS_SUPER_BASE_BYTES);
	fsimg_store_le32(raw, EROFS_MAGIC);
	fsimg_store_le32(raw + SB_CHECKSUM, sb->checksum);
	fsimg_store_le32(raw + SB_COMPAT, sb->compat);
	raw[SB_BLKSZBITS] = sb->blkszbits;
	fsimg_store_le16(raw + SB_ROOT_NID, (uint16_t)sb->root_nid);
	fsimg_store_le64(raw + SB_INODE_COUNT, sb->inode_count);
	fsimg_store_le64(raw + SB_EPOCH, (uint64_t)sb->epoch);
	fsimg_store_le32(raw + SB_EPOCH_NSEC, sb->epoch_nsec);
	fsimg_store_le32(raw + SB_BLOCK_COUNT, (uint32_t)sb->block_count);
	fsimg_store_le32(raw + SB_META_BLKADDR, sb->meta_blkaddr);
	fsimg_store_le32(raw + SB_XATTR_BLKADDR, sb->xattr_blkaddr);
	bcopy(sb->uuid, raw + SB_UUID, sizeof(sb->uuid));
	bcopy(sb->name, raw + SB_NAME, sb->name_length);
	fsimg_store_le32(raw + SB_INCOMPAT, sb->incompat);
	fsimg_store_le16(raw + SB_COMPRESSION, sb->compression_word);
	raw[SB_PREFIX_COUNT] = sb->xattr_prefix_count;
	fsimg_store_le32(raw + SB_PREFIX_START, sb->xattr_prefix_start);
	fsimg_store_le64(raw + SB_PACKED_NID, sb->packed_nid);
	raw[SB_FILTER_RESERVED] = sb->filter_disabled ? 1 : 0;
	raw[SB_ISHARE_PREFIX] = sb->ishare_prefix;
	fsimg_store_le32(raw + SB_BUILD_TIME, sb->build_time);
	return (true);
}

/*
 * The compression configuration. Without COMPR_CFGS the superblock word is
 * LZ4's maximum distance and LZ4 is the only algorithm. With it the word
 * is the mask of algorithms present, and one record per set bit follows
 * the superblock and its extension slots in increasing algorithm order,
 * each on a 4-byte boundary: a 16-bit payload length, then the payload.
 * offset is where the first record may start.
 */
static fsimg_status_t
config_decode(const unsigned char *region, size_t length, size_t offset,
    const erofs_super_t *sb, erofs_config_t *out, const char **what)
{
	static const uint16_t payload_bytes[EROFS_ALGORITHM_COUNT] = {
		[EROFS_ALGORITHM_LZ4] = 14,
		[EROFS_ALGORITHM_LZMA] = 14,
		[EROFS_ALGORITHM_DEFLATE] = 6,
		[EROFS_ALGORITHM_ZSTD] = 6
	};
	erofs_config_t config;
	fsimg_status_t status;
	uint16_t mask;
	size_t record_end;

	bzero(&config, sizeof(config));
	if ((sb->incompat & EROFS_INCOMPAT_COMPR_CFGS) == 0) {
		config.algorithms = 1u << EROFS_ALGORITHM_LZ4;
		config.lz4_max_distance = sb->compression_word;
		*out = config;
		return (FSIMG_OK);
	}

	mask = sb->compression_word;
	if ((mask >> EROFS_ALGORITHM_COUNT) != 0)
		return (reject(FSIMG_EUNSUPPORTED, "compression algorithm mask",
		    what));
	for (unsigned algorithm = 0; algorithm < EROFS_ALGORITHM_COUNT;
	    algorithm++) {
		if ((mask & (1u << algorithm)) == 0)
			continue;

		/*
		 * The record's length word, then its whole extent. Slots and
		 * every record length keep offset 4-aligned already; the
		 * rounding states the format's rule rather than relying on it.
		 */
		offset = (offset + 3) & ~(size_t)3;
		if (offset + 2 > EROFS_SUPER_REGION_BYTES)
			return (reject(FSIMG_EUNSUPPORTED,
			    "configuration record past the superblock region",
			    what));
		if (offset + 2 > length)
			return (reject(FSIMG_ECORRUPT,
			    "configuration record truncated", what));
		if (fsimg_load_le16(region + offset) !=
		    payload_bytes[algorithm])
			return (reject(FSIMG_EUNSUPPORTED,
			    "configuration record length", what));
		record_end = offset + 2 + payload_bytes[algorithm];
		if (record_end > EROFS_SUPER_REGION_BYTES)
			return (reject(FSIMG_EUNSUPPORTED,
			    "configuration record past the superblock region",
			    what));
		if (record_end > length)
			return (reject(FSIMG_ECORRUPT,
			    "configuration record truncated", what));

		status = config_payload_decode(algorithm, region + offset + 2,
		    &config, what);
		if (status != FSIMG_OK)
			return (status);
		offset = record_end;
	}
	config.algorithms = (uint8_t)mask;

	*out = config;
	return (FSIMG_OK);
}

/*
 * One record's payload, whose length config_decode() proved. Its bounds
 * are the limits the kernel enforces; a value outside them is unsupported.
 */
static fsimg_status_t
config_payload_decode(unsigned algorithm, const unsigned char *payload,
    erofs_config_t *config, const char **what)
{
	uint32_t dict_size;

	switch (algorithm) {
	case EROFS_ALGORITHM_LZ4:
		config->lz4_max_distance = fsimg_load_le16(payload);
		config->lz4_max_pcluster_blocks = fsimg_load_le16(payload + 2);
		if (!all_zero(payload + 4, 10))
			config->reserved_nonzero = true;
		break;
	case EROFS_ALGORITHM_LZMA:
		dict_size = fsimg_load_le32(payload);
		if (dict_size < LZMA_DICT_MIN || dict_size > LZMA_DICT_MAX)
			return (reject(FSIMG_EUNSUPPORTED,
			    "MicroLZMA dictionary size", what));
		if (fsimg_load_le16(payload + 4) != 0)
			return (reject(FSIMG_EUNSUPPORTED, "MicroLZMA format",
			    what));
		config->lzma_dict_size = dict_size;
		if (!all_zero(payload + 6, 8))
			config->reserved_nonzero = true;
		break;
	case EROFS_ALGORITHM_DEFLATE:
		if (payload[0] > DEFLATE_WINDOW_BITS_MAX)
			return (reject(FSIMG_EUNSUPPORTED,
			    "DEFLATE window bits", what));
		config->deflate_window_bits = payload[0];
		if (!all_zero(payload + 1, 5))
			config->reserved_nonzero = true;
		break;
	case EROFS_ALGORITHM_ZSTD:
		if (payload[0] != 0)
			return (reject(FSIMG_EUNSUPPORTED, "Zstandard format",
			    what));
		if (payload[1] > ZSTD_WINDOW_LOG_EXTRA_MAX)
			return (reject(FSIMG_EUNSUPPORTED,
			    "Zstandard window log", what));
		config->zstd_window_log =
		    (uint8_t)(payload[1] + ZSTD_WINDOW_LOG_BASE);
		if (!all_zero(payload + 2, 4))
			config->reserved_nonzero = true;
		break;
	}
	return (FSIMG_OK);
}

/*
 * The fixed part's length, from the format word in the inode's first two
 * bytes, so a caller knows how much to read before erofs_inode_decode().
 */
size_t
erofs_inode_fixed_bytes(const unsigned char *raw)
{
	if ((fsimg_load_le16(raw) & FORMAT_EXTENDED) != 0)
		return (EROFS_INODE_EXTENDED_BYTES);
	return (EROFS_INODE_COMPACT_BYTES);
}

/*
 * An inode's fixed part, compact or extended, with its kind, layout and
 * body length; raw holds at least erofs_inode_fixed_bytes() bytes. sb is
 * the decoded superblock, which supplies the compact time base, the block
 * size and the features a layout needs. Whether the body lies inside its
 * address space, and whether an inline tail stays in one block, depend on
 * where the inode is and belong to the caller.
 */
fsimg_status_t
erofs_inode_decode(const unsigned char *raw, size_t length,
    const erofs_super_t *sb, erofs_inode_t *out, const char **what)
{
	erofs_inode_t inode;
	fsimg_status_t status;
	uint16_t format, layout, mode, xattr_count;
	bool bit4;

	if (length < 2 || length < erofs_inode_fixed_bytes(raw))
		return (reject(FSIMG_ECORRUPT, "inode truncated", what));

	format = fsimg_load_le16(raw);
	bit4 = (format & FORMAT_BIT4) != 0;
	if ((format & FORMAT_RESERVED) != 0)
		return (reject(FSIMG_ECORRUPT, "inode format", what));
	if (bit4 && (sb->incompat & EROFS_INCOMPAT_48BIT) == 0)
		return (reject(FSIMG_ECORRUPT, "inode format", what));

	bzero(&inode, sizeof(inode));
	inode.extended = (format & FORMAT_EXTENDED) != 0;
	inode.fixed_bytes = (uint32_t)erofs_inode_fixed_bytes(raw);
	layout = (format >> FORMAT_LAYOUT_SHIFT) & FORMAT_LAYOUT_MASK;
	if (layout > EROFS_LAYOUT_CHUNK_BASED)
		return (reject(FSIMG_ECORRUPT, "inode data layout", what));
	inode.layout = (erofs_layout_t)layout;

	/*
	 * The attribute region is empty, or a 12-byte header and 4-byte
	 * units: at most 12 + 4 * 65534 bytes.
	 */
	xattr_count = fsimg_load_le16(raw + IC_XATTR_COUNT);
	inode.xattr_bytes = xattr_count == 0 ? 0 :
	    EROFS_XATTR_HEADER_BYTES + 4 * ((uint32_t)xattr_count - 1);
	mode = fsimg_load_le16(raw + IC_MODE);
	if (!kind_decode(mode, &inode.kind))
		return (reject(FSIMG_ECORRUPT, "inode file type", what));
	inode.permissions = mode & EROFS_MODE_PERMISSIONS;

	if (inode.extended)
		status = extended_decode(raw, sb, bit4, &inode, what);
	else
		status = compact_decode(raw, sb, bit4, &inode, what);
	if (status != FSIMG_OK)
		return (status);

	status = inode_kind_check(&inode, what);
	if (status != FSIMG_OK)
		return (status);

	status = inode_body_measure(sb, &inode, what);
	if (status != FSIMG_OK)
		return (status);

	*out = inode;
	return (FSIMG_OK);
}

static bool
kind_decode(uint16_t mode, fsimg_entry_kind_t *kind)
{
	switch (mode & EROFS_MODE_TYPE_MASK) {
	case EROFS_MODE_REGULAR:
		*kind = FSIMG_ENTRY_FILE;
		return (true);
	case EROFS_MODE_DIRECTORY:
		*kind = FSIMG_ENTRY_DIRECTORY;
		return (true);
	case EROFS_MODE_SYMLINK:
		*kind = FSIMG_ENTRY_SYMLINK;
		return (true);
	case EROFS_MODE_FIFO:
		*kind = FSIMG_ENTRY_FIFO;
		return (true);
	case EROFS_MODE_CHAR_DEVICE:
		*kind = FSIMG_ENTRY_CHAR_DEVICE;
		return (true);
	case EROFS_MODE_BLOCK_DEVICE:
		*kind = FSIMG_ENTRY_BLOCK_DEVICE;
		return (true);
	case EROFS_MODE_SOCKET:
		*kind = FSIMG_ENTRY_SOCKET;
		return (true);
	default:
		return (false);
	}
}

/*
 * The 32-byte compact form. Its time is the superblock's epoch plus the
 * unsigned word at 0x0C, with the superblock's nanoseconds, on every
 * volume. Under the 48-bit layout format bit 4 omits a directory's "."
 * record, and on any other inode means a link count of 1 with start-block
 * bits 32 to 47 in the link-count word.
 */
static fsimg_status_t
compact_decode(const unsigned char *raw, const erofs_super_t *sb, bool bit4,
    erofs_inode_t *inode, const char **what)
{
	uint32_t time_offset;

	time_offset = fsimg_load_le32(raw + IC_TIME);
	if (sb->epoch > INT64_MAX - (int64_t)time_offset)
		return (reject(FSIMG_ECORRUPT, "inode time", what));
	if (fsimg_load_le32(raw + IC_RESERVED) != 0)
		return (reject(FSIMG_ECORRUPT, "inode reserved bytes", what));

	inode->nlink = fsimg_load_le16(raw + IC_NLINK);
	inode->size = fsimg_load_le32(raw + IC_SIZE);
	inode->time = sb->epoch + (int64_t)time_offset;
	inode->time_nsec = sb->epoch_nsec;
	inode->layout_word = fsimg_load_le32(raw + IC_LAYOUT_WORD);
	inode->serial = fsimg_load_le32(raw + IC_SERIAL);
	inode->uid = fsimg_load_le16(raw + IC_UID);
	inode->gid = fsimg_load_le16(raw + IC_GID);
	if (bit4 && inode->kind == FSIMG_ENTRY_DIRECTORY) {
		inode->omits_dot = true;
	} else if (bit4) {
		inode->layout_high = (uint16_t)inode->nlink;
		inode->nlink = 1;
	}
	return (FSIMG_OK);
}

/*
 * The 64-byte extended form, with a signed time of its own. The word at
 * 0x06 is reserved, and holds start-block bits 32 to 47 under the 48-bit
 * layout. Format bit 4 omits a directory's "." record there; on any other
 * extended inode no published layout defines it.
 */
static fsimg_status_t
extended_decode(const unsigned char *raw, const erofs_super_t *sb, bool bit4,
    erofs_inode_t *inode, const char **what)
{
	uint16_t high_word;
	uint32_t time_nsec;

	high_word = fsimg_load_le16(raw + IX_HIGH_WORD);
	time_nsec = fsimg_load_le32(raw + IX_TIME_NSEC);
	if (bit4 && inode->kind != FSIMG_ENTRY_DIRECTORY)
		return (reject(FSIMG_EUNSUPPORTED,
		    "extended inode format bit 4", what));
	if ((sb->incompat & EROFS_INCOMPAT_48BIT) == 0 && high_word != 0)
		return (reject(FSIMG_ECORRUPT, "inode reserved bytes", what));
	if (time_nsec >= NSEC_PER_SEC)
		return (reject(FSIMG_ECORRUPT, "inode nanoseconds", what));
	if (!all_zero(raw + IX_RESERVED, IX_RESERVED_BYTES))
		return (reject(FSIMG_ECORRUPT, "inode reserved bytes", what));

	inode->layout_high = high_word;
	inode->omits_dot = bit4;
	inode->size = fsimg_load_le64(raw + IX_SIZE);
	inode->layout_word = fsimg_load_le32(raw + IX_LAYOUT_WORD);
	inode->serial = fsimg_load_le32(raw + IX_SERIAL);
	inode->uid = fsimg_load_le32(raw + IX_UID);
	inode->gid = fsimg_load_le32(raw + IX_GID);
	inode->time = load_le_s64(raw + IX_TIME);
	inode->time_nsec = time_nsec;
	inode->nlink = fsimg_load_le32(raw + IX_NLINK);
	return (FSIMG_OK);
}

/*
 * The rules each kind places on size and layout. A directory holds at
 * least "." and "..", so it is never empty. A symbolic link's target is
 * its content, 1 to EROFS_SYMLINK_MAX bytes. FIFOs, sockets and devices
 * have no content: flat layout and size zero, and a FIFO's or socket's
 * layout word is zero too, where a device keeps its number. Having no
 * start block, none of them has start-block bits 32 to 47 either.
 */
static fsimg_status_t
inode_kind_check(const erofs_inode_t *inode, const char **what)
{
	switch (inode->kind) {
	case FSIMG_ENTRY_FILE:
		break;
	case FSIMG_ENTRY_DIRECTORY:
		if (inode->size == 0)
			return (reject(FSIMG_ECORRUPT, "directory size", what));
		break;
	case FSIMG_ENTRY_SYMLINK:
		if (inode->size == 0)
			return (reject(FSIMG_ECORRUPT, "symbolic link size",
			    what));
		if (inode->size > EROFS_SYMLINK_MAX)
			return (reject(FSIMG_EUNSUPPORTED,
			    "symbolic link size", what));
		break;
	case FSIMG_ENTRY_FIFO:
	case FSIMG_ENTRY_SOCKET:
		if (inode->layout_word != 0)
			return (reject(FSIMG_ECORRUPT,
			    "special file layout word", what));
		/* FALLTHROUGH */
	case FSIMG_ENTRY_CHAR_DEVICE:
	case FSIMG_ENTRY_BLOCK_DEVICE:
		if (inode->layout != EROFS_LAYOUT_FLAT)
			return (reject(FSIMG_ECORRUPT,
			    "special file data layout", what));
		if (inode->layout_high != 0)
			return (reject(FSIMG_ECORRUPT,
			    "special file start-block high bits", what));
		if (inode->size != 0)
			return (reject(FSIMG_ECORRUPT, "special file size",
			    what));
		break;
	}
	return (FSIMG_OK);
}

/*
 * The body's length and where its layout bytes start: the fixed part, the
 * attribute region, then the layout's own bytes. A flat inode has none; a
 * flat-inline inode adds its tail, size mod B; a chunk-based inode adds its
 * index, rounded up to the entry width and one entry per chunk. A
 * flat-inline inode whose size is a nonzero multiple of B would have a
 * whole block as its tail, which is corrupt. A compressed inode adds its
 * map header, at the attribute region's end rounded up to 8, and its index
 * of one record per block of content: 8-byte records from 16 bytes after
 * the header's start, or compact groups from the header's end. Its layout
 * bytes start at the header.
 */
static fsimg_status_t
inode_body_measure(const erofs_super_t *sb, erofs_inode_t *inode,
    const char **what)
{
	erofs_chunk_format_t *chunk_format = &inode->chunk_format;
	erofs_compact_layout_t compact;
	fsimg_status_t status;
	uint64_t block_bytes, chunk_count, index_bytes, index_start;
	uint64_t attributes_end, header_start, records;

	block_bytes = UINT64_C(1) << sb->blkszbits;
	attributes_end = (uint64_t)inode->fixed_bytes + inode->xattr_bytes;
	switch (inode->layout) {
	case EROFS_LAYOUT_FLAT:
		inode->layout_offset = attributes_end;
		inode->body_bytes = attributes_end;
		break;
	case EROFS_LAYOUT_FLAT_INLINE:
		if (inode->size != 0 && inode->size % block_bytes == 0)
			return (reject(FSIMG_ECORRUPT,
			    "inline tail of a whole block", what));
		inode->layout_offset = attributes_end;
		inode->body_bytes = attributes_end + inode->size % block_bytes;
		break;
	case EROFS_LAYOUT_CHUNK_BASED:
		if ((sb->incompat & EROFS_INCOMPAT_CHUNKED_FILE) == 0)
			return (reject(FSIMG_ECORRUPT,
			    "chunk-based layout without CHUNKED_FILE", what));
		status = erofs_chunk_format_decode(inode->layout_word, sb,
		    chunk_format, what);
		if (status != FSIMG_OK)
			return (status);
		index_start = (attributes_end + chunk_format->entry_bytes - 1) &
		    ~(uint64_t)(chunk_format->entry_bytes - 1);
		chunk_count = inode->size / chunk_format->chunk_bytes +
		    (inode->size % chunk_format->chunk_bytes != 0);
		if (!fsimg_mul_u64(chunk_count, chunk_format->entry_bytes,
		    &index_bytes) ||
		    !fsimg_add_u64(index_start, index_bytes,
		    &inode->body_bytes))
			return (reject(FSIMG_ECORRUPT, "chunk index length",
			    what));
		inode->layout_offset = index_start;
		break;
	case EROFS_LAYOUT_COMPRESSED_FULL:
	case EROFS_LAYOUT_COMPRESSED_COMPACT:
		header_start = (attributes_end + 7) & ~(uint64_t)7;
		records = inode->size / block_bytes +
		    (inode->size % block_bytes != 0);
		if (inode->layout == EROFS_LAYOUT_COMPRESSED_FULL) {
			/* At most 2^55 records: the product cannot wrap. */
			index_bytes = EROFS_FULL_INDEX_OFFSET +
			    records * EROFS_FULL_RECORD_BYTES;
		} else {
			erofs_compact_layout(header_start + EROFS_ZHEADER_BYTES,
			    records, &compact);
			index_bytes = EROFS_ZHEADER_BYTES + compact.total_bytes;
		}
		if (!fsimg_add_u64(header_start, index_bytes,
		    &inode->body_bytes))
			return (reject(FSIMG_ECORRUPT,
			    "compressed index length", what));
		inode->layout_offset = header_start;
		break;
	}
	return (FSIMG_OK);
}

/*
 * An inode's fixed part into raw, compact or extended by inode->extended,
 * so raw receives EROFS_INODE_COMPACT_BYTES or EROFS_INODE_EXTENDED_BYTES.
 * It is the inverse of erofs_inode_decode() without format bit 4, which
 * only the 48-bit layout defines; every reserved byte is zero, and sb
 * supplies the compact form's time base and the block size. false means
 * the record does not fit: a kind or attribute region the format cannot
 * express, permission bits beyond 12, or, in the compact form, an owner,
 * group or link count beyond 16 bits, a size beyond 32 bits, start-block
 * high bits, or a time that is not the epoch plus a 32-bit offset with the
 * superblock's nanoseconds. It also means the decoder refuses the encoded
 * bytes, for a rule a single inode must keep, such as a symbolic link's
 * length or a FIFO's empty layout word: the encoded inode is decoded once
 * more, so an inode that encodes is one the reader accepts. raw is then
 * unspecified.
 */
bool
erofs_inode_encode(const erofs_inode_t *inode, const erofs_super_t *sb,
    unsigned char *raw)
{
	erofs_inode_t decoded;
	const char *what;
	uint64_t time_offset;
	uint32_t xattr_count;
	uint16_t mode;

	if (inode->layout > EROFS_LAYOUT_CHUNK_BASED || inode->omits_dot ||
	    inode->permissions > EROFS_MODE_PERMISSIONS ||
	    !kind_encode(inode->kind, &mode))
		return (false);
	xattr_count = 0;
	if (inode->xattr_bytes != 0) {
		if (inode->xattr_bytes < EROFS_XATTR_HEADER_BYTES ||
		    (inode->xattr_bytes - EROFS_XATTR_HEADER_BYTES) % 4 != 0)
			return (false);
		xattr_count = (inode->xattr_bytes - EROFS_XATTR_HEADER_BYTES) /
		    4 + 1;
		if (xattr_count > UINT16_MAX)
			return (false);
	}

	if (inode->extended) {
		if (inode->time_nsec >= NSEC_PER_SEC ||
		    (inode->layout_high != 0 &&
		    (sb->incompat & EROFS_INCOMPAT_48BIT) == 0))
			return (false);
		bzero(raw, EROFS_INODE_EXTENDED_BYTES);
		fsimg_store_le16(raw + IX_HIGH_WORD, inode->layout_high);
		fsimg_store_le64(raw + IX_SIZE, inode->size);
		fsimg_store_le32(raw + IX_LAYOUT_WORD, inode->layout_word);
		fsimg_store_le32(raw + IX_SERIAL, inode->serial);
		fsimg_store_le32(raw + IX_UID, inode->uid);
		fsimg_store_le32(raw + IX_GID, inode->gid);
		fsimg_store_le64(raw + IX_TIME, (uint64_t)inode->time);
		fsimg_store_le32(raw + IX_TIME_NSEC, inode->time_nsec);
		fsimg_store_le32(raw + IX_NLINK, inode->nlink);
	} else {
		/* Unsigned subtraction is exact once time >= epoch. */
		if (inode->time < sb->epoch)
			return (false);
		time_offset = (uint64_t)inode->time - (uint64_t)sb->epoch;
		if (time_offset > UINT32_MAX ||
		    inode->time_nsec != sb->epoch_nsec ||
		    inode->size > UINT32_MAX || inode->nlink > UINT16_MAX ||
		    inode->uid > UINT16_MAX || inode->gid > UINT16_MAX ||
		    inode->layout_high != 0)
			return (false);
		bzero(raw, EROFS_INODE_COMPACT_BYTES);
		fsimg_store_le16(raw + IC_NLINK, (uint16_t)inode->nlink);
		fsimg_store_le32(raw + IC_SIZE, (uint32_t)inode->size);
		fsimg_store_le32(raw + IC_TIME, (uint32_t)time_offset);
		fsimg_store_le32(raw + IC_LAYOUT_WORD, inode->layout_word);
		fsimg_store_le32(raw + IC_SERIAL, inode->serial);
		fsimg_store_le16(raw + IC_UID, (uint16_t)inode->uid);
		fsimg_store_le16(raw + IC_GID, (uint16_t)inode->gid);
	}
	/* The first eight bytes are common to both forms. */
	fsimg_store_le16(raw, (uint16_t)((inode->extended ? FORMAT_EXTENDED :
	    0) | (unsigned)inode->layout << FORMAT_LAYOUT_SHIFT));
	fsimg_store_le16(raw + IC_XATTR_COUNT, (uint16_t)xattr_count);
	fsimg_store_le16(raw + IC_MODE, (uint16_t)(mode | inode->permissions));
	return (erofs_inode_decode(raw, inode->extended ?
	    EROFS_INODE_EXTENDED_BYTES : EROFS_INODE_COMPACT_BYTES, sb,
	    &decoded, &what) == FSIMG_OK);
}

/* The mode's type bits for a kind; false for a kind EROFS cannot store. */
static bool
kind_encode(fsimg_entry_kind_t kind, uint16_t *type)
{
	switch (kind) {
	case FSIMG_ENTRY_FILE:
		*type = EROFS_MODE_REGULAR;
		return (true);
	case FSIMG_ENTRY_DIRECTORY:
		*type = EROFS_MODE_DIRECTORY;
		return (true);
	case FSIMG_ENTRY_SYMLINK:
		*type = EROFS_MODE_SYMLINK;
		return (true);
	case FSIMG_ENTRY_FIFO:
		*type = EROFS_MODE_FIFO;
		return (true);
	case FSIMG_ENTRY_CHAR_DEVICE:
		*type = EROFS_MODE_CHAR_DEVICE;
		return (true);
	case FSIMG_ENTRY_BLOCK_DEVICE:
		*type = EROFS_MODE_BLOCK_DEVICE;
		return (true);
	case FSIMG_ENTRY_SOCKET:
		*type = EROFS_MODE_SOCKET;
		return (true);
	default:
		return (false);
	}
}

/*
 * A device number in the Linux 32-bit encoding: the minor's low 8 bits in
 * bits 0 to 7, the major in bits 8 to 19 and the minor's remaining 12 bits
 * in bits 20 to 31. Every word is a valid number.
 */
void
erofs_device_decode(uint32_t word, uint32_t *major, uint32_t *minor)
{
	*major = (word >> 8) & 0xfff;
	*minor = (word & 0xff) | (word >> 20) << 8;
}

/*
 * A chunk-based inode's chunk format from its layout word. The word's high
 * 16 bits are not part of it: a pinned producer leaves them uninitialised,
 * so they are ignored. The 48-bit bit needs the 48-bit layout and 8-byte
 * entries, the only ones with high bits for it to widen. A chunk is
 * B << chunkbits bytes, at most 2^43 with B at most 2^12 and chunkbits at
 * most 31, so it needs no bound of its own.
 */
fsimg_status_t
erofs_chunk_format_decode(uint32_t layout_word, const erofs_super_t *sb,
    erofs_chunk_format_t *out, const char **what)
{
	erofs_chunk_format_t format;
	uint16_t word = (uint16_t)layout_word;

	bzero(&format, sizeof(format));
	if ((word & CHUNK_FORMAT_RESERVED) != 0)
		return (reject(FSIMG_ECORRUPT, "chunk format", what));
	if ((word & CHUNK_FORMAT_48BIT) != 0 &&
	    ((sb->incompat & EROFS_INCOMPAT_48BIT) == 0 ||
	    (word & CHUNK_FORMAT_INDEXES) == 0))
		return (reject(FSIMG_ECORRUPT, "chunk format 48-bit bit", what));

	format.chunkbits = (uint8_t)(word & CHUNK_FORMAT_BITS);
	format.chunk_bytes = UINT64_C(1) << (sb->blkszbits + format.chunkbits);
	format.entry_bytes = (word & CHUNK_FORMAT_INDEXES) != 0 ? 8 : 4;
	format.wide = (word & CHUNK_FORMAT_48BIT) != 0;
	*out = format;
	return (FSIMG_OK);
}

/*
 * One chunk index entry in the inode's format. A 4-byte entry is a block
 * number. An 8-byte entry is a 16-bit high part, a 16-bit device
 * identifier and a 32-bit block number; the high part widens the number to
 * 48 bits under the format's 48-bit bit and is zero otherwise, and the
 * identifier is zero since the volume has no extra devices. A number of
 * all ones at its width, 32 or 48 bits, is a hole.
 */
fsimg_status_t
erofs_chunk_decode(const unsigned char *raw, size_t length,
    const erofs_chunk_format_t *format, erofs_chunk_t *out,
    const char **what)
{
	erofs_chunk_t chunk;
	uint32_t block_low;
	uint16_t high;

	if (length < format->entry_bytes)
		return (reject(FSIMG_ECORRUPT, "chunk index truncated", what));

	bzero(&chunk, sizeof(chunk));
	if (format->entry_bytes == 4) {
		block_low = fsimg_load_le32(raw);
		chunk.hole = block_low == CHUNK_HOLE_32;
		chunk.block = block_low;
	} else {
		high = fsimg_load_le16(raw);
		block_low = fsimg_load_le32(raw + 4);
		if (fsimg_load_le16(raw + 2) != 0)
			return (reject(FSIMG_ECORRUPT,
			    "chunk device identifier", what));
		if (!format->wide && high != 0)
			return (reject(FSIMG_ECORRUPT, "chunk block high bits",
			    what));
		chunk.block = (uint64_t)high << 32 | block_low;
		chunk.hole = format->wide ? chunk.block == CHUNK_HOLE_48 :
		    block_low == CHUNK_HOLE_32;
	}
	if (chunk.hole)
		chunk.block = 0;

	*out = chunk;
	return (FSIMG_OK);
}

/*
 * The index-th entry of a block whose entry count is known. Names follow the
 * array, concatenated: entry i's name runs from its name offset to entry
 * i + 1's, and the last entry's to the first NUL byte or the block's valid
 * end. Name offsets increase strictly, which leaves only the last name able
 * to be empty. Names are 1 to 255 bytes without "/" or NUL.
 */
static fsimg_status_t
dirent_parse(const unsigned char *bytes, size_t length, size_t count,
    size_t index, erofs_dirent_t *out, const char **what)
{
	const unsigned char *entry, *name, *nul;
	size_t name_end, name_length, name_start;
	uint8_t file_type;

	entry = bytes + index * EROFS_DIRENT_BYTES;
	name_start = fsimg_load_le16(entry + 8);
	if (name_start < count * EROFS_DIRENT_BYTES || name_start >= length)
		return (reject(FSIMG_ECORRUPT, "directory name offset", what));
	if (index + 1 < count) {
		name_end = fsimg_load_le16(entry + EROFS_DIRENT_BYTES + 8);
		if (name_end <= name_start)
			return (reject(FSIMG_ECORRUPT,
			    "directory name offsets not increasing", what));
		if (name_end > length)
			return (reject(FSIMG_ECORRUPT,
			    "directory name offset", what));
	} else {
		nul = memchr(bytes + name_start, '\0', length - name_start);
		name_end = nul == NULL ? length : (size_t)(nul - bytes);
	}

	name = bytes + name_start;
	name_length = name_end - name_start;
	if (name_length == 0)
		return (reject(FSIMG_ECORRUPT, "empty directory name", what));
	if (name_length > EROFS_NAME_MAX)
		return (reject(FSIMG_ECORRUPT, "directory name length", what));
	if (memchr(name, '/', name_length) != NULL ||
	    memchr(name, '\0', name_length) != NULL)
		return (reject(FSIMG_ECORRUPT, "directory name byte", what));
	file_type = entry[10];
	if (file_type > EROFS_FT_SYMLINK)
		return (reject(FSIMG_ECORRUPT, "directory file type", what));

	bzero(out, sizeof(*out));
	out->nid = fsimg_load_le64(entry);
	out->name_offset = (uint16_t)name_start;
	out->name_length = (uint16_t)name_length;
	out->file_type = file_type;
	return (FSIMG_OK);
}

/*
 * The map header of a compressed inode. Byte 6 names the head-1 algorithm
 * in its low four bits and the head-2 algorithm in its high four; byte 7
 * holds the logical cluster size as a shift above the block size in bits 0
 * to 3 and the whole-file fragment mark in bit 7. With the mark set, the
 * header with that bit cleared is a 64-bit offset into the packed inode,
 * and nothing else is read; producers write it only in a full-index inode
 * (E8, e8-lz4-allfrag, e8-lz4-zdir), so a compact one is unsupported.
 * Otherwise the first word is the fragment offset's low half under the
 * fragment advice, or holds the inline tail's stored length in its high
 * half under the inline-tail advice, which may not be zero. Both at once
 * would give the word two meanings, which no producer writes and no reader
 * agrees on: unsupported. Whether a named algorithm is configured is judged
 * when a record of its head type is used, since a file need not use both.
 * The reader requires a logical cluster of one block, and a compact index
 * with 2-byte groups permitted: no producer writes another, so either is
 * unsupported. A structure whose feature the volume lacks is corrupt.
 */
fsimg_status_t
erofs_zheader_decode(const unsigned char *raw, const erofs_super_t *sb,
    erofs_layout_t layout, erofs_zheader_t *out, const char **what)
{
	uint16_t advice = fsimg_load_le16(raw + 4);
	uint8_t algorithms = raw[6], cluster_bits = raw[7];
	uint16_t tail_bytes = 0;
	bool big_head1, big_head2;

	if ((cluster_bits & 0x80) != 0) {
		if ((sb->incompat & EROFS_INCOMPAT_FRAGMENTS) == 0)
			return (reject(FSIMG_ECORRUPT,
			    "whole-file fragment without FRAGMENTS", what));
		if (layout != EROFS_LAYOUT_COMPRESSED_FULL)
			return (reject(FSIMG_EUNSUPPORTED,
			    "whole-file fragment in a compact index", what));
		*out = (erofs_zheader_t){
			.fragment_offset = fsimg_load_le64(raw) &
			    ~(UINT64_C(1) << 63),
			.whole_fragment = true
		};
		return (FSIMG_OK);
	}
	if ((cluster_bits & 0x0f) != 0)
		return (reject(FSIMG_EUNSUPPORTED,
		    "logical cluster size other than the block size", what));
	if ((advice & ~EROFS_ADVICE_KNOWN) != 0)
		return (reject(FSIMG_EUNSUPPORTED, "map header advice bit",
		    what));
	if ((advice & EROFS_ADVICE_INLINE_TAIL) != 0) {
		if ((sb->incompat & EROFS_INCOMPAT_ZTAILPACKING) == 0)
			return (reject(FSIMG_ECORRUPT,
			    "compressed inline tail without ZTAILPACKING",
			    what));
		tail_bytes = fsimg_load_le16(raw + 2);
		if (tail_bytes == 0)
			return (reject(FSIMG_ECORRUPT,
			    "compressed inline tail of zero bytes", what));
	}
	if ((advice & EROFS_ADVICE_FRAGMENT) != 0) {
		if ((sb->incompat & EROFS_INCOMPAT_FRAGMENTS) == 0)
			return (reject(FSIMG_ECORRUPT,
			    "fragment without FRAGMENTS", what));
		if ((advice & EROFS_ADVICE_INLINE_TAIL) != 0)
			return (reject(FSIMG_EUNSUPPORTED,
			    "compressed inline tail with a fragment", what));
	}

	big_head1 = (advice & EROFS_ADVICE_BIG_HEAD1) != 0;
	big_head2 = (advice & EROFS_ADVICE_BIG_HEAD2) != 0;
	if ((big_head1 || big_head2) &&
	    (sb->incompat & EROFS_INCOMPAT_COMPR_CFGS) == 0)
		return (reject(FSIMG_ECORRUPT,
		    "big physical clusters without COMPR_CFGS", what));
	if (layout == EROFS_LAYOUT_COMPRESSED_COMPACT) {
		if (big_head1 != big_head2)
			return (reject(FSIMG_ECORRUPT,
			    "compact index with unequal big-cluster advice",
			    what));
		if ((advice & EROFS_ADVICE_COMPACT_2B) == 0)
			return (reject(FSIMG_EUNSUPPORTED,
			    "compact index without 2-byte groups", what));
	}

	*out = (erofs_zheader_t){
		.fragment_offset = (advice & EROFS_ADVICE_FRAGMENT) != 0 ?
		    fsimg_load_le32(raw) : 0,
		.advice = advice,
		.tail_bytes = tail_bytes,
		.head1_algorithm = algorithms & 0x0f,
		.head2_algorithm = algorithms >> 4
	};
	return (FSIMG_OK);
}

/*
 * The map header the writer emits, the inverse of erofs_zheader_decode()
 * for a header without an inline tail or fragment: the advice word, the
 * two algorithms and a logical cluster of one block, every other bit zero.
 * false for an inline tail, a fragment, an unknown advice bit or an
 * algorithm beyond four bits.
 */
bool
erofs_zheader_encode(const erofs_zheader_t *header, unsigned char *raw)
{
	if (header->whole_fragment || header->fragment_offset != 0 ||
	    header->tail_bytes != 0 ||
	    (header->advice & ~EROFS_ADVICE_KNOWN) != 0 ||
	    (header->advice & (EROFS_ADVICE_INLINE_TAIL |
	    EROFS_ADVICE_FRAGMENT)) != 0 ||
	    header->head1_algorithm > 0x0f || header->head2_algorithm > 0x0f)
		return (false);
	bzero(raw, EROFS_ZHEADER_BYTES);
	fsimg_store_le16(raw + 4, header->advice);
	raw[6] = (unsigned char)(header->head1_algorithm |
	    header->head2_algorithm << 4);
	return (true);
}

/*
 * One full-index record, the inverse of erofs_full_record_decode() with the
 * forward distance a non-head record also carries: a head's or plain
 * record's clusterofs and 32-bit block, or a non-head record's backward
 * and forward distances, with the partial mark on a head that sets it.
 * false for a count marker, which the writer never emits, a zero or
 * marker-sized backward distance, a block beyond 32 bits or an unknown
 * type.
 */
bool
erofs_full_record_encode(const erofs_zrecord_t *record, uint16_t forward,
    unsigned char *raw)
{
	uint16_t advice;

	if (record->marker || record->type > EROFS_RECORD_HEAD2)
		return (false);
	advice = (uint16_t)record->type;
	bzero(raw, EROFS_FULL_RECORD_BYTES);
	if (record->type == EROFS_RECORD_NONHEAD) {
		if (record->backward == 0 ||
		    (record->backward & RECORD_MARKER) != 0)
			return (false);
		fsimg_store_le16(raw, advice);
		fsimg_store_le16(raw + 4, record->backward);
		fsimg_store_le16(raw + 6, forward);
		return (true);
	}
	if (record->block > UINT32_MAX)
		return (false);
	if (record->partial && record->type != EROFS_RECORD_PLAIN)
		advice |= RECORD_PARTIAL;
	fsimg_store_le16(raw, advice);
	fsimg_store_le16(raw + 2, record->clusterofs);
	fsimg_store_le32(raw + 4, (uint32_t)record->block);
	return (true);
}

/*
 * One 8-byte full-index record: a 16-bit advice word with the type in bits
 * 0 and 1 and the partial-reference mark in bit 15, a 16-bit clusterofs,
 * then a head's or plain record's 32-bit block, or a non-head record's
 * backward and forward distances. A backward field with bit 11 set is a
 * count marker instead, its low 11 bits the cluster's blocks and its
 * backward distance one; big says whether the inode permits big clusters,
 * without which a marker is corrupt. Bits 2 to 14 of the advice word have
 * no meaning this reader knows and are unsupported. A zero backward
 * distance is corrupt; so is a marker with bits above its count, which no
 * count fits. A zero count is judged where a cluster uses it, since one
 * follows a fragment's or inline tail's head. The forward distance is not
 * read (erofs_zrecord_t).
 */
fsimg_status_t
erofs_full_record_decode(const unsigned char *raw, bool big,
    erofs_zrecord_t *out, const char **what)
{
	erofs_zrecord_t record;
	uint16_t advice, backward_field;

	advice = fsimg_load_le16(raw);
	if ((advice & ~(RECORD_TYPE_MASK | RECORD_PARTIAL)) != 0)
		return (reject(FSIMG_EUNSUPPORTED, "index record bit", what));

	bzero(&record, sizeof(record));
	record.type = (erofs_record_type_t)(advice & RECORD_TYPE_MASK);
	if (record.type != EROFS_RECORD_NONHEAD) {
		record.clusterofs = fsimg_load_le16(raw + 2);
		record.block = fsimg_load_le32(raw + 4);
		record.partial = (advice & RECORD_PARTIAL) != 0 &&
		    record.type != EROFS_RECORD_PLAIN;
		*out = record;
		return (FSIMG_OK);
	}

	backward_field = fsimg_load_le16(raw + 4);
	if ((backward_field & RECORD_MARKER) != 0) {
		if (!big)
			return (reject(FSIMG_ECORRUPT,
			    "count marker without big clusters", what));
		if ((backward_field & ~(RECORD_MARKER | RECORD_COUNT_MASK)) != 0)
			return (reject(FSIMG_ECORRUPT, "count marker field",
			    what));
		record.marker = true;
		record.marker_blocks = backward_field & RECORD_COUNT_MASK;
		record.backward = 1;
	} else
		record.backward = backward_field;
	if (record.backward == 0)
		return (reject(FSIMG_ECORRUPT, "index backward distance zero",
		    what));
	*out = record;
	return (FSIMG_OK);
}

/*
 * The compact index's group layout, from the records' count and the
 * header's end measured from the inode's start, which is a multiple of 32,
 * so header_end mod 32 is that of the absolute offset. Groups of 2 records
 * in 8 bytes fill up to the first 32-byte boundary, which takes 6, 4, 2 or
 * 0 records for a header ending 8, 16, 24 or 0 bytes past one. Then, while
 * 16 or more records remain, groups of 16 records in 32 bytes, and the rest
 * in 8-byte groups again; an odd last record fills a whole group. For
 * example a header ending at offset 72 (8 past 64) with 40 records lays
 * records 0 to 5 in three 8-byte groups, 6 to 37 in two 32-byte groups and
 * 38 and 39 in one 8-byte group: 24 + 64 + 8 = 96 bytes.
 *
 * This is the layout under advice bit 0x01, the only compact form the
 * reader accepts. The record count is at most 2^55, the largest size over
 * the smallest block, so no product here overflows.
 */
void
erofs_compact_layout(uint64_t header_end, uint64_t records,
    erofs_compact_layout_t *out)
{
	uint64_t initial, remaining, wide;

	initial = ((32 - header_end % 32) / 4) & 7;
	if (initial >= records) {
		*out = (erofs_compact_layout_t){
			.records = records,
			.initial_records = records,
			.wide_records = 0,
			.total_bytes = (records + 1) / 2 * 8
		};
		return;
	}
	remaining = records - initial;
	wide = remaining / 16 * 16;
	*out = (erofs_compact_layout_t){
		.records = records,
		.initial_records = initial,
		.wide_records = wide,
		.total_bytes = initial * 4 + wide * 2 +
		    (remaining - wide + 1) / 2 * 8
	};
}

/*
 * The group holding record, below layout's count: its offset from the
 * header's end, its form, how many of its slots hold records (fewer than
 * two only for an odd last record) and the record's slot.
 */
void
erofs_compact_locate(const erofs_compact_layout_t *layout, uint64_t record,
    erofs_index_unit_t *out)
{
	uint64_t first, offset, within;

	if (record >= layout->initial_records &&
	    record < layout->initial_records + layout->wide_records) {
		within = record - layout->initial_records;
		*out = (erofs_index_unit_t){
			.offset = layout->initial_records * 4 +
			    within / 16 * 32,
			.bytes = 32,
			.slots = 16,
			.valid = 16,
			.slot = (unsigned)(within % 16)
		};
		return;
	}
	if (record < layout->initial_records) {
		first = record & ~(uint64_t)1;
		offset = first * 4;
	} else {
		within = record - layout->initial_records -
		    layout->wide_records;
		first = record - within % 2;
		offset = layout->initial_records * 4 +
		    layout->wide_records * 2 + (within - within % 2) * 4;
	}
	*out = (erofs_index_unit_t){
		.offset = offset,
		.bytes = 8,
		.slots = 2,
		.valid = layout->records - first < 2 ? 1 : 2,
		.slot = (unsigned)(record - first)
	};
}

/*
 * One compact group, decoded into its valid slots' records. A group is
 * fields of 16 bits (8-byte groups) or 14 bits (32-byte groups) packed
 * little-endian from bit 0, followed by a 32-bit anchor block. Each field
 * holds 12 bits of value, then 2 bits of type. A head's or plain record's
 * value is its clusterofs. A non-head's is its backward distance or a count
 * marker, except in the group's last slot, which holds the forward distance
 * of a non-head that is no marker, which the reader does not use; that
 * slot's backward distance is derived from the slot before it: one after a
 * head or plain record, two after a marker, else that slot's distance plus
 * one, so it is never zero.
 *
 * A compact head stores no block. It is the anchor plus a sum gathered by
 * walking back over the group's records before it, never leaving the group
 * (group_blocks_before()). big says whether the inode permits big
 * clusters, which changes both what the anchor counts and the walk.
 *
 * Every record of the group is checked, not only the one a lookup wants,
 * so a decoded group's verdict depends on its bytes alone: a zero backward
 * distance, a marker without big clusters or directly after a
 * non-head, a non-head right after its head without a
 * marker under big clusters, and a walk landing on the wrong record type
 * are corrupt. A count marker's value is the one exception: a zero count is
 * legal after the head of an inline tail or a fragment, which only the
 * lookup knows, so the lookup judges it (zmap.c, cluster_blocks()) and a
 * read that never reaches the cluster before such a marker does not.
 */
fsimg_status_t
erofs_compact_group_decode(const unsigned char *bytes,
    const erofs_index_unit_t *group, bool big, erofs_zrecord_t *out,
    const char **what)
{
	erofs_zrecord_t records[EROFS_GROUP_RECORDS_MAX];
	const erofs_zrecord_t *before;
	uint64_t blocks;
	uint32_t anchor;
	unsigned last = group->slots - 1, value;
	fsimg_status_t status;

	bzero(records, sizeof(records));
	for (unsigned i = 0; i < group->valid; i++) {
		erofs_zrecord_t *record = &records[i];

		value = compact_field(bytes, group->slots, i, &record->type);
		if (record->type != EROFS_RECORD_NONHEAD) {
			record->clusterofs = (uint16_t)value;
			continue;
		}
		before = i > 0 ? &records[i - 1] : NULL;
		if ((value & RECORD_MARKER) != 0) {
			if (!big)
				return (reject(FSIMG_ECORRUPT,
				    "count marker without big clusters", what));
			if (before != NULL &&
			    before->type == EROFS_RECORD_NONHEAD)
				return (reject(FSIMG_ECORRUPT,
				    "count marker after a non-head record",
				    what));
			record->marker = true;
			record->marker_blocks = value & RECORD_COUNT_MASK;
			record->backward = 1;
			continue;
		}
		if (i == last) {
			/* last >= 1, so the slot before exists. */
			if (before->type != EROFS_RECORD_NONHEAD)
				record->backward = 1;
			else if (before->marker)
				record->backward = 2;
			else
				record->backward = before->backward + 1;
		} else
			record->backward = (uint16_t)value;
		if (record->backward == 0)
			return (reject(FSIMG_ECORRUPT,
			    "index backward distance zero", what));
		if (big && record->backward == 1)
			return (reject(FSIMG_ECORRUPT,
			    "non-head record after its head without a count "
			    "marker", what));
	}

	anchor = fsimg_load_le32(bytes + group->bytes - 4);
	for (unsigned i = 0; i < group->valid; i++) {
		if (records[i].type == EROFS_RECORD_NONHEAD)
			continue;
		status = group_blocks_before(records, i, big, &blocks, what);
		if (status != FSIMG_OK)
			return (status);
		records[i].block = (uint64_t)anchor + blocks;
	}

	bcopy(records, out, group->valid * sizeof(records[0]));
	return (FSIMG_OK);
}

/*
 * The value and type of a compact group's field slot. A field spans at
 * most three bytes from its first, all inside the group: the last field
 * of an 8-byte group ends at byte 4, of a 32-byte group at byte 27, and
 * the anchor bytes a read may cover are masked off.
 */
static unsigned
compact_field(const unsigned char *bytes, unsigned slots, unsigned slot,
    erofs_record_type_t *type)
{
	unsigned bit = slot * (slots == 2 ? 16u : 14u);
	uint32_t word;

	word = (uint32_t)bytes[bit / 8] | (uint32_t)bytes[bit / 8 + 1] << 8 |
	    (uint32_t)bytes[bit / 8 + 2] << 16;
	word >>= bit % 8;
	*type = (erofs_record_type_t)((word >> 12) & RECORD_TYPE_MASK);
	return (word & 0x0fffu);
}

/*
 * The blocks a compact head at slot target lies past its group's anchor.
 * The walk visits the records before it, from the nearest back to the
 * group's first, and stops at a jump that would land before the first.
 *
 * Without big clusters the anchor is the block of the last cluster before
 * the group, every cluster is one block, and the sum starts at one: each
 * head or plain record met adds one, and a non-head jumps back by its
 * distance to its own head, which is then met. With big clusters the anchor
 * is the first block of the first cluster the group must still account for,
 * and the sum starts at zero: a marker adds its cluster's count and skips
 * the head directly before it, a head met directly is a one-block cluster
 * without a marker and adds one, and a non-head that is no marker jumps to
 * its cluster's marker, its distance less one back. A cluster whose marker
 * lies before the group is in the anchor and is never met. For example,
 * with big clusters, records head, marker 4, non-head, head after an anchor
 * of 21 put the second head at block 25.
 *
 * Every step moves at least one record back, so the walk ends.
 */
static fsimg_status_t
group_blocks_before(const erofs_zrecord_t *records, unsigned target,
    bool big, uint64_t *out, const char **what)
{
	const erofs_zrecord_t *record;
	uint64_t sum = big ? 0 : 1;
	int next, visit = (int)target - 1;

	while (visit >= 0) {
		record = &records[visit];
		if (record->type != EROFS_RECORD_NONHEAD) {
			sum++;
			visit--;
			continue;
		}
		if (record->marker) {
			sum += record->marker_blocks;
			visit -= 2;
			continue;
		}
		next = visit - (int)record->backward + (big ? 1 : 0);
		if (next < 0)
			break;
		if (big ? !records[next].marker :
		    records[next].type == EROFS_RECORD_NONHEAD)
			return (reject(FSIMG_ECORRUPT,
			    "index distance lands on the wrong record type",
			    what));
		visit = next;
	}
	*out = sum;
	return (FSIMG_OK);
}

/*
 * A whole directory block, length its valid length. The block starts with
 * 12-byte entries, and the first entry's name offset, where the names
 * begin, gives their count: it is nonzero, a multiple of 12 and inside the
 * valid length. Every entry is then checked, so a decoded block holds only
 * valid entries and erofs_dirent_decode() cannot fail on it. Rules across
 * blocks, such as name order, are the caller's.
 */
fsimg_status_t
erofs_dirblock_decode(const unsigned char *bytes, size_t length,
    erofs_dirblock_t *out, const char **what)
{
	erofs_dirent_t dirent;
	fsimg_status_t status;
	uint16_t names_start;
	size_t count;

	if (length < EROFS_DIRENT_BYTES)
		return (reject(FSIMG_ECORRUPT, "directory block truncated",
		    what));
	names_start = fsimg_load_le16(bytes + 8);
	if (names_start == 0 || names_start % EROFS_DIRENT_BYTES != 0 ||
	    names_start >= length)
		return (reject(FSIMG_ECORRUPT, "directory entry count", what));
	count = names_start / EROFS_DIRENT_BYTES;

	for (size_t index = 0; index < count; index++) {
		status = dirent_parse(bytes, length, count, index, &dirent,
		    what);
		if (status != FSIMG_OK)
			return (status);
	}

	out->bytes = bytes;
	out->length = length;
	out->count = count;
	return (FSIMG_OK);
}

/*
 * Entry index of a block erofs_dirblock_decode() accepted. Only an index
 * past the entry array fails, as the caller's error FSIMG_EINVAL.
 */
fsimg_status_t
erofs_dirent_decode(const erofs_dirblock_t *block, size_t index,
    erofs_dirent_t *out, const char **what)
{
	if (index >= block->count)
		return (reject(FSIMG_EINVAL, "directory entry index", what));
	return (dirent_parse(block->bytes, block->length, block->count, index,
	    out, what));
}

/*
 * The header of an inode's attribute region, length its size from the
 * attribute count: a 32-bit name filter, the shared count and seven
 * reserved bytes, then the shared indexes, which must fit the region.
 */
fsimg_status_t
erofs_xattr_header_decode(const unsigned char *region, size_t length,
    erofs_xattr_header_t *out, const char **what)
{
	uint32_t entries_offset;
	uint8_t shared_count;

	if (length < EROFS_XATTR_HEADER_BYTES)
		return (reject(FSIMG_ECORRUPT, "attribute region truncated",
		    what));
	shared_count = region[4];
	entries_offset = EROFS_XATTR_HEADER_BYTES + 4 * (uint32_t)shared_count;
	if (entries_offset > length)
		return (reject(FSIMG_ECORRUPT, "shared attribute count", what));

	bzero(out, sizeof(*out));
	out->filter = fsimg_load_le32(region);
	out->shared_count = shared_count;
	out->entries_offset = entries_offset;
	return (FSIMG_OK);
}

/*
 * The index-th shared index of a region whose header decoded into header;
 * false for an index at or past its shared count. The header's decode
 * proved that every shared index lies inside the region.
 */
bool
erofs_xattr_shared_id(const unsigned char *region,
    const erofs_xattr_header_t *header, unsigned index, uint32_t *id)
{
	if (index >= header->shared_count)
		return (false);
	*id = fsimg_load_le32(region + EROFS_XATTR_HEADER_BYTES +
	    4 * (size_t)index);
	return (true);
}

/*
 * Where shared index id's entry starts: xattr_blkaddr * B + 4 * id, in the
 * volume or, under SHARED_EA_IN_METABOX, in the metadata box's content.
 * Both terms are below 2^44 and 2^34, so the sum cannot overflow.
 */
uint64_t
erofs_xattr_shared_offset(const erofs_super_t *sb, uint32_t id)
{
	return (((uint64_t)sb->xattr_blkaddr << sb->blkszbits) +
	    4 * (uint64_t)id);
}

static bool
builtin_prefix(uint8_t index)
{
	switch (index) {
	case 1:		/* user. */
	case 2:		/* system.posix_acl_access */
	case 3:		/* system.posix_acl_default */
	case 4:		/* trusted. */
	case 6:		/* security. */
		return (true);
	default:
		return (false);
	}
}

/*
 * One attribute entry, inline or shared; length is what remains of its
 * region or space. The entry is a suffix length, a name index and a value
 * length, then the suffix and the value, padded to 4 bytes, and the padded
 * entry must fit. A name index below 0x80 selects a built-in prefix, and
 * the two ACL indexes name complete attributes with an empty suffix; index
 * 0x80 | k names the k-th long prefix, which must exist. Another index has
 * no published meaning and is unsupported.
 */
fsimg_status_t
erofs_xattr_decode(const unsigned char *raw, size_t length,
    const erofs_super_t *sb, erofs_xattr_t *out, const char **what)
{
	erofs_xattr_t xattr;
	uint32_t entry_bytes;

	if (length < EROFS_XATTR_ENTRY_BYTES)
		return (reject(FSIMG_ECORRUPT, "attribute entry truncated",
		    what));
	bzero(&xattr, sizeof(xattr));
	xattr.suffix_length = raw[0];
	xattr.name_index = raw[1];
	xattr.value_length = fsimg_load_le16(raw + 2);
	entry_bytes = EROFS_XATTR_ENTRY_BYTES + xattr.suffix_length +
	    (uint32_t)xattr.value_length;
	xattr.entry_bytes = (entry_bytes + 3) & ~UINT32_C(3);
	if (xattr.entry_bytes > length)
		return (reject(FSIMG_ECORRUPT, "attribute entry length", what));

	if ((xattr.name_index & EROFS_XATTR_LONG_PREFIX) != 0) {
		if ((sb->incompat & EROFS_INCOMPAT_XATTR_PREFIXES) == 0)
			return (reject(FSIMG_ECORRUPT,
			    "long prefix without XATTR_PREFIXES", what));
		if ((xattr.name_index & ~EROFS_XATTR_LONG_PREFIX) >=
		    sb->xattr_prefix_count)
			return (reject(FSIMG_ECORRUPT, "long prefix index",
			    what));
	} else if (!builtin_prefix(xattr.name_index)) {
		return (reject(FSIMG_EUNSUPPORTED, "attribute name index",
		    what));
	} else if ((xattr.name_index == 2 || xattr.name_index == 3) &&
	    xattr.suffix_length != 0) {
		return (reject(FSIMG_ECORRUPT, "ACL attribute suffix", what));
	}

	*out = xattr;
	return (FSIMG_OK);
}

/*
 * One long-prefix record: a 16-bit length L, a base index naming a
 * built-in prefix, then L - 1 infix bytes, padded to 4 bytes. The full
 * prefix is the built-in prefix followed by the infix. length is what
 * remains of the table's region.
 */
fsimg_status_t
erofs_prefix_decode(const unsigned char *raw, size_t length,
    erofs_prefix_t *out, const char **what)
{
	erofs_prefix_t prefix;
	uint16_t prefix_length;

	if (length < 2)
		return (reject(FSIMG_ECORRUPT, "long prefix truncated", what));
	bzero(&prefix, sizeof(prefix));
	prefix_length = fsimg_load_le16(raw);
	if (prefix_length == 0)
		return (reject(FSIMG_ECORRUPT, "long prefix length", what));
	prefix.record_bytes = (2 + (uint32_t)prefix_length + 3) &
	    ~UINT32_C(3);
	if (prefix.record_bytes > length)
		return (reject(FSIMG_ECORRUPT, "long prefix record", what));
	prefix.base_index = raw[2];
	if (!builtin_prefix(prefix.base_index))
		return (reject(FSIMG_ECORRUPT, "long prefix base index", what));
	prefix.infix_length = (uint16_t)(prefix_length - 1);

	*out = prefix;
	return (FSIMG_OK);
}

/*
 * XXH32, from the algorithm's published description. All arithmetic is
 * modulo 2^32 and input words are little-endian. A message of at least 16
 * bytes is consumed in 16-byte stripes by four accumulators, each taking
 * one 4-byte lane per stripe, which then merge into one; a shorter message
 * starts from a single accumulator instead. The total length is added, the
 * last 0 to 15 bytes are mixed in as 4-byte words and then single bytes,
 * and a final avalanche spreads every input bit across the result.
 * rotl32() rotates by 1 to 31 bits; every caller passes a constant.
 */
static uint32_t
rotl32(uint32_t value, unsigned bits)
{
	return (value << bits | value >> (32 - bits));
}

/* One lane of one stripe into its accumulator. */
static uint32_t
xxh32_round(uint32_t accumulator, uint32_t lane)
{
	accumulator += lane * XXH32_PRIME2;
	accumulator = rotl32(accumulator, 13);
	return (accumulator * XXH32_PRIME1);
}

static void
xxh32_init(xxh32_state_t *state, uint32_t seed)
{
	bzero(state, sizeof(*state));
	state->seed = seed;
	state->accumulator[0] = seed + XXH32_PRIME1 + XXH32_PRIME2;
	state->accumulator[1] = seed + XXH32_PRIME2;
	state->accumulator[2] = seed;
	state->accumulator[3] = seed - XXH32_PRIME1;
}

static void
xxh32_absorb(xxh32_state_t *state, const unsigned char *bytes, size_t length)
{
	size_t take;

	state->total_bytes += length;
	while (length > 0) {
		take = XXH32_STRIPE_BYTES - state->stripe_bytes;
		if (take > length)
			take = length;
		bcopy(bytes, state->stripe + state->stripe_bytes, take);
		state->stripe_bytes += take;
		bytes += take;
		length -= take;

		if (state->stripe_bytes < XXH32_STRIPE_BYTES)
			break;
		for (unsigned lane = 0; lane < 4; lane++)
			state->accumulator[lane] = xxh32_round(
			    state->accumulator[lane],
			    fsimg_load_le32(state->stripe + 4 * lane));
		state->stripe_bytes = 0;
	}
}

static uint32_t
xxh32_finish(const xxh32_state_t *state)
{
	const unsigned char *rest = state->stripe;
	size_t rest_bytes = state->stripe_bytes;
	uint32_t hash;

	if (state->total_bytes >= XXH32_STRIPE_BYTES)
		hash = rotl32(state->accumulator[0], 1) +
		    rotl32(state->accumulator[1], 7) +
		    rotl32(state->accumulator[2], 12) +
		    rotl32(state->accumulator[3], 18);
	else
		hash = state->seed + XXH32_PRIME5;
	hash += (uint32_t)state->total_bytes;

	for (; rest_bytes >= 4; rest += 4, rest_bytes -= 4) {
		hash += fsimg_load_le32(rest) * XXH32_PRIME3;
		hash = rotl32(hash, 17) * XXH32_PRIME4;
	}
	for (; rest_bytes > 0; rest++, rest_bytes--) {
		hash += *rest * XXH32_PRIME5;
		hash = rotl32(hash, 11) * XXH32_PRIME1;
	}

	hash ^= hash >> 15;
	hash *= XXH32_PRIME2;
	hash ^= hash >> 13;
	hash *= XXH32_PRIME3;
	hash ^= hash >> 16;
	return (hash);
}

/* XXH32 of first followed by second, either of which may be empty. */
uint32_t
erofs_xxh32(const unsigned char *first, size_t first_length,
    const unsigned char *second, size_t second_length, uint32_t seed)
{
	xxh32_state_t state;

	xxh32_init(&state, seed);
	xxh32_absorb(&state, first, first_length);
	xxh32_absorb(&state, second, second_length);
	return (xxh32_finish(&state));
}

/*
 * The name-filter hash of an attribute: XXH32 of its name without the
 * built-in prefix, seeded with the filter seed plus that prefix's index,
 * and its bit the hash modulo 32. For a built-in index the hashed name is
 * the stored suffix; for a long prefix it is the prefix's infix followed
 * by the suffix, and base_index is the prefix's base index.
 */
void
erofs_name_hash_init(erofs_name_hash_t *hash, uint8_t base_index)
{
	xxh32_init(hash, FILTER_SEED + base_index);
}

void
erofs_name_hash_absorb(erofs_name_hash_t *hash, const unsigned char *bytes,
    size_t length)
{
	xxh32_absorb(hash, bytes, length);
}

unsigned
erofs_name_hash_bit(const erofs_name_hash_t *hash)
{
	return (xxh32_finish(hash) & 31);
}
