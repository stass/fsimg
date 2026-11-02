/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "write-iso9660.h"

/*
 * Rock Ridge entries for the writer. Planning and emission both call these
 * functions with the same row, name and target, so the entries emission
 * writes are byte for byte the ones planning sized and placed.
 *
 * A record's entries, in order: SP on the root's first dot record, PX
 * and TF on every record, ER on the root's first dot record, then NM on
 * an entry's record and SL on a symbolic link's. ER comes after PX and
 * TF so the root's record keeps them and only ER continues: libarchive
 * finds Rock Ridge only when the root's own record holds an entry it
 * decodes, as mkisofs and xorriso write it. The writer writes RRIP 1.10: the ER identifier
 * RRIP_1991A and the 36-byte PX without a serial number.
 *
 * Entries that do not fit their record are split off whole into a chain
 * of continuation fragments, each within one 2048-byte block and placed
 * by next fit in the continuation region.
 */

#define SUSP_HEADER		4u	/* signature, length, version */
#define SP_BYTES		7u
#define PX_BYTES		36u
#define TF_HEADER		5u
#define NM_HEADER		5u
#define NM_DATA_MAX		250u
#define SL_HEADER		5u
#define SL_ENTRY_MAX		255u
#define SL_PIECE_MAX		(SL_ENTRY_MAX - SL_HEADER - 2u)	/* 248 */
#define ER_HEADER		8u

#define RR_CONTINUE		0x01u
#define SL_CURRENT		0x02u
#define SL_PARENT		0x04u
#define SL_ROOT			0x08u

/* TF flag bits: modification, access and attribute change times. */
#define TF_MODIFY		0x02u
#define TF_ACCESS		0x04u
#define TF_ATTRIBUTES		0x08u

/* RRIP 1.10's extension identifier, description and source texts. */
static const char er_identifier[] = "RRIP_1991A";
static const char er_description[] =
    "THE ROCK RIDGE INTERCHANGE PROTOCOL PROVIDES SUPPORT FOR POSIX FILE "
    "SYSTEM SEMANTICS";
static const char er_source[] =
    "PLEASE CONTACT DISC PUBLISHER FOR SPECIFICATION SOURCE.  SEE "
    "PUBLISHER IDENTIFIER IN PRIMARY VOLUME DESCRIPTOR FOR CONTACT "
    "INFORMATION.";

static size_t entry_header(unsigned char *, const char *, size_t);
static size_t sp_encode(unsigned char *);
static size_t er_encode(unsigned char *);
static size_t px_encode(const iso_row_t *, unsigned char *);
static size_t tf_encode(const iso_row_t *, unsigned char *);
static size_t nm_encode(const char *, size_t, unsigned char *);
static size_t sl_component(unsigned char *, unsigned,
    const unsigned char *, size_t);
static void sl_pack(unsigned char *, size_t *, size_t *,
    const unsigned char *, size_t);
static size_t ce_encode(uint32_t, uint64_t, size_t, unsigned char *);

/* Write a SUSP entry header of length bytes, version 1. */
static size_t
entry_header(unsigned char *out, const char *signature, size_t length)
{
	out[0] = (unsigned char)signature[0];
	out[1] = (unsigned char)signature[1];
	out[2] = (unsigned char)length;
	out[3] = 1;
	return (SUSP_HEADER);
}

/* SP: the check bytes BE EF and a skip count of zero. */
static size_t
sp_encode(unsigned char *out)
{
	entry_header(out, "SP", SP_BYTES);
	out[4] = 0xbe;
	out[5] = 0xef;
	out[6] = 0;
	return (SP_BYTES);
}

/* ER: the lengths of the three texts, extension version 1, the texts. */
static size_t
er_encode(unsigned char *out)
{
	size_t id = strlen(er_identifier), description = strlen(er_description),
	    source = strlen(er_source), length;

	length = ER_HEADER + id + description + source;
	entry_header(out, "ER", length);
	out[4] = (unsigned char)id;
	out[5] = (unsigned char)description;
	out[6] = (unsigned char)source;
	out[7] = 1;
	bcopy(er_identifier, out + ER_HEADER, id);
	bcopy(er_description, out + ER_HEADER + id, description);
	bcopy(er_source, out + ER_HEADER + id + description, source);
	return (length);
}

/* PX in its 36-byte form: mode, link count, uid and gid. */
static size_t
px_encode(const iso_row_t *row, unsigned char *out)
{
	entry_header(out, "PX", PX_BYTES);
	iso_u32both_put(out + 4, row->mode);
	iso_u32both_put(out + 12, row->links);
	iso_u32both_put(out + 20, row->uid);
	iso_u32both_put(out + 28, row->gid);
	return (PX_BYTES);
}

/* TF in the seven-byte form: modification, then access and attribute
 * change when the row has them.
 */
static size_t
tf_encode(const iso_row_t *row, unsigned char *out)
{
	size_t length = TF_HEADER;
	unsigned flags = TF_MODIFY;

	bcopy(row->modified, out + length, ISO_DATE7_BYTES);
	length += ISO_DATE7_BYTES;
	if ((row->times & ISO_TIME_ACCESSED) != 0) {
		flags |= TF_ACCESS;
		bcopy(row->accessed, out + length, ISO_DATE7_BYTES);
		length += ISO_DATE7_BYTES;
	}
	if ((row->times & ISO_TIME_CHANGED) != 0) {
		flags |= TF_ATTRIBUTES;
		bcopy(row->changed, out + length, ISO_DATE7_BYTES);
		length += ISO_DATE7_BYTES;
	}
	entry_header(out, "TF", length);
	out[4] = (unsigned char)flags;
	return (length);
}

/* NM entries of at most 250 name bytes each, every one but the last
 * flagged to continue.
 */
static size_t
nm_encode(const char *name, size_t length, unsigned char *out)
{
	size_t at = 0, chunk;

	do {
		chunk = length > NM_DATA_MAX ? NM_DATA_MAX : length;
		entry_header(out + at, "NM", NM_HEADER + chunk);
		out[at + 4] = chunk < length ? RR_CONTINUE : 0;
		bcopy(name, out + at + NM_HEADER, chunk);
		at += NM_HEADER + chunk;
		name += chunk;
		length -= chunk;
	} while (length != 0);
	return (at);
}

/* One SL component record: flags, length, bytes. */
static size_t
sl_component(unsigned char *out, unsigned flags, const unsigned char *bytes,
    size_t length)
{
	out[0] = (unsigned char)flags;
	out[1] = (unsigned char)length;
	if (length != 0)
		bcopy(bytes, out + 2, length);
	return (2 + length);
}

/* Append one component record to the SL entry being filled, which
 * starts at *entry and holds *entry_bytes bytes so far; when the record
 * does not fit, close that entry flagged to continue and open the next
 * right after it.
 */
static void
sl_pack(unsigned char *out, size_t *entry, size_t *entry_bytes,
    const unsigned char *record, size_t record_bytes)
{
	if (*entry_bytes + record_bytes > SL_ENTRY_MAX) {
		entry_header(out + *entry, "SL", *entry_bytes);
		out[*entry + 4] = RR_CONTINUE;
		*entry += *entry_bytes;
		*entry_bytes = SL_HEADER;
	}
	bcopy(record, out + *entry + *entry_bytes, record_bytes);
	*entry_bytes += record_bytes;
}

/*
 * Encode a symbolic link's target of 1 to 1023 bytes as SL entries into
 * out, returning their length. The reader joins components with '/'
 * except after a root component, so the target is written as one root
 * component per leading slash, then the rest split at
 * each '/' into components, empty ones included: "/a" is root and "a",
 * "a//b" is "a", "" and "b", "a/" is "a" and "". "." and ".." become
 * their flagged components. All-root targets retain their terminal empty
 * text component, as other targets ending in '/' do. A component longer
 * than 248 bytes is written
 * as pieces of at most 248 bytes, each but the last flagged to continue
 * into the next without a separator. Component records are packed whole
 * into entries of at most 255 bytes, every entry but the last flagged to
 * continue.
 *
 * Every target byte becomes at most two component bytes, plus at most two
 * for the terminal empty component of an all-root target. An entry is
 * closed only when the next record, at most 250 bytes, does not fit, so
 * two consecutive entries carry more than 250 component bytes: a
 * 1023-byte target needs at most 2048 component bytes in 17 entries,
 * 2133 bytes with their headers.
 */
size_t
iso_sl_encode(const unsigned char *target, size_t length, unsigned char *out)
{
	unsigned char record[2 + SL_PIECE_MAX];
	size_t entry, entry_bytes, roots, start, end, piece, record_bytes;
	unsigned flags;

	entry = 0;
	entry_bytes = SL_HEADER;
	for (roots = 0; roots < length && target[roots] == '/'; roots++) {
		record_bytes = sl_component(record, SL_ROOT, NULL, 0);
		sl_pack(out, &entry, &entry_bytes, record, record_bytes);
	}
	start = roots;
	for (;;) {
		end = start;
		while (end < length && target[end] != '/')
			end++;
		for (piece = end - start; piece > SL_PIECE_MAX;
		    piece -= SL_PIECE_MAX) {
			record_bytes = sl_component(record, RR_CONTINUE,
			    target + start, SL_PIECE_MAX);
			sl_pack(out, &entry, &entry_bytes, record, record_bytes);
			start += SL_PIECE_MAX;
		}
		flags = 0;
		if (piece == 1 && target[start] == '.')
			flags = SL_CURRENT;
		else if (piece == 2 && target[start] == '.' &&
		    target[start + 1] == '.')
			flags = SL_PARENT;
		record_bytes = sl_component(record, flags, target + start,
		    flags == 0 ? piece : 0);
		sl_pack(out, &entry, &entry_bytes, record, record_bytes);
		if (end == length)
			break;
		start = end + 1;
	}
	entry_header(out + entry, "SL", entry_bytes);
	out[entry + 4] = 0;
	return (entry + entry_bytes);
}

/* Encode one record's Rock Ridge entries into out, which holds
 * ISO_RR_ENTRIES_MAX bytes, returning their length. A child record's
 * name is its exact source component; a link's SL entries are encoded
 * from its target, which the caller read.
 */
size_t
iso_rr_encode(const iso_rr_input_t *input, unsigned char *out)
{
	size_t at = 0;

	if (input->record == ISO_RR_ROOT_SELF)
		at += sp_encode(out + at);
	at += px_encode(input->row, out + at);
	at += tf_encode(input->row, out + at);
	switch (input->record) {
	case ISO_RR_ROOT_SELF:
		return (at + er_encode(out + at));
	case ISO_RR_DOT:
		return (at);
	case ISO_RR_PARENT_LINK:
		entry_header(out + at, "PL", 12);
		iso_u32both_put(out + at + 4, input->link_block);
		return (at + 12);
	case ISO_RR_CHILD:
	case ISO_RR_PLACEHOLDER:
	case ISO_RR_RELOCATED:
		break;
	}
	at += nm_encode(input->name, input->name_length, out + at);
	if (input->record == ISO_RR_PLACEHOLDER) {
		entry_header(out + at, "CL", 12);
		iso_u32both_put(out + at + 4, input->row->block[ISO_PRIMARY]);
		at += 12;
	} else if (input->record == ISO_RR_RELOCATED) {
		at += entry_header(out + at, "RE", 4);
	}
	if (input->target_length != 0)
		at += iso_sl_encode(input->target, input->target_length,
		    out + at);
	return (at);
}

/* CE: the block, offset and length of the next continuation area. */
static size_t
ce_encode(uint32_t first_block, uint64_t position, size_t length,
    unsigned char *out)
{
	entry_header(out, "CE", ISO_CE_BYTES);
	iso_u32both_put(out + 4,
	    (uint32_t)(first_block + position / ISO_BLOCK_BYTES));
	iso_u32both_put(out + 12, (uint32_t)(position % ISO_BLOCK_BYTES));
	iso_u32both_put(out + 20, (uint32_t)length);
	return (ISO_CE_BYTES);
}

/* The length of the SUSP entry at entries, from its length byte. */
static size_t
entry_length(const unsigned char *entry)
{
	return (entry[2]);
}

/*
 * Split length bytes of encoded entries for a record whose system use
 * area holds at most room bytes. When they all fit, the record keeps
 * them. Otherwise the record keeps the longest prefix of whole entries
 * that leaves room for a 28-byte CE, and the rest form fragments: each
 * takes every remaining entry when they fit 2048 bytes, else the longest
 * run that fits with 28 bytes kept for the CE to the next fragment. An
 * entry is at most 255 bytes, so each fragment takes at least one.
 * Entry lengths alone decide the split, never an address.
 */
fsimg_status_t
iso_susp_split(const unsigned char *entries, size_t length, size_t room,
    iso_split_t *split, fsimg_error_t *error)
{
	size_t at, start, bytes, next;

	bzero(split, sizeof(*split));
	if (length <= room) {
		split->kept = length;
		split->record_bytes = length;
		return (FSIMG_OK);
	}
	at = 0;
	while (at + entry_length(entries + at) + ISO_CE_BYTES <= room)
		at += entry_length(entries + at);
	split->kept = at;
	split->record_bytes = at + ISO_CE_BYTES;

	while (at < length) {
		if (split->fragments == ISO_FRAGMENTS_MAX)
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_WRITER,
			    "ISO 9660 Rock Ridge entries exceed their fragments"));
		start = at;
		if (length - start <= ISO_BLOCK_BYTES) {
			bytes = length - start;
			at = length;
		} else {
			for (;;) {
				next = at + entry_length(entries + at);
				if (next - start + ISO_CE_BYTES >
				    ISO_BLOCK_BYTES)
					break;
				at = next;
			}
			bytes = at - start + ISO_CE_BYTES;
		}
		split->fragment_bytes[split->fragments++] = (uint16_t)bytes;
	}
	return (FSIMG_OK);
}

/* Place length bytes by next fit at *cursor, a byte offset in a region
 * of 2048-byte blocks, the continuation region or a directory: at the
 * cursor when they fit in its block, else at the start of the next
 * block, never crossing one. Returns the position
 * and advances the cursor past it.
 */
uint64_t
iso_place(uint64_t *cursor, size_t length)
{
	uint64_t position;

	if (*cursor % ISO_BLOCK_BYTES + length > ISO_BLOCK_BYTES)
		*cursor += ISO_BLOCK_BYTES - *cursor % ISO_BLOCK_BYTES;
	position = *cursor;
	*cursor += length;
	return (position);
}

/*
 * Assemble a split record: the record's system use bytes, split's
 * record_bytes, into record, and each fragment's bytes into fragments,
 * one after another, with its byte position in the continuation region,
 * whose first block is first_block, in positions. The first fragment is
 * at position and each later one where next fit puts it after the
 * previous, as planning placed them. Every CE names the next fragment.
 */
void
iso_susp_assemble(const unsigned char *entries, const iso_split_t *split,
    uint32_t first_block, uint64_t position, unsigned char *record,
    unsigned char *fragments, uint64_t *positions)
{
	uint64_t cursor = position;
	size_t at, out, data;

	for (unsigned i = 0; i < split->fragments; i++)
		positions[i] = iso_place(&cursor, split->fragment_bytes[i]);
	bcopy(entries, record, split->kept);
	if (split->fragments == 0)
		return;
	ce_encode(first_block, positions[0], split->fragment_bytes[0],
	    record + split->kept);
	at = split->kept;
	out = 0;
	for (unsigned i = 0; i < split->fragments; i++) {
		data = split->fragment_bytes[i];
		if (i + 1 < split->fragments)
			data -= ISO_CE_BYTES;
		bcopy(entries + at, fragments + out, data);
		at += data;
		out += data;
		if (i + 1 < split->fragments) {
			ce_encode(first_block, positions[i + 1],
			    split->fragment_bytes[i + 1], fragments + out);
			out += ISO_CE_BYTES;
		}
	}
}

/* The 64-bit FNV-1a hash of length bytes. */
uint64_t
iso_hash(const unsigned char *bytes, size_t length)
{
	uint64_t hash = UINT64_C(0xcbf29ce484222325);

	for (size_t i = 0; i < length; i++) {
		hash ^= bytes[i];
		hash *= UINT64_C(0x100000001b3);
	}
	return (hash);
}
