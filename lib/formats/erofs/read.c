/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "erofs.h"
#include "unicode.h"

/*
 * One bit of a feature word: its name, "A/B" for a bit the format gives two
 * names, and whether this reader reads a volume that sets it.
 */
typedef struct erofs_feature {
	uint32_t	 bit;
	const char	*name;
	bool		 readable;
} erofs_feature_t;

/*
 * The closed tables of both words, in bit order. Every compatible bit is
 * readable, since the format lets a reader ignore what it does not use. An
 * incompatible bit is readable when this driver reads the structures it
 * adds. Bits outside the incompatible table never reach open: the probe's
 * checks refuse them first.
 */
static const erofs_feature_t compat_features[] = {
	{ EROFS_COMPAT_SB_CHKSUM, "SB_CHKSUM", true },
	{ EROFS_COMPAT_MTIME, "MTIME", true },
	{ EROFS_COMPAT_XATTR_FILTER, "XATTR_FILTER", true },
	{ EROFS_COMPAT_SHARED_EA_IN_METABOX, "SHARED_EA_IN_METABOX", true },
	{ EROFS_COMPAT_PLAIN_XATTR_PFX, "PLAIN_XATTR_PFX", true },
	{ EROFS_COMPAT_ISHARE_XATTRS, "ISHARE_XATTRS", true }
};

static const erofs_feature_t incompat_features[] = {
	{ EROFS_INCOMPAT_LZ4_0PADDING, "LZ4_0PADDING", false },
	{ EROFS_INCOMPAT_COMPR_CFGS, "COMPR_CFGS/BIG_PCLUSTER", false },
	{ EROFS_INCOMPAT_CHUNKED_FILE, "CHUNKED_FILE", false },
	{ EROFS_INCOMPAT_DEVICE_TABLE, "DEVICE_TABLE/COMPR_HEAD2", false },
	{ EROFS_INCOMPAT_ZTAILPACKING, "ZTAILPACKING", false },
	{ EROFS_INCOMPAT_FRAGMENTS, "FRAGMENTS/DEDUPE", false },
	{ EROFS_INCOMPAT_XATTR_PREFIXES, "XATTR_PREFIXES", true },
	{ EROFS_INCOMPAT_48BIT, "48BIT", false },
	{ EROFS_INCOMPAT_METABOX, "METABOX", false }
};

#define FEATURE_COUNT(table)	(sizeof(table) / sizeof((table)[0]))

/* The volume information's named values, in this order. */
enum {
	DETAIL_BLOCK_COUNT,
	DETAIL_INODE_COUNT,
	DETAIL_EPOCH_NSEC,
	DETAIL_COMPAT,
	DETAIL_INCOMPAT,
	DETAIL_BUILD_TIME,
	DETAIL_ISHARE_PREFIX,
	DETAIL_RESERVED_NONZERO,
	DETAIL_COUNT
};

_Static_assert(DETAIL_COUNT <= FSIMG_VOLUME_DETAILS_MAX,
    "the EROFS details fit the volume information");
/* Four texts at most: the epoch, both feature lists and the raw name. */
_Static_assert(4 <= FSIMG_VOLUME_TEXTS_MAX,
    "the EROFS texts fit the volume information");
_Static_assert(sizeof(((erofs_super_t *)NULL)->name) ==
    EROFS_NAME_FIELD_BYTES, "the name field holds the superblock's name");
/* An escaped name byte takes at most four characters. */
_Static_assert(4 * EROFS_NAME_FIELD_BYTES <
    sizeof(((fsimg_volume_text_t *)NULL)->value),
    "an escaped volume name fits a text value");
_Static_assert(EROFS_NAME_FIELD_BYTES <
    sizeof(((fsimg_volume_info_t *)NULL)->label),
    "the volume name fits the label");

static fsimg_status_t	domain_set(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t	erofs_open(fsimg_reader_t *,
			    const fsimg_format_variant_t *, fsimg_error_t *);
static void		erofs_close(fsimg_reader_t *);
static fsimg_status_t	features_decide(fsimg_erofs_reader_t *,
			    fsimg_error_t *);
static uint32_t		features_named(const erofs_feature_t *, size_t);
static void		features_text(const erofs_feature_t *, size_t,
			    uint32_t, char *, size_t);
static void		info_report(fsimg_reader_t *);
static bool		label_printable(const unsigned char *, size_t);
static void		name_escape(const unsigned char *, size_t, char *,
			    size_t);
static fsimg_status_t	super_read(fsimg_reader_t *, fsimg_error_t *);
static void		text_add(fsimg_volume_info_t *, const char *,
			    const char *);
static void		text_append(char *, size_t, const char *);
static void		texts_report(const erofs_super_t *,
			    fsimg_volume_info_t *);

/*
 * The reader: open and close here, the directory callbacks in directory.c,
 * resolve in content.c and full verification in verify.c.
 */
const fsimg_reader_ops_t fsimg_erofs_reader_ops = {
	.open = erofs_open,
	.close = erofs_close,
	.prepare = erofs_prepare,
	.replay = erofs_replay,
	.keys = erofs_keys,
	.key = erofs_key,
	.ambiguous = "duplicate EROFS directory name",
	.name_profile = FSIMG_NAME_PROFILE_PORTABLE,
	.resolve = erofs_resolve,
	.verify = erofs_verify
};

fsimg_erofs_reader_t *
erofs_state(const fsimg_reader_t *reader)
{
	return (reader->driver);
}

/*
 * Report a rejection with the static phrase naming the field or rule, as
 * "invalid EROFS superblock checksum" or "unsupported EROFS extra devices".
 * Any other status keeps the phrase with the format's name.
 */
fsimg_status_t
erofs_reject(fsimg_error_t *error, fsimg_status_t status, const char *what)
{
	char message[sizeof(((fsimg_error_t *)NULL)->message)];
	const char *verdict;

	switch (status) {
	case FSIMG_ECORRUPT:
		verdict = "invalid ";
		break;
	case FSIMG_EUNSUPPORTED:
		verdict = "unsupported ";
		break;
	default:
		verdict = "";
		break;
	}
	snprintf(message, sizeof(message), "%sEROFS %s", verdict, what);
	return (fsimg_error_set(error, status, 0, FSIMG_OP_READER, message));
}

/*
 * Open in the order the format fixes: the superblock region and every
 * superblock rule, the address domain, the feature decision, then the root.
 * A refused feature fails before any inode is read, so no partial view of a
 * refused volume exists. Open reads nothing else: no directory, attribute
 * pool or prefix table.
 */
static fsimg_status_t
erofs_open(fsimg_reader_t *reader,
    const fsimg_format_variant_t *forced FSIMG_UNUSED, fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs;
	erofs_inode_t root;
	fsimg_error_t detail;
	fsimg_status_t status;

	erofs = fsimg_alloc(reader->ctx, sizeof(*erofs), &detail);
	if (erofs == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(erofs, sizeof(*erofs));
	reader->driver = erofs;

	status = super_read(reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = domain_set(reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = features_decide(erofs, error);
	if (status != FSIMG_OK)
		return (status);

	status = erofs_inode_load(reader, erofs->super.root_nid, &root, error);
	if (status != FSIMG_OK)
		return (status);
	if (root.kind != FSIMG_ENTRY_DIRECTORY)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "root inode is not a directory"));

	erofs->tail_window = fsimg_alloc(reader->ctx, erofs->block_bytes,
	    &detail);
	if (erofs->tail_window == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}

	info_report(reader);
	return (FSIMG_OK);
}

static void
erofs_close(fsimg_reader_t *reader)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);

	if (erofs == NULL)
		return;
	fsimg_dealloc(erofs->tail_window);
	fsimg_dealloc(erofs);
	reader->driver = NULL;
}

/*
 * The superblock region, [1024, min(4096, input end)), and every superblock
 * rule erofs_region_decode() applies, in the format's order. The domain is
 * not known yet, so this one read is bounded by the region alone. The magic
 * is the identity check a forced open waives; every other rule stands.
 */
static fsimg_status_t
super_read(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	unsigned char region[EROFS_SUPER_REGION_BYTES];
	const char *what;
	size_t length, transferred;
	fsimg_status_t status;

	length = 0;
	if (reader->slice_bytes > EROFS_SUPER_OFFSET) {
		length = EROFS_SUPER_REGION_BYTES;
		if (reader->slice_bytes - EROFS_SUPER_OFFSET < length)
			length = (size_t)(reader->slice_bytes -
			    EROFS_SUPER_OFFSET);
	}
	/*
	 * An input ending before byte 1024 has no region to read; the
	 * identity check and the decoder then judge the empty region.
	 */
	if (length != 0) {
		status = fsimg_reader_bytes(reader, EROFS_SUPER_OFFSET, region,
		    length, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		if (transferred != length)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "superblock region truncated"));
	}

	status = fsimg_reader_identity(reader,
	    erofs_magic_matches(region, length),
	    "no EROFS superblock magic", "EROFS superblock magic", error);
	if (status != FSIMG_OK)
		return (status);
	status = erofs_region_decode(region, length, &erofs->super,
	    &erofs->config, &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));
	erofs->block_bytes = UINT32_C(1) << erofs->super.blkszbits;
	return (FSIMG_OK);
}

/*
 * The address domain and what the volume declares. A nonzero block count
 * declares blocks * B bytes, at most 2^60, so the product cannot overflow;
 * when that passes the input's end the reader core reports the difference
 * as missing bytes, and only a structure read there fails. A zero count
 * declares nothing, and the domain is the input. The superblock is a
 * structure like any other, so a domain ending inside it is corrupt.
 * Content lies in whole blocks of the domain.
 */
static fsimg_status_t
domain_set(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	uint64_t super_end;

	if (erofs->super.block_count != 0)
		erofs->domain_bytes = erofs->super.block_count <<
		    erofs->super.blkszbits;
	else
		erofs->domain_bytes = reader->slice_bytes;
	super_end = EROFS_SUPER_OFFSET + EROFS_SUPER_BASE_BYTES +
	    (uint64_t)erofs->super.extension_slots * EROFS_SUPER_SLOT_BYTES;
	if (erofs->domain_bytes < super_end)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "block count ends inside the superblock"));

	reader->info.volume_bytes = erofs->domain_bytes;
	reader->content_domain = (fsimg_extent_domain_t){
		.offset = 0,
		.length = erofs->domain_bytes - erofs->domain_bytes %
		    erofs->block_bytes,
		.unit_bytes = erofs->block_bytes
	};
	return (FSIMG_OK);
}

/*
 * Decide the features once from the decoded words. The pairing rule comes
 * first: a long-prefix table lives in a plain region or in the packed
 * inode, and without either the volume is corrupt. Then each incompatible
 * bit this reader does not implement is refused, naming the bit, so a
 * volume using it never opens.
 */
static fsimg_status_t
features_decide(fsimg_erofs_reader_t *erofs, fsimg_error_t *error)
{
	const erofs_super_t *sb = &erofs->super;
	erofs_features_t *features = &erofs->features;
	char phrase[sizeof(((fsimg_error_t *)NULL)->message)];
	uint32_t named_compat;

	if ((sb->incompat & EROFS_INCOMPAT_XATTR_PREFIXES) != 0 &&
	    (sb->compat & EROFS_COMPAT_PLAIN_XATTR_PFX) == 0 &&
	    (sb->incompat & EROFS_INCOMPAT_FRAGMENTS) == 0)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "long prefixes without a plain table or a packed inode"));

	for (size_t i = 0; i < FEATURE_COUNT(incompat_features); i++) {
		const erofs_feature_t *feature = &incompat_features[i];

		if ((sb->incompat & feature->bit) == 0 || feature->readable)
			continue;
		snprintf(phrase, sizeof(phrase), "feature %s", feature->name);
		return (erofs_reject(error, FSIMG_EUNSUPPORTED, phrase));
	}

	features->checksum = (sb->compat & EROFS_COMPAT_SB_CHKSUM) != 0;
	features->mtime = (sb->compat & EROFS_COMPAT_MTIME) != 0;
	features->xattr_filter = (sb->compat & EROFS_COMPAT_XATTR_FILTER) != 0;
	features->long_prefixes =
	    (sb->incompat & EROFS_INCOMPAT_XATTR_PREFIXES) != 0;
	features->plain_prefixes =
	    (sb->compat & EROFS_COMPAT_PLAIN_XATTR_PFX) != 0;
	named_compat = features_named(compat_features,
	    FEATURE_COUNT(compat_features));
	features->unknown_compat = (sb->compat & ~named_compat) != 0;
	return (FSIMG_OK);
}

/*
 * The volume information, from the superblock and the feature decision:
 * geometry in blocks of the domain, the UUID as stored, and the format's
 * own values as details and texts. fsimg_reader_open() zeroes the reader
 * before open, so every field not set here, and every detail and text past
 * the counts, is zero.
 */
static void
info_report(fsimg_reader_t *reader)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	const erofs_super_t *sb = &erofs->super;
	fsimg_volume_info_t *info = &reader->info;
	static const char *const detail_names[DETAIL_COUNT] = {
		[DETAIL_BLOCK_COUNT] = "block_count",
		[DETAIL_INODE_COUNT] = "inode_count",
		[DETAIL_EPOCH_NSEC] = "epoch_nsec",
		[DETAIL_COMPAT] = "compat",
		[DETAIL_INCOMPAT] = "incompat",
		[DETAIL_BUILD_TIME] = "build_time",
		[DETAIL_ISHARE_PREFIX] = "ishare_prefix",
		[DETAIL_RESERVED_NONZERO] = "reserved_nonzero"
	};
	const uint64_t detail_values[DETAIL_COUNT] = {
		[DETAIL_BLOCK_COUNT] = sb->block_count,
		[DETAIL_INODE_COUNT] = sb->inode_count,
		[DETAIL_EPOCH_NSEC] = sb->epoch_nsec,
		[DETAIL_COMPAT] = sb->compat,
		[DETAIL_INCOMPAT] = sb->incompat,
		[DETAIL_BUILD_TIME] = sb->build_time,
		[DETAIL_ISHARE_PREFIX] = sb->ishare_prefix,
		[DETAIL_RESERVED_NONZERO] = sb->reserved_nonzero ||
		    erofs->config.reserved_nonzero
	};

	strcpy(info->format, "erofs");
	strcpy(info->profile, "core");
	info->valid |= FSIMG_VOLUME_GEOMETRY | FSIMG_VOLUME_UUID;
	bcopy(sb->uuid, info->uuid, sizeof(info->uuid));
	info->sector_bytes = erofs->block_bytes;
	info->allocation_unit_bytes = erofs->block_bytes;
	info->allocation_units = erofs->domain_bytes >> sb->blkszbits;
	info->time_basis = FSIMG_TIME_UTC;
	info->scope = FSIMG_VALIDATION_HEADER;
	/* A read-only format records no mount state. */
	info->clean = true;
	info->io_error = false;

	for (unsigned i = 0; i < DETAIL_COUNT; i++) {
		strcpy(info->details[i].name, detail_names[i]);
		info->details[i].value = detail_values[i];
	}
	info->detail_count = DETAIL_COUNT;
	texts_report(sb, info);
}

/*
 * The named texts: the signed epoch, which a detail's unsigned value
 * cannot hold, and both feature words by name. The volume name is the
 * label when it is printable UTF-8, and otherwise, unless empty, a text
 * showing its bytes escaped.
 */
static void
texts_report(const erofs_super_t *sb, fsimg_volume_info_t *info)
{
	char text[sizeof(((fsimg_volume_text_t *)NULL)->value)];

	snprintf(text, sizeof(text), "%" PRId64, sb->epoch);
	text_add(info, "epoch", text);
	features_text(compat_features, FEATURE_COUNT(compat_features),
	    sb->compat, text, sizeof(text));
	text_add(info, "compat_features", text);
	features_text(incompat_features, FEATURE_COUNT(incompat_features),
	    sb->incompat, text, sizeof(text));
	text_add(info, "incompat_features", text);

	if (label_printable(sb->name, sb->name_length)) {
		info->valid |= FSIMG_VOLUME_LABEL;
		bcopy(sb->name, info->label, sb->name_length);
	} else if (sb->name_length != 0) {
		name_escape(sb->name, sb->name_length, text, sizeof(text));
		text_add(info, "name", text);
	}
}

/* The bits a feature table names. */
static uint32_t
features_named(const erofs_feature_t *table, size_t count)
{
	uint32_t named = 0;

	for (size_t i = 0; i < count; i++)
		named |= table[i].bit;
	return (named);
}

/*
 * The names of the bits set in word, separated by spaces, then the bits
 * outside the table as one hexadecimal mask; empty when no bit is set.
 * Every name and the mask together are shorter than a text value, and
 * text_append() clips rather than overruns regardless.
 */
static void
features_text(const erofs_feature_t *table, size_t count, uint32_t word,
    char *out, size_t size)
{
	char unknown[sizeof("0xffffffff")];
	uint32_t other;

	out[0] = '\0';
	for (size_t i = 0; i < count; i++) {
		if ((word & table[i].bit) != 0)
			text_append(out, size, table[i].name);
	}
	other = word & ~features_named(table, count);
	if (other != 0) {
		snprintf(unknown, sizeof(unknown), "0x%" PRIx32, other);
		text_append(out, size, unknown);
	}
}

/*
 * A label is valid UTF-8 without a control character, U+0080 to U+009F
 * included, and not empty.
 */
static bool
label_printable(const unsigned char *bytes, size_t length)
{
	size_t units;

	if (length == 0 || fsimg_utf8_to_utf16((const char *)bytes, length,
	    NULL, 0, &units, NULL) != FSIMG_OK)
		return (false);
	for (size_t i = 0; i < length; i++) {
		if (bytes[i] < 0x20 || bytes[i] == 0x7f ||
		    (bytes[i] == 0xc2 && i + 1 < length &&
		    bytes[i + 1] >= 0x80 && bytes[i + 1] < 0xa0))
			return (false);
	}
	return (true);
}

/*
 * The raw name in printable ASCII: a printable byte as itself, a backslash
 * doubled and any other byte as \xHH. The assertions at the top prove a
 * whole name fits a text value; the loop also stops before a byte that
 * would not fit, so used never passes size.
 */
static void
name_escape(const unsigned char *bytes, size_t length, char *out,
    size_t size)
{
	size_t used = 0;

	out[0] = '\0';
	for (size_t i = 0; i < length; i++) {
		/* Stop rather than clip: a byte takes at most four characters. */
		if (used + 4 >= size)
			break;
		if (bytes[i] == '\\')
			used += snprintf(out + used, size - used, "\\\\");
		else if (bytes[i] >= 0x20 && bytes[i] < 0x7f)
			used += snprintf(out + used, size - used, "%c",
			    bytes[i]);
		else
			used += snprintf(out + used, size - used, "\\x%02x",
			    bytes[i]);
	}
}

/* Append piece to the list in out, after a space unless it is the first. */
static void
text_append(char *out, size_t size, const char *piece)
{
	size_t used = strlen(out);

	if (used != 0 && used + 1 < size)
		out[used++] = ' ';
	snprintf(out + used, size - used, "%s", piece);
}

/* Append one named text; an empty value is not reported. */
static void
text_add(fsimg_volume_info_t *info, const char *name, const char *value)
{
	fsimg_volume_text_t *text;

	if (value[0] == '\0')
		return;
	text = &info->texts[info->text_count++];
	strcpy(text->name, name);
	strcpy(text->value, value);
}
