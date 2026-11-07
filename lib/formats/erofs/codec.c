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
#define LZMA_DICT_MIN		UINT32_C(4096)
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

static bool		all_zero(const unsigned char *, size_t);
static bool		builtin_prefix(uint8_t);
static fsimg_status_t	compact_decode(const unsigned char *,
			    const erofs_super_t *, bool, erofs_inode_t *,
			    const char **);
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
 * whole block as its tail, which is corrupt. A compressed inode's map
 * header and index are not decoded, so its body length is unknown and the
 * layout is unsupported.
 */
static fsimg_status_t
inode_body_measure(const erofs_super_t *sb, erofs_inode_t *inode,
    const char **what)
{
	erofs_chunk_format_t *chunk_format = &inode->chunk_format;
	fsimg_status_t status;
	uint64_t block_bytes, chunk_count, index_bytes, index_start;
	uint64_t attributes_end;

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
		return (reject(FSIMG_EUNSUPPORTED, "compressed data layout",
		    what));
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
