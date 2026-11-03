/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "iso9660.h"

/*
 * Rock Ridge entries (specs/design-iso9660.md#rock-ridge). iso_rr_decode()
 * is the one decoder of a record's SUSP stream; it fills an iso_rr_t,
 * which owns what it decodes: PX owner, mode and file type; PN device
 * number; TF times; the name its NM entries assemble; the link target its
 * SL entries assemble; and the CL, PL and RE relocation pointers. Every
 * length, version and repetition is checked as the entry table states.
 * rr_kind() then runs the kind checks against PX and the record's
 * directory flag. What the profile refuses (SF, ZF, a volume-root or host link component, a name
 * or target over 1024 bytes, a type outside the seven POSIX types) is not
 * returned from here: it is noted in iso_rr_t.refusal, the record is left
 * without a kind, and the caller that finishes the record, after its own
 * checks of the record in context, reports it through iso_rr_refusal().
 * So corruption in the stream or in the record's context is reported
 * before such a refusal. The one exception is a SUSP profile limit, which
 * susp.c reports at once: the stream cannot be read past it, and every
 * later check depends on what it holds.
 *
 * A name or target is assembled across entries: NM and SL entries carry a
 * continue flag, every one but the last of a record must carry it, and no
 * name or target begins again after it ends. Within a target, an SL
 * component's own continue flag joins the next component without a '/',
 * possibly across entries.
 */
#define RR_FLAG_CONTINUE	0x01u
#define NM_CURRENT		0x02u
#define NM_PARENT		0x04u
#define SL_CURRENT		0x02u
#define SL_PARENT		0x04u
#define SL_ROOT			0x08u
#define SL_VOLUME_ROOT		0x10u
#define SL_HOST			0x20u
#define TF_LONG			0x80u
#define TF_TIMES		7u

/* POSIX file types in PX mode, as RRIP records them. */
#define RR_TYPE_MASK		0170000u
#define RR_TYPE_FIFO		0010000u
#define RR_TYPE_CHARACTER	0020000u
#define RR_TYPE_DIRECTORY	0040000u
#define RR_TYPE_BLOCK		0060000u
#define RR_TYPE_FILE		0100000u
#define RR_TYPE_LINK		0120000u
#define RR_TYPE_SOCKET		0140000u
#define RR_PERMISSIONS		07777u

/* Content lengths: an entry's length less its four-byte header. */
#define PX_SHORT		32u
#define PX_LONG			40u
#define PN_LENGTH		16u
#define CL_LENGTH		8u
#define RE_LENGTH		0u
#define RR_LENGTH		1u

/* The assembly in progress across one stream's NM and SL entries. */
typedef struct rr_assembly {
	bool		 name_open;	/* the last NM continues */
	bool		 target_open;	/* the last SL continues */
	bool		 joining;	/* the last component continues */
	bool		 after_root;	/* the last component was the root */
	bool		 has_component;	/* a component was appended */
	bool		 component_refused; /* a component was dropped */
	bool		 name_long;	/* the name outgrew ISO_NAME_MAX */
	bool		 target_long;	/* the target outgrew ISO_NAME_MAX */
} rr_assembly_t;

/* Note that the profile refuses rr's record; the first refusal noted is
 * the one reported.
 */
static void
rr_refuse(iso_rr_t *rr, const char *refusal)
{
	if (rr->refusal == NULL)
		rr->refusal = refusal;
}

static bool
entry_is(const iso_entry_t *entry, const char *signature)
{
	return (entry->signature[0] == signature[0] &&
	    entry->signature[1] == signature[1]);
}

/* Note an entry of which a stream holds at most one. */
static fsimg_status_t
single_note(iso_rr_t *rr, uint32_t bit, fsimg_error_t *error)
{
	if ((rr->entries & bit) != 0)
		return (iso_corrupt(error,
		    "Rock Ridge entry appears twice in one record"));
	rr->entries |= bit;
	return (FSIMG_OK);
}

/* PX: mode, link count, uid and gid, and in the 44-byte form a serial
 * number, each both-order. Only the type, permissions and owner are used,
 * but every copy must agree.
 */
static fsimg_status_t
px_decode(const iso_entry_t *entry, iso_rr_t *rr, fsimg_error_t *error)
{
	uint32_t field[5];
	fsimg_status_t status;

	if (entry->length != PX_SHORT && entry->length != PX_LONG)
		return (iso_corrupt(error, "Rock Ridge PX entry has an "
		    "invalid length"));
	for (size_t i = 0; i < entry->length / 8; i++) {
		status = iso_u32both(entry->data + i * 8, &field[i], error);
		if (status != FSIMG_OK)
			return (status);
	}
	rr->type = field[0] & RR_TYPE_MASK;
	rr->metadata.mode = field[0] & RR_PERMISSIONS;
	rr->metadata.uid = field[2];
	rr->metadata.gid = field[3];
	rr->metadata.valid |= FSIMG_META_OWNER | FSIMG_META_MODE;
	return (FSIMG_OK);
}

/* TF: a flag byte selecting up to seven times, each of seven or, with
 * TF_LONG, seventeen bytes. Creation, modification, access and attribute
 * change are kept; backup, expiration and effective are decoded only to be
 * checked.
 */
static fsimg_status_t
tf_decode(const iso_entry_t *entry, iso_rr_t *rr, fsimg_error_t *error)
{
	fsimg_metadata_t *metadata = &rr->metadata;
	fsimg_timestamp_t time;
	const unsigned char *at;
	size_t width, count;
	unsigned flags;
	bool present;
	fsimg_status_t status;

	if (entry->length < 1)
		return (iso_corrupt(error, "Rock Ridge TF entry is too short"));
	flags = entry->data[0];
	width = (flags & TF_LONG) != 0 ? 17 : 7;
	count = 0;
	for (unsigned bit = 0; bit < TF_TIMES; bit++)
		count += (flags >> bit) & 1u;
	if (entry->length != 1 + count * width)
		return (iso_corrupt(error,
		    "Rock Ridge TF length disagrees with its flags"));
	at = entry->data + 1;
	for (unsigned bit = 0; bit < TF_TIMES; bit++) {
		if ((flags & (1u << bit)) == 0)
			continue;
		if (width == 17)
			status = iso_date17_decode(at, &present, &time, error);
		else
			status = iso_date7_decode(at, &present, &time, error);
		if (status != FSIMG_OK)
			return (status);
		at += width;
		if (!present)
			continue;
		switch (bit) {
		case 0:
			metadata->created = time;
			metadata->valid |= FSIMG_META_CREATED;
			break;
		case 1:
			metadata->modified = time;
			metadata->valid |= FSIMG_META_MODIFIED;
			break;
		case 2:
			metadata->accessed = time;
			metadata->valid |= FSIMG_META_ACCESSED;
			break;
		case 3:
			metadata->changed = time;
			metadata->valid |= FSIMG_META_CHANGED;
			break;
		default:	/* backup, expiration, effective */
			break;
		}
	}
	return (FSIMG_OK);
}

/* Append length bytes to rr's name: no NUL or '/'. A name growing past
 * ISO_NAME_MAX bytes is refused, and its bytes are still checked.
 */
static fsimg_status_t
name_append(iso_rr_t *rr, rr_assembly_t *assembly,
    const unsigned char *bytes, size_t length, fsimg_error_t *error)
{
	if (memchr(bytes, '\0', length) != NULL ||
	    memchr(bytes, '/', length) != NULL)
		return (iso_corrupt(error,
		    "Rock Ridge name holds a NUL or a slash"));
	if (assembly->name_long || length > ISO_NAME_MAX - rr->name_length) {
		assembly->name_long = true;
		rr_refuse(rr, "Rock Ridge name is longer than 1024 bytes");
		return (FSIMG_OK);
	}
	bcopy(bytes, rr->name + rr->name_length, length);
	rr->name_length += length;
	return (FSIMG_OK);
}

/* NM: a flag byte and name bytes. On a dot record it is framed and not
 * assembled; elsewhere the current and parent flags are corrupt and the
 * bytes extend the name.
 */
static fsimg_status_t
nm_decode(const iso_entry_t *entry, bool dot, iso_rr_t *rr,
    rr_assembly_t *assembly, fsimg_error_t *error)
{
	unsigned flags;

	if (entry->length < 1)
		return (iso_corrupt(error, "Rock Ridge NM entry is too short"));
	if (dot)
		return (FSIMG_OK);
	flags = entry->data[0];
	if ((flags & (NM_CURRENT | NM_PARENT)) != 0)
		return (iso_corrupt(error,
		    "Rock Ridge NM names a dot record outside one"));
	if ((rr->entries & RR_NM) != 0 && !assembly->name_open)
		return (iso_corrupt(error,
		    "Rock Ridge name starts again after it ended"));
	rr->entries |= RR_NM;
	assembly->name_open = (flags & RR_FLAG_CONTINUE) != 0;
	return (name_append(rr, assembly, entry->data + 1, entry->length - 1,
	    error));
}

/* Append length bytes to rr's target; one growing past ISO_NAME_MAX bytes
 * is refused.
 */
static void
target_append(iso_rr_t *rr, rr_assembly_t *assembly, const void *bytes,
    size_t length)
{
	if (assembly->target_long ||
	    length > ISO_NAME_MAX - rr->target_length) {
		assembly->target_long = true;
		rr_refuse(rr,
		    "Rock Ridge link target is longer than 1024 bytes");
		return;
	}
	bcopy(bytes, rr->target + rr->target_length, length);
	rr->target_length += length;
}

/* Add one SL component of length bytes with flags. Every component is
 * checked first, whatever it means: at most one of the meaning flags and
 * no NUL byte. A volume-root or host component is then refused and adds
 * nothing; any other is appended to the target, joined with '/' except
 * after a root component or when the previous component continues into
 * this one. Every component's continue flag counts, so a refused final
 * component still cannot leave the target continuing.
 */
static fsimg_status_t
component_append(iso_rr_t *rr, rr_assembly_t *assembly, unsigned flags,
    const unsigned char *bytes, size_t length, fsimg_error_t *error)
{
	unsigned special = flags & (SL_CURRENT | SL_PARENT | SL_ROOT |
	    SL_VOLUME_ROOT | SL_HOST);

	if ((special & (special - 1)) != 0)
		return (iso_corrupt(error,
		    "Rock Ridge link component has two meanings"));
	if (memchr(bytes, '\0', length) != NULL)
		return (iso_corrupt(error,
		    "Rock Ridge link component holds a NUL"));
	/* A refused component adds nothing, and the entry is never delivered,
	 * so only its continue flag still matters: a target cannot end on it.
	 */
	if (special == SL_VOLUME_ROOT || special == SL_HOST) {
		assembly->component_refused = true;
		rr_refuse(rr, "Rock Ridge volume-root or host link "
		    "components are unsupported");
		assembly->joining = (flags & RR_FLAG_CONTINUE) != 0;
		return (FSIMG_OK);
	}
	if (assembly->has_component && !assembly->joining &&
	    !assembly->after_root)
		target_append(rr, assembly, "/", 1);
	switch (special) {
	case SL_CURRENT:
		target_append(rr, assembly, ".", 1);
		break;
	case SL_PARENT:
		target_append(rr, assembly, "..", 2);
		break;
	case SL_ROOT:
		target_append(rr, assembly, "/", 1);
		break;
	default:
		target_append(rr, assembly, bytes, length);
		break;
	}
	assembly->has_component = true;
	assembly->joining = (flags & RR_FLAG_CONTINUE) != 0;
	assembly->after_root = special == SL_ROOT;
	return (FSIMG_OK);
}

/* SL: a flag byte, then components of a flag byte, a length and that many
 * bytes, each within the entry. On a dot record it is framed and not
 * assembled.
 */
static fsimg_status_t
sl_decode(const iso_entry_t *entry, bool dot, iso_rr_t *rr,
    rr_assembly_t *assembly, fsimg_error_t *error)
{
	const unsigned char *at, *end;
	unsigned flags;
	size_t length;
	fsimg_status_t status;

	if (entry->length < 1)
		return (iso_corrupt(error, "Rock Ridge SL entry is too short"));
	if (dot)
		return (FSIMG_OK);
	if ((rr->entries & RR_SL) != 0 && !assembly->target_open)
		return (iso_corrupt(error,
		    "Rock Ridge link target starts again after it ended"));
	rr->entries |= RR_SL;
	assembly->target_open = (entry->data[0] & RR_FLAG_CONTINUE) != 0;
	at = entry->data + 1;
	end = entry->data + entry->length;
	while (at < end) {
		if (end - at < 2 || at[1] > (size_t)(end - at) - 2)
			return (iso_corrupt(error,
			    "Rock Ridge link component overruns its entry"));
		flags = at[0];
		length = at[1];
		status = component_append(rr, assembly, flags, at + 2, length,
		    error);
		if (status != FSIMG_OK)
			return (status);
		at += 2 + length;
	}
	return (FSIMG_OK);
}

/* A block pointer of CL or PL. */
static fsimg_status_t
pointer_decode(const iso_entry_t *entry, uint32_t bit, uint32_t *block,
    iso_rr_t *rr, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (entry->length != CL_LENGTH)
		return (iso_corrupt(error,
		    "Rock Ridge CL or PL entry has an invalid length"));
	status = single_note(rr, bit, error);
	if (status == FSIMG_OK)
		status = iso_u32both(entry->data, block, error);
	return (status);
}

/* PN: a device number in two both-order words, high then low, reported as
 * stored: the high word is dev_major and the low word dev_minor. A system
 * with a 32-bit dev_t stores zero and its packed number; that reads as
 * major 0 and the packed value, since the packing is not in the image.
 * rr_kind() admits PN on exactly the devices, so the field set here never
 * reaches another kind.
 */
static fsimg_status_t
pn_decode(const iso_entry_t *entry, iso_rr_t *rr, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (entry->length != PN_LENGTH)
		return (iso_corrupt(error,
		    "Rock Ridge PN entry has an invalid length"));
	status = single_note(rr, RR_PN, error);
	if (status == FSIMG_OK)
		status = iso_u32both(entry->data, &rr->metadata.dev_major,
		    error);
	if (status == FSIMG_OK)
		status = iso_u32both(entry->data + 8, &rr->metadata.dev_minor,
		    error);
	if (status == FSIMG_OK)
		rr->metadata.valid |= FSIMG_META_DEVICE;
	return (status);
}

/* Decode one entry under the entry table: an entry the table does not
 * list is skipped, a listed entry has version 1, and SF and ZF of any
 * length are refused.
 */
static fsimg_status_t
entry_decode(const iso_entry_t *entry, bool dot, iso_rr_t *rr,
    rr_assembly_t *assembly, fsimg_error_t *error)
{
	static const char listed[][3] = {
		"PX", "PN", "SL", "NM", "CL", "PL", "RE", "TF", "RR", "SF", "ZF"
	};
	bool known = false;
	fsimg_status_t status;

	for (size_t i = 0; i < sizeof(listed) / sizeof(listed[0]); i++)
		known = known || entry_is(entry, listed[i]);
	if (!known)
		return (FSIMG_OK);
	if (entry->version != 1)
		return (iso_corrupt(error,
		    "Rock Ridge entry has an unknown version"));
	if (entry_is(entry, "SF")) {
		rr_refuse(rr, "Rock Ridge sparse files are unsupported");
		return (FSIMG_OK);
	}
	if (entry_is(entry, "ZF")) {
		rr_refuse(rr, "zisofs compressed files are unsupported");
		return (FSIMG_OK);
	}
	if (entry_is(entry, "PX")) {
		status = single_note(rr, RR_PX, error);
		if (status != FSIMG_OK)
			return (status);
		return (px_decode(entry, rr, error));
	}
	if (entry_is(entry, "TF")) {
		status = single_note(rr, RR_TF, error);
		if (status != FSIMG_OK)
			return (status);
		return (tf_decode(entry, rr, error));
	}
	if (entry_is(entry, "NM"))
		return (nm_decode(entry, dot, rr, assembly, error));
	if (entry_is(entry, "SL"))
		return (sl_decode(entry, dot, rr, assembly, error));
	if (entry_is(entry, "CL"))
		return (pointer_decode(entry, RR_CL, &rr->child, rr, error));
	if (entry_is(entry, "PL"))
		return (pointer_decode(entry, RR_PL, &rr->parent, rr, error));
	if (entry_is(entry, "PN"))
		return (pn_decode(entry, rr, error));
	if (entry_is(entry, "RE")) {
		if (entry->length != RE_LENGTH)
			return (iso_corrupt(error,
			    "Rock Ridge RE entry has an invalid length"));
		return (single_note(rr, RR_RE, error));
	}
	/* RR, the historical presence flags, is ignored. */
	if (entry->length != RR_LENGTH)
		return (iso_corrupt(error,
		    "Rock Ridge RR entry has an invalid length"));
	return (FSIMG_OK);
}

/* Set rr->kind for the record whose stream decoded into rr
 * (specs/design-iso9660.md#kinds-and-metadata). The corruption checks run
 * on the PX type or, without PX, the type the directory flag implies: a
 * placeholder carrying CL is a directory and carries no RE; otherwise the
 * type is a directory exactly when the flag is set, RE marks only a
 * directory, SL belongs to a symbolic link, and PN to exactly the
 * character and block devices, which RRIP requires to carry it. A type
 * outside the seven POSIX types is noted as a refusal. A record with any
 * refusal is then left without a kind (0, no fsimg_entry_kind_t), so an
 * entry whose refusal went unreported would fail admission, its payload
 * decoding as invalid, rather than be admitted.
 */
static fsimg_status_t
rr_kind(iso_rr_t *rr, const iso_record_t *record, fsimg_error_t *error)
{
	bool flagged = (record->flags & ISO_FLAG_DIRECTORY) != 0;
	uint32_t type;

	rr->kind = FSIMG_ENTRY_FILE;
	if ((rr->entries & RR_PX) != 0)
		type = rr->type;
	else
		type = flagged ? RR_TYPE_DIRECTORY : RR_TYPE_FILE;
	if ((rr->entries & RR_CL) != 0) {
		if ((rr->entries & RR_RE) != 0 ||
		    ((rr->entries & RR_PX) != 0 && type != RR_TYPE_DIRECTORY))
			return (iso_corrupt(error, "Rock Ridge relocation "
			    "placeholder is not a directory"));
		type = RR_TYPE_DIRECTORY;
	} else if (flagged != (type == RR_TYPE_DIRECTORY))
		return (iso_corrupt(error,
		    "Rock Ridge file type disagrees with the directory flag"));
	if ((rr->entries & RR_RE) != 0 && !flagged)
		return (iso_corrupt(error,
		    "Rock Ridge RE entry on a record that is not a directory"));
	if (((rr->entries & RR_SL) != 0) != (type == RR_TYPE_LINK))
		return (iso_corrupt(error,
		    "Rock Ridge link target and file type disagree"));
	if (((rr->entries & RR_PN) != 0) !=
	    (type == RR_TYPE_CHARACTER || type == RR_TYPE_BLOCK))
		return (iso_corrupt(error,
		    "Rock Ridge device number and file type disagree"));

	switch (type) {
	case RR_TYPE_FILE:
		break;
	case RR_TYPE_DIRECTORY:
		rr->kind = FSIMG_ENTRY_DIRECTORY;
		break;
	case RR_TYPE_LINK:
		rr->kind = FSIMG_ENTRY_SYMLINK;
		break;
	case RR_TYPE_FIFO:
		rr->kind = FSIMG_ENTRY_FIFO;
		break;
	case RR_TYPE_CHARACTER:
		rr->kind = FSIMG_ENTRY_CHAR_DEVICE;
		break;
	case RR_TYPE_BLOCK:
		rr->kind = FSIMG_ENTRY_BLOCK_DEVICE;
		break;
	case RR_TYPE_SOCKET:
		rr->kind = FSIMG_ENTRY_SOCKET;
		break;
	default:
		rr_refuse(rr, "Rock Ridge file type is unsupported");
		break;
	}
	if (rr->refusal != NULL)
		rr->kind = (fsimg_entry_kind_t)0;
	return (FSIMG_OK);
}

/* Report what decoding rr noted as refused, FSIMG_EUNSUPPORTED, or
 * FSIMG_OK when nothing was. A caller calls it once the record has passed
 * every check of its own.
 */
fsimg_status_t
iso_rr_refusal(const iso_rr_t *rr, fsimg_error_t *error)
{
	if (rr->refusal == NULL)
		return (FSIMG_OK);
	return (iso_unsupported(error, rr->refusal));
}

/* Decode the SUSP stream of record, in role, into rr and derive rr->kind.
 * raw holds the decoded record, which lies at offset of volume block
 * number block. This is the only interpreting decoder of a stream, so no
 * caller can skip the kind checks. Besides a SUSP profile limit it
 * returns only corruption, I/O and resource failures; a refusal stays in
 * rr->refusal for iso_rr_refusal(). An entry cannot skip that call
 * unnoticed, having no kind; a dot record yields no entry, so dots_check()
 * is the one that reports its refusal. A dot record's stream takes every
 * check but the interpretation of its NM and SL entries, which are
 * framed and not assembled, and carries no CL or RE, nor PL on the first
 * dot record. An entry's name is not empty, "." or "..", its
 * target is not empty unless a refused component was dropped from it, and
 * neither ends on a continue flag; the name is NUL-terminated.
 */
fsimg_status_t
iso_rr_decode(fsimg_reader_t *reader, const unsigned char *raw,
    uint32_t block, uint32_t offset, const iso_record_t *record,
    iso_rr_role_t role, iso_rr_t *rr, fsimg_error_t *error)
{
	bool dot = role != ISO_RR_ENTRY;
	rr_assembly_t assembly;
	iso_susp_t susp;
	iso_entry_t entry;
	fsimg_status_t status;

	bzero(rr, sizeof(*rr));
	bzero(&assembly, sizeof(assembly));
	iso_susp_start(&susp, raw, block, offset, record);
	while ((status = iso_susp_next(reader, &susp, &entry, error)) ==
	    FSIMG_OK) {
		status = entry_decode(&entry, dot, rr, &assembly, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (status != FSIMG_DONE)
		return (status);
	if (assembly.name_open || assembly.target_open || assembly.joining)
		return (iso_corrupt(error, "Rock Ridge name or link target "
		    "continues past its record"));
	if ((rr->entries & RR_NM) != 0 && !assembly.name_long &&
	    (rr->name_length == 0 || strcmp(rr->name, ".") == 0 ||
	    strcmp(rr->name, "..") == 0))
		return (iso_corrupt(error, "Rock Ridge name names no entry"));
	if ((rr->entries & RR_SL) != 0 && rr->target_length == 0 &&
	    !assembly.component_refused)
		return (iso_corrupt(error, "Rock Ridge link target is empty"));
	/* Relocation lives in the records that name directories and in a
	 * relocated directory's parent record, nowhere else.
	 */
	if (dot && ((rr->entries & (RR_CL | RR_RE)) != 0 ||
	    (role == ISO_RR_SELF && (rr->entries & RR_PL) != 0)))
		return (iso_corrupt(error,
		    "Rock Ridge relocation entry on a dot record"));
	return (rr_kind(rr, record, error));
}
