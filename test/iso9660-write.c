/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "fsimg.h"
#include "format.h"
#include "internal.h"
#include "meta.h"
#include "test.h"
#include "write-iso9660.h"

/*
 * ISO 9660 writer tests. The writer is reached through its unregistered
 * descriptor. Images are checked twice: by fsimg's reader and verifier in
 * all three views, and by the independent checker below, written from the
 * format design with its own offsets and its own SUSP walk. The checker
 * decodes the descriptor set, both hierarchies' directories and path
 * tables, every Rock Ridge entry with its continuation chain, and file
 * data; it requires the writer profile's layout and identifiers and marks
 * every byte a structure gives a meaning. Every other byte must be zero
 * over a sink poisoned with 0xa5, so a stray or stale byte fails a test.
 */
#define BLOCK		2048u
#define POISON		0xa5u
#define PATH_BYTES	2048u
#define TARGET_BYTES	1100u
#define NAME_BYTES	300u
#define CHK_ENTRIES	4096u
#define TARGET_SOURCE	1000u	/* source IDs of link targets */

#define S_TYPE		0170000u
#define S_FILE		0100000u
#define S_DIR		0040000u
#define S_LINK		0120000u
#define S_FIFO		0010000u

static uint16_t
le16(const unsigned char *p)
{
	return ((uint16_t)(p[0] | p[1] << 8));
}

static uint32_t
le32(const unsigned char *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

static uint16_t
be16(const unsigned char *p)
{
	return ((uint16_t)(p[0] << 8 | p[1]));
}

static uint32_t
be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | (uint32_t)p[3]);
}

static uint64_t
hash_update(uint64_t hash, const unsigned char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		hash ^= p[i];
		hash *= UINT64_C(0x100000001b3);
	}
	return (hash);
}
#define HASH_START	UINT64_C(0xcbf29ce484222325)

/*
 * The independent checker.
 */

/* One entry the checker found in a hierarchy. path is the Rock Ridge
 * path in the primary walk and the Joliet path in the Joliet walk;
 * display is the path a reader of the primary view shows, identifiers
 * without ";1". identifier is the entry's own identifier as bytes.
 */
typedef struct chk_entry {
	char		 path[PATH_BYTES];
	char		 display[PATH_BYTES];
	unsigned char	 identifier[256];
	char		 target[TARGET_BYTES];
	uint64_t	 size;
	uint64_t	 hash;
	uint32_t	 extent;
	uint32_t	 child_link;
	uint32_t	 containing_directory;
	uint32_t	 mode;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 links;
	int64_t		 modified;
	size_t		 identifier_length;
	unsigned	 sections;
	bool		 directory;
	bool		 has_px;
	bool		 relocated;
} chk_entry_t;

/* One directory found by a walk, for the path-table comparison. */
typedef struct chk_dir {
	uint32_t	 extent;
	uint32_t	 parent;
	uint32_t	 logical_parent;
	unsigned char	 identifier[256];
	size_t		 length;
} chk_dir_t;

/* What the checker requires beyond the format. */
typedef struct chk_expect {
	const char	*label;		/* NULL for CDROM */
	unsigned	 level;		/* 0 for 3 */
	bool		 quiet;
} chk_expect_t;

/* The checker's state: explained has one byte per image byte, set where
 * a structure gives it a meaning; owner one per block: 0 free, 1
 * metadata, 2 data, 3 continuation, which only continuation shares and
 * data with data.
 */
typedef struct chk {
	const unsigned char *img;
	unsigned char	*explained;
	unsigned char	*owner;
	chk_entry_t	*entries[2];	/* primary, Joliet */
	chk_dir_t	*dirs[2];
	size_t		 count[2];
	size_t		 dir_count[2];
	uint64_t	 length;
	uint32_t	 blocks;
	uint32_t	 path_bytes[2];
	uint32_t	 path_l[2];
	uint32_t	 path_m[2];
	uint32_t	 root_extent[2];
	uint32_t	 root_length[2];
	unsigned	 level;
	unsigned	 errors;
	unsigned	 depth;
	bool		 quiet;
} chk_t;

#define CHK(c, cond, what) do { \
	if (!(cond)) \
		chk_fail((c), (what), __LINE__); \
} while (0)

static void
chk_fail(chk_t *c, const char *what, int line)
{
	c->errors++;
	if (!c->quiet && c->errors <= 20)
		fprintf(stderr, "iso9660 checker (line %d): %s\n", line, what);
}

static bool
all_zero(const unsigned char *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != 0)
			return (false);
	return (true);
}

/* A both-byte-order field whose copies agree. */
static uint32_t
both32(chk_t *c, const unsigned char *p)
{
	CHK(c, le32(p) == be32(p + 4), "u32 copies differ");
	return (le32(p));
}

static uint16_t
both16(chk_t *c, const unsigned char *p)
{
	CHK(c, le16(p) == be16(p + 2), "u16 copies differ");
	return (le16(p));
}

static void
chk_explain(chk_t *c, uint64_t offset, uint64_t n)
{
	if (offset > c->length || n > c->length - offset) {
		CHK(c, false, "structure outside the image");
		return;
	}
	memset(c->explained + offset, 1, (size_t)n);
}

/* Claim blocks [first, first + count) for kind. */
static void
chk_claim(chk_t *c, uint64_t first, uint64_t count, unsigned char kind)
{
	if (first + count > c->blocks) {
		CHK(c, false, "claim past the volume");
		return;
	}
	for (uint64_t b = first; b < first + count; b++) {
		CHK(c, c->owner[b] == 0 || (c->owner[b] == kind &&
		    kind != 1), "blocks claimed twice");
		c->owner[b] = kind;
	}
}

/* A seven-byte recording date at offset 0 as UTC seconds, by an
 * independent day count.
 */
static int64_t
date7(chk_t *c, const unsigned char *p)
{
	static const int month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30,
	    31, 30, 31 };
	int64_t days = 0;
	int year = 1900 + p[0];

	CHK(c, p[1] >= 1 && p[1] <= 12 && p[2] >= 1 && p[3] < 24 &&
	    p[4] < 60 && p[5] < 60 && p[6] == 0, "invalid recording date");
	if (p[1] < 1 || p[1] > 12)
		return (0);
	for (int y = 1970; y < year; y++)
		days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 :
		    365;
	for (int y = year; y < 1970; y++)
		days -= (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 :
		    365;
	for (int m = 1; m < p[1]; m++)
		days += month_days[m - 1] + (m == 2 && year % 4 == 0 &&
		    (year % 100 != 0 || year % 400 == 0) ? 1 : 0);
	days += p[2] - 1;
	return (days * 86400 + p[3] * 3600 + p[4] * 60 + p[5]);
}

/* The primary order: name before '.', extension before ';', each padded
 * with spaces; then the identifier bytes.
 */
static int
primary_order(const unsigned char *a, size_t an, bool ad,
    const unsigned char *b, size_t bn, bool bd)
{
	size_t a_name = an, b_name = bn, a_end = an, b_end = bn, i, n;
	int ca, cb;

	if (!ad) {
		a_end = an - 2;
		a_name = (size_t)((const unsigned char *)memchr(a, '.', an) - a);
	}
	if (!bd) {
		b_end = bn - 2;
		b_name = (size_t)((const unsigned char *)memchr(b, '.', bn) - b);
	}
	n = a_name > b_name ? a_name : b_name;
	for (i = 0; i < n; i++) {
		ca = i < a_name ? a[i] : ' ';
		cb = i < b_name ? b[i] : ' ';
		if (ca != cb)
			return (ca - cb);
	}
	{
		size_t ax = ad ? 0 : a_end - a_name - 1;
		size_t bx = bd ? 0 : b_end - b_name - 1;

		n = ax > bx ? ax : bx;
		for (i = 0; i < n; i++) {
			ca = i < ax ? a[a_name + 1 + i] : ' ';
			cb = i < bx ? b[b_name + 1 + i] : ' ';
			if (ca != cb)
				return (ca - cb);
		}
	}
	n = an < bn ? an : bn;
	if (memcmp(a, b, n) != 0)
		return (memcmp(a, b, n));
	return ((int)an - (int)bn);
}

/* The writer's Joliet order: over 16-bit units, the name before the
 * first '.', then the extension before ";1", each padded with U+0020; a
 * directory's whole identifier is its name; then the identifier bytes.
 */
static int
joliet_order(const unsigned char *a, size_t an, bool ad,
    const unsigned char *b, size_t bn, bool bd)
{
	const unsigned char *id[2] = { a, b };
	size_t units[2] = { ad ? an / 2 : an / 2 - 2, bd ? bn / 2 : bn / 2 - 2 };
	size_t name[2], ext_start[2], ext[2], n;
	bool directory[2] = { ad, bd };

	for (unsigned s = 0; s < 2; s++) {
		name[s] = ext_start[s] = units[s];
		ext[s] = 0;
		for (size_t i = 0; !directory[s] && i < units[s]; i++) {
			if (be16(id[s] + 2 * i) == '.') {
				name[s] = i;
				ext_start[s] = i + 1;
				ext[s] = units[s] - i - 1;
				break;
			}
		}
	}
	n = name[0] > name[1] ? name[0] : name[1];
	for (size_t i = 0; i < n; i++) {
		int ca = i < name[0] ? be16(a + 2 * i) : ' ';
		int cb = i < name[1] ? be16(b + 2 * i) : ' ';

		if (ca != cb)
			return (ca - cb);
	}
	n = ext[0] > ext[1] ? ext[0] : ext[1];
	for (size_t i = 0; i < n; i++) {
		int ca = i < ext[0] ? be16(a + 2 * (ext_start[0] + i)) : ' ';
		int cb = i < ext[1] ? be16(b + 2 * (ext_start[1] + i)) : ' ';

		if (ca != cb)
			return (ca - cb);
	}
	n = an < bn ? an : bn;
	if (memcmp(a, b, n) != 0)
		return (memcmp(a, b, n));
	return ((int)an - (int)bn);
}

/* A primary identifier under the writer profile at the level. */
static void
check_primary_identifier(chk_t *c, const unsigned char *id, size_t n,
    bool directory)
{
	size_t dot = n, name, extension;

	for (size_t i = 0; i < n; i++) {
		bool d = (id[i] >= 'A' && id[i] <= 'Z') ||
		    (id[i] >= '0' && id[i] <= '9') || id[i] == '_';

		if (d)
			continue;
		if (!directory && id[i] == '.' && dot == n) {
			dot = i;
			continue;
		}
		if (!directory && i + 2 == n && id[i] == ';' && id[i + 1] == '1')
			break;
		CHK(c, false, "primary identifier holds a non-d-character");
		return;
	}
	if (directory) {
		CHK(c, n >= 1 && n <= (c->level == 1 ? 8u : 31u),
		    "primary directory identifier length");
		return;
	}
	CHK(c, dot != n && n >= 4 && id[n - 2] == ';' && id[n - 1] == '1',
	    "primary file identifier lacks '.' or ';1'");
	if (dot == n || n < 4)
		return;
	name = dot;
	extension = n - 2 - dot - 1;
	if (c->level == 1)
		CHK(c, name <= 8 && extension <= 3, "level 1 file identifier");
	else
		CHK(c, name + extension <= 30, "file identifier over 30");
}

/* Decode a Joliet identifier to UTF-8 (BMP only, as the writer writes). */
static size_t
joliet_utf8(const unsigned char *id, size_t n, char *out)
{
	size_t at = 0;

	for (size_t i = 0; i + 1 < n; i += 2) {
		unsigned u = be16(id + i);

		if (u < 0x80) {
			out[at++] = (char)u;
		} else if (u < 0x800) {
			out[at++] = (char)(0xc0 | u >> 6);
			out[at++] = (char)(0x80 | (u & 0x3f));
		} else {
			out[at++] = (char)(0xe0 | u >> 12);
			out[at++] = (char)(0x80 | (u >> 6 & 0x3f));
			out[at++] = (char)(0x80 | (u & 0x3f));
		}
	}
	out[at] = '\0';
	return (at);
}

/* The Rock Ridge facts of one record's SUSP stream. */
typedef struct chk_rr {
	char		 name[NAME_BYTES];
	char		 target[TARGET_BYTES];
	size_t		 name_length;
	size_t		 target_length;
	uint32_t	 mode;
	uint32_t	 links;
	uint32_t	 uid;
	uint32_t	 gid;
	int64_t		 modified;
	unsigned	 areas;
	bool		 px;
	bool		 tf;
	bool		 sp;
	bool		 er;
	bool		 nm_open;
	bool		 sl_open;
	bool		 joining;
	bool		 after_root;
	bool		 component;
	uint32_t	 child_link;
	uint32_t	 parent_link;
	bool		 relocated;
} chk_rr_t;

static void
chk_sl(chk_t *c, chk_rr_t *rr, const unsigned char *p, size_t n)
{
	size_t at = 1;

	rr->sl_open = (p[0] & 1) != 0;
	while (at < n) {
		unsigned flags = p[at], length = p[at + 1];
		const char *text = (const char *)p + at + 2;

		CHK(c, at + 2 + length <= n &&
		    rr->target_length + length + 3 < TARGET_BYTES,
		    "SL component overruns its entry or the target");
		if (at + 2 + length > n ||
		    rr->target_length + length + 3 >= TARGET_BYTES)
			return;
		if (rr->component && !rr->joining && !rr->after_root)
			rr->target[rr->target_length++] = '/';
		if (flags == 0x08) {
			rr->target[rr->target_length++] = '/';
		} else if (flags == 0x02) {
			rr->target[rr->target_length++] = '.';
		} else if (flags == 0x04) {
			rr->target[rr->target_length++] = '.';
			rr->target[rr->target_length++] = '.';
		} else {
			CHK(c, (flags & ~1u) == 0, "SL component flags");
			bcopy(text, rr->target + rr->target_length, length);
			rr->target_length += length;
		}
		CHK(c, rr->target_length < TARGET_BYTES - 4, "SL too long");
		rr->component = true;
		rr->joining = (flags & 1) != 0;
		rr->after_root = flags == 0x08;
		at += 2 + length;
	}
}

/* Walk one record's SUSP entries from its system use area through every
 * continuation area, each of which must be within one block and is
 * explained and claimed as continuation.
 */
static void
chk_susp(chk_t *c, const unsigned char *area, size_t length, chk_rr_t *rr)
{
	const unsigned char *p = area;
	size_t at = 0, n;
	const unsigned char *next = NULL;
	size_t next_length = 0;

	bzero(rr, sizeof(*rr));
	for (;;) {
		if (at + 4 > length || p[at] == 0) {
			if (next == NULL)
				break;
			p = next;
			length = next_length;
			next = NULL;
			at = 0;
			continue;
		}
		n = p[at + 2];
		CHK(c, n >= 4 && at + n <= length, "SUSP entry overruns");
		CHK(c, p[at + 3] == 1, "SUSP entry version");
		if (n < 4 || at + n > length)
			return;
		if (bcmp(p + at, "CE", 2) == 0) {
			uint32_t block = both32(c, p + at + 4);
			uint32_t offset = both32(c, p + at + 12);
			uint32_t size = both32(c, p + at + 20);

			CHK(c, n == 28 && next == NULL, "CE entry");
			CHK(c, size != 0 && offset + size <= BLOCK &&
			    block < c->blocks, "CE area");
			CHK(c, ++rr->areas <= 16, "more than 16 continuation areas");
			if (size == 0 || offset + size > BLOCK ||
			    block >= c->blocks || rr->areas > 16)
				return;
			for (uint32_t i = 0; i < size; i++)
				CHK(c, c->explained[(uint64_t)block * BLOCK +
				    offset + i] == 0, "continuation bytes reused");
			chk_explain(c, (uint64_t)block * BLOCK + offset, size);
			if (c->owner[block] != 3)
				chk_claim(c, block, 1, 3);
			next = c->img + (uint64_t)block * BLOCK + offset;
			next_length = size;
		} else if (bcmp(p + at, "SP", 2) == 0) {
			CHK(c, n == 7 && p[at + 4] == 0xbe && p[at + 5] == 0xef &&
			    p[at + 6] == 0, "SP entry");
			rr->sp = true;
		} else if (bcmp(p + at, "ER", 2) == 0) {
			CHK(c, p[at + 4] == 10 && p[at + 7] == 1 &&
			    bcmp(p + at + 8, "RRIP_1991A", 10) == 0 &&
			    n == 8u + p[at + 4] + p[at + 5] + p[at + 6],
			    "ER entry");
			rr->er = true;
		} else if (bcmp(p + at, "PX", 2) == 0) {
			CHK(c, n == 36 && !rr->px, "PX entry");
			rr->mode = both32(c, p + at + 4);
			rr->links = both32(c, p + at + 12);
			rr->uid = both32(c, p + at + 20);
			rr->gid = both32(c, p + at + 28);
			rr->px = true;
		} else if (bcmp(p + at, "TF", 2) == 0) {
			unsigned flags = p[at + 4], count = 0;

			CHK(c, !rr->tf && (flags & 0xf1) == 0 &&
			    (flags & 0x02) != 0, "TF flags");
			for (unsigned b = 1; b < 4; b++)
				count += (flags >> b) & 1;
			CHK(c, n == 5 + 7 * count, "TF length");
			rr->modified = date7(c, p + at + 5);
			rr->tf = true;
		} else if (bcmp(p + at, "NM", 2) == 0) {
			CHK(c, n >= 5 && (rr->name_length == 0 || rr->nm_open),
			    "NM entry");
			CHK(c, rr->name_length + n - 5 < NAME_BYTES,
			    "NM name too long");
			if (rr->name_length + n - 5 >= NAME_BYTES)
				return;
			bcopy(p + at + 5, rr->name + rr->name_length, n - 5);
			rr->name_length += n - 5;
			rr->nm_open = (p[at + 4] & 1) != 0;
		} else if (bcmp(p + at, "SL", 2) == 0) {
			CHK(c, rr->target_length == 0 || rr->sl_open,
			    "SL after the target ended");
			chk_sl(c, rr, p + at + 4, n - 4);
		} else if (bcmp(p + at, "CL", 2) == 0 ||
		    bcmp(p + at, "PL", 2) == 0) {
			CHK(c, n == 12, "relocation pointer length");
			if (n != 12)
				return;
			if (p[at] == 'C')
				rr->child_link = both32(c, p + at + 4);
			else
				rr->parent_link = both32(c, p + at + 4);
		} else if (bcmp(p + at, "RE", 2) == 0) {
			CHK(c, n == 4, "relocation marker length");
			rr->relocated = true;
		} else {
			CHK(c, false, "unexpected SUSP entry");
		}
		at += n;
	}
	CHK(c, !rr->nm_open && !rr->sl_open && !rr->joining,
	    "a name or target still continues");
	CHK(c, rr->target_length == 0 || !rr->after_root,
	    "slash-only target lacks its terminal empty text component");
	rr->name[rr->name_length] = '\0';
	rr->target[rr->target_length] = '\0';
}

static void chk_directory(chk_t *, unsigned, const char *, const char *,
    const unsigned char *, size_t, uint32_t, uint32_t, uint32_t);

static chk_entry_t *
chk_add(chk_t *c, unsigned h)
{
	CHK(c, c->count[h] < CHK_ENTRIES, "too many entries for the checker");
	if (c->count[h] >= CHK_ENTRIES)
		return (NULL);
	bzero(&c->entries[h][c->count[h]], sizeof(chk_entry_t));
	return (&c->entries[h][c->count[h]++]);
}

/* A file's sections: the data of each, contiguous and ascending, every
 * one but the last a whole number of blocks; explained and claimed as
 * data, which both hierarchies share.
 */
static void
chk_data(chk_t *c, chk_entry_t *e, uint32_t extent, uint32_t length,
    bool first)
{
	uint64_t offset = (uint64_t)extent * BLOCK;

	if (first) {
		e->extent = extent;
		e->hash = HASH_START;
	} else {
		CHK(c, e->size % BLOCK == 0 && extent ==
		    e->extent + e->size / BLOCK, "sections not contiguous");
	}
	if (length == 0) {
		CHK(c, extent == 0, "empty section names a block");
	} else {
		CHK(c, extent > 18 && offset + length <= c->length,
		    "file data outside the volume");
		if (offset + length <= c->length) {
			e->hash = hash_update(e->hash, c->img + offset, length);
			chk_explain(c, offset, length);
			chk_claim(c, extent, (length + BLOCK - 1) / BLOCK, 2);
		}
	}
	e->size += length;
	e->sections++;
}

/* Check one child record of a primary directory, with its Rock Ridge
 * stream, and fill its entry.
 */
static void
chk_primary_child(chk_t *c, chk_entry_t *e, const unsigned char *r,
    const char *path, const char *display, uint32_t *subdirectories)
{
	size_t n = r[32], used = 33 + n + (n % 2 == 0 ? 1 : 0);
	bool directory = (r[25] & 0x02) != 0;
	chk_rr_t rr;

	chk_susp(c, r + used, r[0] - used, &rr);
	check_primary_identifier(c, r + 33, n, directory || rr.child_link != 0);
	CHK(c, rr.px && rr.tf && rr.name_length > 0 && !rr.sp && !rr.er,
	    "primary record lacks PX, TF or NM");
	e->mode = rr.mode;
	e->uid = rr.uid;
	e->gid = rr.gid;
	e->links = rr.links;
	e->has_px = rr.px;
	e->modified = rr.modified;
	e->extent = le32(r + 2);
	e->child_link = rr.child_link;
	e->relocated = rr.relocated;
	CHK(c, rr.parent_link == 0 && !(rr.relocated && rr.child_link != 0),
	    "child relocation entries");
	CHK(c, rr.modified == date7(c, r + 18),
	    "recording date differs from TF modification");
	snprintf(e->path, sizeof(e->path), "%s/%s", path, rr.name);
	snprintf(e->display, sizeof(e->display), "%s/%.*s", display,
	    directory || rr.child_link != 0 ? (int)n : (int)n - 2,
	    (const char *)r + 33);
	bcopy(rr.target, e->target, rr.target_length + 1);
	switch (rr.mode & S_TYPE) {
	case S_DIR:
		CHK(c, directory || rr.child_link != 0, "PX directory without flag or CL");
		if (rr.child_link != 0)
			CHK(c, !directory && le32(r + 2) == 0 && le32(r + 10) == 0,
			    "CL placeholder is not an empty file record");
		(*subdirectories)++;
		break;
	case S_FILE:
		CHK(c, !directory && rr.links == 1, "PX file");
		break;
	case S_LINK:
		CHK(c, !directory && rr.links == 1 && rr.target_length > 0 &&
		    le32(r + 2) == 0 && le32(r + 10) == 0, "PX link");
		break;
	case S_FIFO:
		CHK(c, !directory && rr.links == 1 && le32(r + 2) == 0 &&
		    le32(r + 10) == 0, "PX FIFO");
		break;
	default:
		CHK(c, false, "PX type");
	}
	CHK(c, (rr.mode & S_TYPE) == S_LINK || rr.target_length == 0,
	    "SL on a record that is not a link");
}

/* Walk directory extent of hierarchy h (0 primary, 1 Joliet): its two dot
 * records, then its children in order, every record within a block,
 * every block's tail zero; recurse into subdirectories.
 */
static void
chk_directory(chk_t *c, unsigned h, const char *path, const char *display,
    const unsigned char *identifier, size_t identifier_length,
    uint32_t extent, uint32_t length, uint32_t parent)
{
	unsigned char previous[256];
	size_t previous_length = 0;
	bool previous_directory = false, open_run = false;
	chk_entry_t *e = NULL;
	uint32_t subdirectories = 0, self_links = 0;
	unsigned index = 0;
	chk_rr_t rr;
	chk_dir_t *d;
	unsigned depth_limit = h == 0 ? 8 : 257;

	CHK(c, ++c->depth <= depth_limit, "hierarchy exceeds its depth bound");
	CHK(c, length != 0 && length % BLOCK == 0 && extent > 18 &&
	    (uint64_t)extent + length / BLOCK <= c->blocks,
	    "directory extent");
	if (c->depth > depth_limit || length == 0 || length % BLOCK != 0 ||
	    (uint64_t)extent + length / BLOCK > c->blocks) {
		c->depth--;
		return;
	}
	chk_claim(c, extent, length / BLOCK, 1);
	CHK(c, c->dir_count[h] < CHK_ENTRIES, "too many directories");
	if (c->dir_count[h] >= CHK_ENTRIES) {
		c->depth--;
		return;
	}
	d = &c->dirs[h][c->dir_count[h]++];
	d->extent = extent;
	d->parent = parent;
	bcopy(identifier, d->identifier, identifier_length);
	d->length = identifier_length;

	for (uint32_t b = 0; b < length / BLOCK; b++) {
		const unsigned char *block = c->img +
		    ((uint64_t)extent + b) * BLOCK;
		size_t at = 0;

		while (at < BLOCK && block[at] != 0) {
			const unsigned char *r = block + at;
			size_t n = r[32], used = 33 + n + (n % 2 == 0 ? 1 : 0);
			bool directory = (r[25] & 0x02) != 0;

			CHK(c, r[0] % 2 == 0 && r[0] >= 34 && r[0] >= used &&
			    at + r[0] <= BLOCK && n >= 1, "record framing");
			if (r[0] < 34 || at + r[0] > BLOCK || r[0] < used)
				break;
			chk_explain(c, ((uint64_t)extent + b) * BLOCK + at, r[0]);
			CHK(c, r[1] == 0 && r[26] == 0 && r[27] == 0 &&
			    both16(c, r + 28) == 1 && (r[25] & ~0x83u) == 0 &&
			    (n % 2 != 0 || r[33 + n] == 0),
			    "record fields");
			both32(c, r + 2);
			both32(c, r + 10);
			date7(c, r + 18);
			if (index < 2) {
				CHK(c, n == 1 && r[33] == index && directory,
				    "dot record");
				CHK(c, le32(r + 2) == (index == 0 ? extent : parent),
				    "dot record extent");
				if (index == 0)
					CHK(c, le32(r + 10) == length,
					    "dot record length");
				if (h == 0) {
					chk_susp(c, r + used, r[0] - used, &rr);
					CHK(c, rr.px && rr.tf &&
					    (rr.mode & S_TYPE) == S_DIR &&
					    rr.name_length == 0,
					    "dot record Rock Ridge");
					CHK(c, rr.sp == (index == 0 && extent ==
					    c->root_extent[0]) && rr.er == rr.sp,
					    "SP and ER only on the root's dot");
					if (rr.sp)
						CHK(c, bcmp(r + used, "SP", 2) == 0,
						    "SP not first");
					if (index == 0)
						self_links = rr.links;
					else
						d->logical_parent = rr.parent_link;
				} else {
					CHK(c, r[0] == 34, "Joliet dot record");
				}
				index++;
				at += r[0];
				continue;
			}

			/* A later section of the open run. */
			if (open_run) {
				CHK(c, n == previous_length && bcmp(r + 33,
				    previous, n) == 0 && !directory,
				    "section run changes identifier");
				if (h == 0) {
					chk_susp(c, r + used, r[0] - used, &rr);
					CHK(c, rr.px && rr.tf &&
					    strcmp(rr.name, strrchr(e->path, '/') + 1) == 0 &&
					    rr.mode == e->mode && rr.uid == e->uid &&
					    rr.gid == e->gid && rr.modified == e->modified,
					    "section Rock Ridge metadata differs");
				} else {
					CHK(c, r[0] == (used + 1) / 2 * 2,
					    "Joliet section carries system use");
				}
				chk_data(c, e, le32(r + 2), le32(r + 10), false);
				open_run = (r[25] & 0x80) != 0;
				if (open_run)
					CHK(c, le32(r + 10) == 0xfffff800u,
					    "middle section length");
				at += r[0];
				continue;
			}

			if (h == 0)
				CHK(c, strlen(display) + n <= 255,
				    "primary physical path exceeds 255 bytes");
			if (previous_length != 0) {
				int order = h == 0 ? primary_order(previous,
				    previous_length, previous_directory, r + 33,
				    n, directory) : joliet_order(previous,
				    previous_length, previous_directory, r + 33,
				    n, directory);

				CHK(c, order < 0, "records out of order");
				if (order >= 0 && !c->quiet)
					fprintf(stderr, "  %.*s then %.*s\n",
					    (int)previous_length, previous, (int)n,
					    r + 33);
				CHK(c, n != previous_length ||
				    bcmp(previous, r + 33, n) != 0,
				    "repeated identifier");
			}
			bcopy(r + 33, previous, n);
			previous_length = n;
			previous_directory = directory;
			e = chk_add(c, h);
			if (e == NULL)
				return;
			bcopy(r + 33, e->identifier, n);
			e->identifier_length = n;
			e->directory = directory;
			e->containing_directory = extent;
			if (h == 0) {
				chk_primary_child(c, e, r, path, display,
				    &subdirectories);
			} else {
				char name[NAME_BYTES];
				size_t units = directory ? n : n - 4;

				CHK(c, n % 2 == 0 && n <= 128 && (directory ||
				    (n >= 6 && be16(r + 33 + n - 4) == ';' &&
				    be16(r + 33 + n - 2) == '1')),
				    "Joliet identifier");
				CHK(c, r[0] == 34 + n, "Joliet record length");
				joliet_utf8(r + 33, units, name);
				snprintf(e->path, sizeof(e->path), "%s/%s", path,
				    name);
				bcopy(e->path, e->display, sizeof(e->path));
				if (directory)
					subdirectories++;
			}
			if (directory) {
				chk_directory(c, h, e->path, e->display, r + 33,
				    n, le32(r + 2), le32(r + 10), extent);
			} else {
				chk_data(c, e, le32(r + 2), le32(r + 10), true);
				open_run = (r[25] & 0x80) != 0;
				if (open_run)
					CHK(c, le32(r + 10) == 0xfffff800u,
					    "first of several sections");
			}
			at += r[0];
		}
		CHK(c, all_zero(block + at, BLOCK - at),
		    "directory block tail not zero");
	}
	CHK(c, index == 2 && !open_run, "directory lacks dot records or ends "
	    "in a section run");
	if (h == 0)
		CHK(c, self_links == 2 + subdirectories, "directory link count");
	c->depth--;
}

/* One hierarchy's type-L and type-M path tables: equal records, the root
 * first, parents numbered below their children and never decreasing,
 * each record naming a directory the walk found under the parent the
 * walk found, and exactly as many records as directories.
 */
static void
chk_path_tables(chk_t *c, unsigned h)
{
	const unsigned char *l, *m;
	uint32_t *extents;
	size_t at = 0, count = 0, n;
	unsigned previous_parent = 1;

	if ((uint64_t)c->path_l[h] * BLOCK + c->path_bytes[h] > c->length ||
	    (uint64_t)c->path_m[h] * BLOCK + c->path_bytes[h] > c->length) {
		CHK(c, false, "path table outside the image");
		return;
	}
	l = c->img + (uint64_t)c->path_l[h] * BLOCK;
	m = c->img + (uint64_t)c->path_m[h] * BLOCK;
	chk_explain(c, (uint64_t)c->path_l[h] * BLOCK, c->path_bytes[h]);
	chk_explain(c, (uint64_t)c->path_m[h] * BLOCK, c->path_bytes[h]);
	chk_claim(c, c->path_l[h], (c->path_bytes[h] + BLOCK - 1) / BLOCK, 1);
	chk_claim(c, c->path_m[h], (c->path_bytes[h] + BLOCK - 1) / BLOCK, 1);
	extents = calloc(CHK_ENTRIES + 1, sizeof(*extents));
	CHK(c, extents != NULL, "allocation");
	while (at < c->path_bytes[h] && count < CHK_ENTRIES) {
		const chk_dir_t *d = NULL;
		unsigned parent;

		n = l[at];
		CHK(c, n >= 1 && at + 8 + n + n % 2 <= c->path_bytes[h] &&
		    l[at + 1] == 0 && m[at] == n && m[at + 1] == 0,
		    "path record header");
		if (n == 0 || at + 8 + n + n % 2 > c->path_bytes[h])
			break;
		CHK(c, le32(l + at + 2) == be32(m + at + 2) &&
		    le16(l + at + 6) == be16(m + at + 6) &&
		    bcmp(l + at + 8, m + at + 8, n + n % 2) == 0,
		    "type-L and type-M records differ");
		CHK(c, n % 2 == 0 || l[at + 8 + n] == 0, "path record pad");
		parent = le16(l + at + 6);
		extents[count] = le32(l + at + 2);
		if (count == 0) {
			CHK(c, n == 1 && l[at + 8] == 0 && parent == 1 &&
			    extents[0] == c->root_extent[h], "root path record");
		} else {
			CHK(c, parent >= previous_parent && parent <= count,
			    "path record parent number");
			previous_parent = parent;
		}
		for (size_t i = 0; i < c->dir_count[h]; i++)
			if (c->dirs[h][i].extent == extents[count])
				d = &c->dirs[h][i];
		CHK(c, d != NULL, "path record names no directory");
		if (d != NULL && count > 0) {
			CHK(c, parent >= 1 && d->parent == extents[parent - 1],
			    "path record parent differs from the walk");
			CHK(c, d->length == n && bcmp(d->identifier,
			    l + at + 8, n) == 0,
			    "path record identifier differs from the walk");
		}
		count++;
		at += 8 + n + n % 2;
	}
	CHK(c, at == c->path_bytes[h] && count == c->dir_count[h],
	    "path table size or record count");
	free(extents);
}

/* Reconcile the physical tree with its logical placeholders. This checks
 * CL/RE/PL triples without building a second logical-tree walker.
 */
static void
chk_relocations(chk_t *c)
{
	const chk_entry_t *container = NULL, *placeholder;
	const chk_dir_t *directory;
	size_t children = 0, relocated = 0, matches;

	for (size_t i = 0; i < c->count[0]; i++) {
		const chk_entry_t *entry = &c->entries[0][i];

		if (strcmp(entry->path, "/.rr_moved") == 0)
			container = entry;
		if (entry->child_link != 0)
			children++;
	}
	for (size_t i = 0; i < c->count[0]; i++) {
		const chk_entry_t *entry = &c->entries[0][i];

		if (!entry->relocated)
			continue;
		relocated++;
		placeholder = NULL;
		directory = NULL;
		matches = 0;
		for (size_t j = 0; j < c->count[0]; j++) {
			if (c->entries[0][j].child_link == entry->extent) {
				placeholder = &c->entries[0][j];
				matches++;
			}
		}
		for (size_t j = 0; j < c->dir_count[0]; j++)
			if (c->dirs[0][j].extent == entry->extent)
				directory = &c->dirs[0][j];
		CHK(c, matches == 1 && placeholder != NULL && directory != NULL &&
		    container != NULL, "relocation lacks one CL, RE, PL or container");
		if (placeholder == NULL || directory == NULL || container == NULL)
			continue;
		CHK(c, entry->directory && entry->containing_directory == container->extent,
		    "RE record outside the relocation container");
		CHK(c, directory->logical_parent == placeholder->containing_directory,
		    "PL does not name the logical parent");
		CHK(c, directory->parent == container->extent,
		    "relocated dot-dot does not name the physical parent");
		CHK(c, strcmp(strrchr(entry->path, '/'), strrchr(placeholder->path, '/')) == 0 &&
		    entry->mode == placeholder->mode && entry->uid == placeholder->uid &&
		    entry->gid == placeholder->gid && entry->links == placeholder->links &&
		    entry->modified == placeholder->modified,
		    "logical and physical directory metadata differ");
	}
	CHK(c, children == relocated, "CL and RE counts differ");
}

/* A descriptor's identifier field of width bytes: text padded with
 * spaces, or for Joliet the same in UCS-2 with a final zero byte when the
 * width is odd.
 */
static void
chk_text(chk_t *c, const unsigned char *p, size_t width, const char *text,
    bool joliet)
{
	size_t length = strlen(text);

	for (size_t i = 0; i < (joliet ? width / 2 : width); i++) {
		unsigned expected = i < length ? (unsigned char)text[i] : ' ';
		unsigned actual = joliet ? be16(p + 2 * i) : p[i];

		CHK(c, actual == expected, "descriptor identifier field");
	}
	if (joliet && width % 2 != 0)
		CHK(c, p[width - 1] == 0, "descriptor identifier pad");
}

/* A primary (h 0) or Joliet (h 1) descriptor at block 16 + h. */
static void
chk_descriptor(chk_t *c, unsigned h, const chk_expect_t *expect)
{
	const unsigned char *d = c->img + (uint64_t)(16 + h) * BLOCK;
	const char *label = expect->label == NULL ? "CDROM" : expect->label;
	bool joliet = h == 1;

	CHK(c, d[0] == 1 + h && bcmp(d + 1, "CD001", 5) == 0 && d[6] == 1 &&
	    d[7] == 0 && d[881] == 1 && d[882] == 0, "descriptor header");
	CHK(c, all_zero(d + 72, 8) && all_zero(d + 1395, BLOCK - 1395) &&
	    all_zero(d + 883, 512), "descriptor reserved bytes");
	if (joliet)
		CHK(c, bcmp(d + 88, "%/E", 3) == 0 && all_zero(d + 91, 29),
		    "Joliet escape sequences");
	else
		CHK(c, all_zero(d + 88, 32), "primary escape bytes");
	CHK(c, both32(c, d + 80) == c->blocks && both16(c, d + 120) == 1 &&
	    both16(c, d + 124) == 1 && both16(c, d + 128) == BLOCK,
	    "descriptor geometry");
	c->path_bytes[h] = both32(c, d + 132);
	c->path_l[h] = le32(d + 140);
	c->path_m[h] = be32(d + 148);
	CHK(c, le32(d + 144) == 0 && be32(d + 152) == 0,
	    "optional path table copies");
	CHK(c, d[156] == 34 && d[156 + 32] == 1 && d[156 + 33] == 0 &&
	    d[156 + 25] == 0x02, "root record");
	c->root_extent[h] = both32(c, d + 156 + 2);
	c->root_length[h] = both32(c, d + 156 + 10);
	chk_text(c, d + 8, 32, "", joliet);
	chk_text(c, d + 190, 128, "", joliet);
	chk_text(c, d + 318, 128, "", joliet);
	chk_text(c, d + 446, 128, "", joliet);
	chk_text(c, d + 574, 128, "FSIMG", joliet);
	for (unsigned i = 0; i < 3; i++)
		chk_text(c, d + 702 + 37 * i, 37, "", joliet);
	CHK(c, bcmp(d + 813, d + 830, 17) == 0 &&
	    bcmp(d + 847, "0000000000000000", 16) == 0 && d[863] == 0 &&
	    bcmp(d + 864, "0000000000000000", 16) == 0 && d[880] == 0,
	    "descriptor dates");
	if (joliet) {
		char decoded[64];

		joliet_utf8(d + 40, 32, decoded);
		for (size_t i = strlen(decoded); i > 0 &&
		    decoded[i - 1] == ' '; i--)
			decoded[i - 1] = '\0';
		CHK(c, strcmp(decoded, label) == 0, "Joliet label");
	} else {
		size_t length = strlen(label);
		unsigned char projected[32];
		size_t characters = 0;

		/* The label is valid BMP UTF-8. Each leading byte starts one
		 * character; continuation bytes add no primary character.
		 */
		memset(projected, ' ', sizeof(projected));
		for (size_t i = 0; i < length; i++) {
			unsigned char e = (unsigned char)label[i];

			if ((e & 0xc0) == 0x80)
				continue;
			if (e >= 'a' && e <= 'z')
				e = (unsigned char)(e - 'a' + 'A');
			if (!((e >= 'A' && e <= 'Z') ||
			    (e >= '0' && e <= '9') || e == '_'))
				e = '_';
			CHECK(characters < sizeof(projected));
			projected[characters++] = e;
		}

		for (size_t i = 0; i < 32; i++) {
			CHK(c, d[40 + i] == projected[i],
			    "primary label projection");
		}
	}
	chk_explain(c, (uint64_t)(16 + h) * BLOCK, BLOCK);
}

static void
chk_free(chk_t *c)
{
	free(c->explained);
	free(c->owner);
	for (unsigned h = 0; h < 2; h++) {
		free(c->entries[h]);
		free(c->dirs[h]);
	}
}

/* Check a whole image; the state stays in c for the caller. */
static void
chk_image(chk_t *c, const unsigned char *img, uint64_t length,
    const chk_expect_t *expect)
{
	const unsigned char *t;
	static const unsigned char root_id[1] = { 0 };

	bzero(c, sizeof(*c));
	c->img = img;
	c->length = length;
	c->quiet = expect->quiet;
	c->level = expect->level == 0 ? 3 : expect->level;
	c->explained = calloc(1, (size_t)length);
	c->blocks = (uint32_t)(length / BLOCK);
	c->owner = calloc(1, c->blocks + 1);
	for (unsigned h = 0; h < 2; h++) {
		c->entries[h] = calloc(CHK_ENTRIES, sizeof(chk_entry_t));
		c->dirs[h] = calloc(CHK_ENTRIES, sizeof(chk_dir_t));
		CHK(c, c->entries[h] != NULL && c->dirs[h] != NULL,
		    "allocation");
	}
	CHK(c, c->explained != NULL && c->owner != NULL, "allocation");
	CHK(c, length % BLOCK == 0 && c->blocks >= 19, "image size");
	if (c->errors != 0)
		return;
	chk_claim(c, 0, 16, 1);
	chk_claim(c, 16, 3, 1);
	chk_descriptor(c, 0, expect);
	chk_descriptor(c, 1, expect);
	t = img + 18 * BLOCK;
	CHK(c, t[0] == 255 && bcmp(t + 1, "CD001", 5) == 0 && t[6] == 1 &&
	    all_zero(t + 7, BLOCK - 7), "terminator");
	chk_explain(c, 18 * BLOCK, BLOCK);
	if (c->errors != 0)
		return;
	for (unsigned h = 0; h < 2; h++) {
		chk_directory(c, h, "", "", root_id, 1, c->root_extent[h],
		    c->root_length[h], c->root_extent[h]);
		chk_path_tables(c, h);
	}
	chk_relocations(c);
	for (uint64_t i = 0; i < length; i++) {
		if (c->explained[i] == 0 && img[i] != 0) {
			CHK(c, false, "unexplained nonzero byte");
			if (!c->quiet)
				fprintf(stderr, "  at byte %llu (block %llu)\n",
				    (unsigned long long)i,
				    (unsigned long long)(i / BLOCK));
			break;
		}
	}
}

static const chk_entry_t *
chk_find(const chk_t *c, unsigned h, const char *path)
{
	for (size_t i = 0; i < c->count[h]; i++)
		if (strcmp(c->entries[h][i].path, path) == 0)
			return (&c->entries[h][i]);
	return (NULL);
}

static const chk_entry_t *
chk_find_display(const chk_t *c, const char *display)
{
	for (size_t i = 0; i < c->count[0]; i++)
		if (strcmp(c->entries[0][i].display, display) == 0)
			return (&c->entries[0][i]);
	return (NULL);
}

/*
 * Fixtures.
 */

/* One catalog entry to add: a provider source s supplies a file's
 * content, zero for the zero source; a link's target is target.
 */
typedef struct spec {
	const char	*path;
	fsimg_entry_kind_t kind;
	uint64_t	 size;
	uint64_t	 source;
	const fsimg_metadata_t *metadata;
	const char	*target;
} spec_t;

typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_writer_t	*writer;
	fsimg_stats_t	 baseline;
	const char	*targets[256];
	size_t		 target_count;
	char		 directory[64];
	int		 parent;
	unsigned	 losses;
	uint32_t	 lost_fields;
	uint64_t	 target_reads;
	uint64_t	 cancel_after;
	uint64_t	 polls;
	bool		 changed;
	bool		 eof;
	bool		 flip_target;
	bool		 refuse_target_read;
	bool		 join_target_components;
} fixture_t;

typedef struct memory_sink {
	unsigned char	*bytes;
	uint64_t	 length;
	uint64_t	 writes;
	size_t		 max_write;
	bool		 fail_write;
	bool		 fail_flush;
} memory_sink_t;

static unsigned char
content_byte(uint64_t id, uint64_t offset)
{
	return ((unsigned char)(offset * 37 + id * 11 + (offset >> 13)));
}

static uint64_t
expected_hash(const spec_t *spec)
{
	unsigned char b;
	uint64_t hash = HASH_START;

	for (uint64_t i = 0; i < spec->size; i++) {
		b = spec->source == 0 ? 0 : content_byte(spec->source, i);
		hash = hash_update(hash, &b, 1);
	}
	return (hash);
}

static fsimg_status_t
provider_validate(void *argument, uint64_t id FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *fixture = argument;

	return (fixture->changed ? FSIMG_ESOURCE_CHANGED : FSIMG_OK);
}

/* Short reads of at most 1000 bytes exercise the source adapter; with eof
 * set a file ends after 1000 bytes. A link's target is its registered
 * text, its last byte changed once flip_target is set.
 */
static fsimg_status_t
provider_read(void *argument, uint64_t id, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *fixture = argument;
	unsigned char *bytes = buffer;

	if (id >= TARGET_SOURCE) {
		const char *target = fixture->targets[id - TARGET_SOURCE];
		size_t size = strlen(target);

		fixture->target_reads++;

		if (fixture->refuse_target_read)
			return (FSIMG_EUNSUPPORTED);
		*transferred = 0;
		for (size_t i = 0; i < length && offset + i < size; i++) {
			bytes[i] = (unsigned char)target[offset + i];
			if (fixture->flip_target && offset + i == size - 1)
				bytes[i] ^= 1;
			if (fixture->join_target_components && offset + i == 255)
				bytes[i] = 'a';
			(*transferred)++;
		}
		return (FSIMG_OK);
	}
	if (fixture->eof && offset >= 1000) {
		*transferred = 0;
		return (FSIMG_OK);
	}
	if (length > 1000)
		length = 1000;
	for (size_t i = 0; i < length; i++)
		bytes[i] = content_byte(id, offset + i);
	*transferred = length;
	return (FSIMG_OK);
}

static void
loss_report(void *argument, const char *path, uint32_t field)
{
	fixture_t *fixture = argument;

	CHECK(path[0] == '/');
	fixture->losses++;
	fixture->lost_fields |= field;
}

static bool
cancel_poll(void *argument)
{
	fixture_t *fixture = argument;

	return (fixture->cancel_after != 0 &&
	    ++fixture->polls >= fixture->cancel_after);
}

/* Open a fixture over the registered writer. pages sizes the metadata
 * cache; scratch false keeps every metadata object in memory. A writer
 * construction failure leaves fixture->writer NULL and returns it.
 */
static fsimg_status_t
fixture_open_with(fixture_t *fixture, const fsimg_write_options_t *write_options,
    uint32_t pages, bool scratch)
{
	fsimg_options_t options;
	fsimg_source_ops_t source_ops;
	fsimg_status_t status;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->directory, sizeof(fixture->directory),
	    "iso9660-write-XXXXXX");
	CHECK(mkdtemp(fixture->directory) != NULL);
	fixture->parent = open(fixture->directory,
	    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fixture->parent != -1);
	fsimg_options_init(&options);
	options.memory_bytes = UINT64_C(64) * 1024 * 1024;
	options.scratch_directory_fd = scratch ? fixture->parent : -1;
	options.max_open_files = 64;
	options.loss = loss_report;
	options.loss_argument = fixture;
	options.cancelled = cancel_poll;
	options.cancel_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_test_meta_cache(fixture->ctx, pages, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline, NULL) ==
	    FSIMG_OK);
	status = fsimg_writer_new(fixture->ctx, "iso9660", write_options,
	    &fixture->writer, NULL);
	if (status != FSIMG_OK)
		return (status);
	fsimg_source_ops_init(&source_ops);
	source_ops.validate = provider_validate;
	source_ops.read_at = provider_read;
	CHECK(fsimg_writer_set_sources(fixture->writer, &source_ops, fixture,
	    NULL) == FSIMG_OK);
	return (FSIMG_OK);
}

static void
fixture_open(fixture_t *fixture, const fsimg_write_options_t *write_options)
{
	CHECK(fixture_open_with(fixture, write_options, 1, true) == FSIMG_OK);
}

/* Free a fixture and require that it released every resource. */
static void
fixture_close(fixture_t *fixture)
{
	fsimg_stats_t stats;

	fsimg_writer_free(fixture->writer);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == fixture->baseline.memory_bytes);
	CHECK(stats.scratch_bytes == 0);
	CHECK(stats.descriptors == fixture->baseline.descriptors);
	fsimg_ctx_free(fixture->ctx);
	CHECK(close(fixture->parent) == 0);
	CHECK(rmdir(fixture->directory) == 0);
}

static fsimg_status_t
add_one(fixture_t *fixture, const spec_t *spec)
{
	fsimg_entry_t entry;
	fsimg_source_t source;
	fsimg_error_t error;
	fsimg_status_t status;

	bzero(&entry, sizeof(entry));
	entry.path = spec->path;
	entry.kind = spec->kind;
	entry.size = spec->size;
	if (spec->metadata != NULL)
		entry.metadata = *spec->metadata;
	bzero(&source, sizeof(source));
	source.kind = spec->source == 0 ? FSIMG_SOURCE_ZERO :
	    FSIMG_SOURCE_PROVIDER;
	source.id = spec->source;
	if (spec->kind == FSIMG_ENTRY_SYMLINK) {
		CHECK(fixture->target_count < 256);
		fixture->targets[fixture->target_count] = spec->target;
		source.kind = FSIMG_SOURCE_PROVIDER;
		source.id = TARGET_SOURCE + fixture->target_count++;
		entry.size = strlen(spec->target);
	}
	status = fsimg_writer_add(fixture->writer, &entry,
	    spec->kind == FSIMG_ENTRY_FILE || spec->kind == FSIMG_ENTRY_SYMLINK ?
	    &source : NULL, &error);
	if (status != FSIMG_OK && getenv("ISO9660_WRITE_DEBUG") != NULL)
		fprintf(stderr, "add %s: %d %s\n", spec->path, status,
		    error.message);
	return (status);
}

static void
add_specs(fixture_t *fixture, const spec_t *specs, size_t count, bool reverse)
{
	for (size_t i = 0; i < count; i++)
		CHECK(add_one(fixture, &specs[reverse ? count - 1 - i : i]) ==
		    FSIMG_OK);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	*length = sink->length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	bcopy(sink->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	*transferred = 0;
	sink->writes++;
	if (sink->fail_write)
		return (FSIMG_EIO);
	CHECK(offset <= sink->length && length <= sink->length - offset);
	if (sink->max_write != 0 && length > sink->max_write)
		length = sink->max_write;
	bcopy(buffer, sink->bytes + offset, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_flush(void *argument, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	return (sink->fail_flush ? FSIMG_EIO : FSIMG_OK);
}

/* An I/O object over a sink; the writable one has no zero_range, so the
 * common emitter overwrites the poison itself.
 */
static fsimg_status_t
memory_io(fsimg_ctx_t *ctx, memory_sink_t *sink, bool writable,
    fsimg_io_t **out)
{
	fsimg_io_ops_t ops;

	fsimg_io_ops_init(&ops);
	ops.get_size = memory_size;
	ops.read_at = memory_read;
	if (writable) {
		ops.write_at = memory_write;
		ops.flush = memory_flush;
	}
	*out = NULL;
	return (fsimg_io_from_callbacks(ctx, &ops, sink, out, NULL));
}

/* Plan, allocate a poisoned sink of the planned size and emit into it. */
static fsimg_status_t
emit_memory(fixture_t *fixture, memory_sink_t *sink, fsimg_error_t *error)
{
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *output = NULL;
	fsimg_error_t detail;
	fsimg_status_t status;

	bzero(sink, sizeof(*sink));
	status = fsimg_writer_plan(fixture->writer, &plan, &detail);
	if (status == FSIMG_OK) {
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		sink->length = info.volume_bytes;
		sink->bytes = malloc((size_t)info.volume_bytes);
		CHECK(sink->bytes != NULL);
		memset(sink->bytes, POISON, (size_t)info.volume_bytes);
		status = memory_io(fixture->ctx, sink, true, &output);
		if (status == FSIMG_OK)
			status = fsimg_writer_emit(fixture->writer, output,
			    &detail);
		fsimg_io_free(output);
	}
	if (error != NULL)
		*error = detail;
	else if (status != FSIMG_OK && status != FSIMG_ENOMEM &&
	    status != FSIMG_ECANCELLED && status != FSIMG_EIO)
		fprintf(stderr, "emit: %d %s %s\n", status, detail.message,
		    detail.path);
	return (status);
}

static void
reproducible_options(fsimg_write_options_t *options)
{
	fsimg_write_options_init(options);
	options->flags |= FSIMG_WRITE_REPRODUCIBLE;
	options->epoch = 1700000000;
}

/*
 * Reading back.
 */

static const spec_t *
spec_find(const spec_t *specs, size_t count, const char *path)
{
	for (size_t i = 0; i < count; i++)
		if (strcmp(specs[i].path, path) == 0)
			return (&specs[i]);
	return (NULL);
}

/* The hash of the current entry's content through the reader. */
static uint64_t
read_hash(fsimg_reader_t *reader, uint64_t size)
{
	unsigned char buffer[4096];
	uint64_t hash = HASH_START;
	size_t length;

	for (uint64_t offset = 0; offset < size; offset += length) {
		CHECK(fsimg_reader_read_at(reader, offset, buffer,
		    sizeof(buffer), &length, NULL) == FSIMG_OK);
		CHECK(length > 0);
		hash = hash_update(hash, buffer, length);
	}
	return (hash);
}

/* Read the image in one view and compare every entry: in the Rock Ridge
 * view with the specs, kind, size, content, owner, mode and modification
 * time; in the Joliet and primary views with the checker's entries of
 * that hierarchy, kind, size and content. Then verify it completely,
 * payload included.
 */
static void
check_view(fsimg_ctx_t *ctx, memory_sink_t *sink, fsimg_iso_view_t view,
    const chk_t *c, const spec_t *specs, size_t count)
{
	fsimg_read_options_t options;
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	const fsimg_entry_t *entry;
	const chk_entry_t *e;
	const spec_t *spec;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	uint64_t delivered = 0;
	fsimg_status_t status;

	CHECK(memory_io(ctx, sink, false, &input) == FSIMG_OK);
	fsimg_read_options_init(&options);
	options.iso_view = view;
	CHECK(fsimg_reader_open(ctx, input, &options, &reader, NULL) ==
	    FSIMG_OK);
	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		delivered++;
		switch (view) {
		case FSIMG_ISO_VIEW_JOLIET:
			e = chk_find(c, 1, entry->path);
			break;
		case FSIMG_ISO_VIEW_PRIMARY:
			e = chk_find_display(c, entry->path);
			break;
		default:
			e = chk_find(c, 0, entry->path);
			break;
		}
		if (e == NULL)
			fprintf(stderr, "view %d: unexpected %s\n", (int)view,
			    entry->path);
		CHECK(e != NULL);
		CHECK((entry->kind == FSIMG_ENTRY_DIRECTORY) == e->directory);
		if (entry->kind == FSIMG_ENTRY_FILE) {
			CHECK(entry->size == e->size);
			CHECK(read_hash(reader, entry->size) == e->hash);
		}
		if (view != FSIMG_ISO_VIEW_ROCKRIDGE &&
		    view != FSIMG_ISO_VIEW_AUTO)
			continue;
		spec = spec_find(specs, count, entry->path);
		CHECK(spec != NULL);
		CHECK(entry->kind == spec->kind);
		CHECK((entry->metadata.mode & 07777) == (e->mode & 07777));
		CHECK(entry->metadata.uid == e->uid &&
		    entry->metadata.gid == e->gid);
		CHECK(entry->metadata.modified.seconds == e->modified);
		if (spec->kind == FSIMG_ENTRY_FILE)
			CHECK(e->hash == expected_hash(spec));
		if (spec->kind == FSIMG_ENTRY_SYMLINK) {
			CHECK(strcmp(e->target, spec->target) == 0);
			CHECK(entry->size == strlen(spec->target));
			CHECK(read_hash(reader, entry->size) ==
			    hash_update(HASH_START,
			    (const unsigned char *)spec->target,
			    strlen(spec->target)));
		}
	}
	CHECK(status == FSIMG_DONE);
	if (view == FSIMG_ISO_VIEW_JOLIET)
		CHECK(delivered == c->count[1]);
	else
		CHECK(delivered == c->count[0] && delivered == count);
	fsimg_reader_free(reader);

	CHECK(fsimg_reader_open(ctx, input, &options, &reader, NULL) ==
	    FSIMG_OK);
	status = fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "verify view %d: %d %s\n", (int)view, status,
		    error.message);
	CHECK(status == FSIMG_OK && report.complete);
	fsimg_reader_free(reader);
	fsimg_io_free(input);
}

/* The checker's Joliet tree mirrors the primary one: as many files and
 * directories, the files with the same sizes and contents in the same
 * order of data.
 */
static void
check_joliet_mirror(const chk_t *c, const spec_t *specs, size_t count)
{
	size_t files = 0, directories = 0;

	for (size_t i = 0; i < count; i++) {
		if (specs[i].kind == FSIMG_ENTRY_FILE)
			files++;
		if (specs[i].kind == FSIMG_ENTRY_DIRECTORY)
			directories++;
	}
	CHECK(c->count[1] == files + directories);
	CHECK(c->dir_count[1] == directories + 1);
	for (size_t i = 0; i < c->count[1]; i++) {
		const chk_entry_t *j = &c->entries[1][i];
		bool found = j->directory;

		for (size_t k = 0; !found && k < c->count[0]; k++) {
			const chk_entry_t *p = &c->entries[0][k];

			found = !p->directory && p->size == j->size &&
			    p->extent == j->extent && p->hash == j->hash &&
			    (p->mode & S_TYPE) == S_FILE;
		}
		CHECK(found);
	}
}

/* Create an image of specs and check it with the checker, the reader in
 * every view and the verifier; the checker's state stays in c.
 */
static void
build_checked(const fsimg_write_options_t *options, const spec_t *specs,
    size_t count, bool reverse, memory_sink_t *sink, chk_t *c)
{
	fixture_t fixture;
	chk_expect_t expect;
	static const fsimg_iso_view_t views[] = { FSIMG_ISO_VIEW_AUTO,
	    FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_ISO_VIEW_JOLIET,
	    FSIMG_ISO_VIEW_PRIMARY };

	fixture_open(&fixture, options);
	add_specs(&fixture, specs, count, reverse);
	CHECK(emit_memory(&fixture, sink, NULL) == FSIMG_OK);
	bzero(&expect, sizeof(expect));
	expect.label = options->label;
	expect.level = options->iso_level;
	chk_image(c, sink->bytes, sink->length, &expect);
	CHECK(c->errors == 0);
	CHECK(c->count[0] == count);
	check_joliet_mirror(c, specs, count);
	for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++)
		check_view(fixture.ctx, sink, views[i], c, specs, count);
	fixture_close(&fixture);
}

/* The plan status of specs under options, with no emission. */
static fsimg_status_t
plan_status(const fsimg_write_options_t *options, const spec_t *specs,
    size_t count, fsimg_error_t *error)
{
	fixture_t fixture;
	const fsimg_plan_t *plan;
	fsimg_status_t status;

	status = fixture_open_with(&fixture, options, 1, true);
	for (size_t i = 0; status == FSIMG_OK && i < count; i++)
		status = add_one(&fixture, &specs[i]);
	if (status == FSIMG_OK)
		status = fsimg_writer_plan(fixture.writer, &plan, error);
	fixture_close(&fixture);
	return (status);
}

/*
 * Trees.
 */

#define TEN	"0123456789"
#define FIFTY	TEN TEN TEN TEN TEN
#define N255	FIFTY FIFTY FIFTY FIFTY FIFTY "abcde"
#define C255	FIFTY FIFTY FIFTY FIFTY FIFTY "abcde"
/* 1023 bytes, the writer's longest, with three components of 255 bytes,
 * longer than one SL component record.
 */
#define T1023	"/usr/" C255 "/" C255 "/" C255 "/" FIFTY FIFTY FIFTY FIFTY \
	FIFTY
#define S64	"////////////////////////////////////////////////////////////////"
#define S1024	S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64
#define S1023	S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 \
	"///////////////////////////////////////////////////////////////"

static const fsimg_metadata_t owned_metadata = {
	.valid = FSIMG_META_OWNER | FSIMG_META_MODE | FSIMG_META_MODIFIED |
	    FSIMG_META_ACCESSED | FSIMG_META_CHANGED,
	.uid = 1000,
	.gid = 4000000000u,
	.mode = 04750,
	.modified = { .seconds = 1234567890, .precision_ns = 1000000000,
	    .basis = FSIMG_TIME_UTC },
	.accessed = { .seconds = -2208988800, .precision_ns = 1000000000,
	    .basis = FSIMG_TIME_UTC },
	.changed = { .seconds = 5869583999, .precision_ns = 1000000000,
	    .basis = FSIMG_TIME_UTC }
};

/* Every kind; natural names, names differing only in case, names a
 * reserved alias number must skip, names outside the d-characters, over
 * the level's limits, outside Joliet's characters and outside the BMP; a
 * root directory naturally named RR_MOVED; a tree eight levels deep;
 * empty, sub-block and multi-block files; links of every component
 * shape, a 255-byte name and 1023-byte targets that continue; owner,
 * mode and times off the defaults.
 */
static const spec_t tree[] = {
	{ "/hello.txt", FSIMG_ENTRY_FILE, 33, 1, NULL, NULL },
	{ "/README", FSIMG_ENTRY_FILE, 0, 0, NULL, NULL },
	{ "/A.TXT", FSIMG_ENTRY_FILE, 10, 2, NULL, NULL },
	{ "/a.txt", FSIMG_ENTRY_FILE, 11, 3, NULL, NULL },
	{ "/dir", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/dir/sub", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/dir/sub/deep.bin", FSIMG_ENTRY_FILE, 5000, 4, NULL, NULL },
	{ "/dir/X_0000001", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/dir/Hello World.txt", FSIMG_ENTRY_FILE, 7, 5, NULL, NULL },
	{ "/dir/hello world.txt", FSIMG_ENTRY_FILE, 8, 12, NULL, NULL },
	{ "/caf\xc3\xa9.txt", FSIMG_ENTRY_FILE, 2, 6, NULL, NULL },
	{ "/a*b", FSIMG_ENTRY_FILE, 3, 7, NULL, NULL },
	{ "/a-very-long-file-name-beyond-thirty-characters.data",
	    FSIMG_ENTRY_FILE, 100, 8, NULL, NULL },
	{ "/smile\xf0\x9f\x98\x80", FSIMG_ENTRY_FILE, 1, 9, NULL, NULL },
	{ "/owned", FSIMG_ENTRY_FILE, 1, 10, &owned_metadata, NULL },
	{ "/fifo", FSIMG_ENTRY_FIFO, 0, 0, &owned_metadata, NULL },
	{ "/rr_moved", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/big", FSIMG_ENTRY_FILE, 70001, 11, NULL, NULL },
	{ "/deep", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/deep/a", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/deep/a/b", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/deep/a/b/c", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/deep/a/b/c/d", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/deep/a/b/c/d/e", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/deep/a/b/c/d/e/f", FSIMG_ENTRY_DIRECTORY, 0, 0, &owned_metadata,
	    NULL },
	{ "/deep/a/b/c/d/e/f/leaf", FSIMG_ENTRY_FILE, 3000, 13, NULL, NULL },
	{ "/" N255, FSIMG_ENTRY_FILE, 5, 14, NULL, NULL },
	{ "/link", FSIMG_ENTRY_SYMLINK, 0, 0, NULL, "hello.txt" },
	{ "/absolute", FSIMG_ENTRY_SYMLINK, 0, 0, &owned_metadata,
	    "/usr/bin/env" },
	{ "/odd", FSIMG_ENTRY_SYMLINK, 0, 0, NULL, "a//b/./../c/" },
	{ "/root", FSIMG_ENTRY_SYMLINK, 0, 0, NULL, "/" },
	{ "/dots", FSIMG_ENTRY_SYMLINK, 0, 0, NULL, "../.." },
	{ "/long-target", FSIMG_ENTRY_SYMLINK, 0, 0, NULL, T1023 },
	{ "/slashes", FSIMG_ENTRY_SYMLINK, 0, 0, NULL, S1023 },
	{ "/dir/" N255, FSIMG_ENTRY_SYMLINK, 0, 0, NULL, S1023 }
};
#define TREE_COUNT	(sizeof(tree) / sizeof(tree[0]))

/* A small relocated tree for fault sweeps and independent consumers. */
static const spec_t relocation_tree[] = {
	{ "/a", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/a/b", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/a/b/c", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/a/b/c/d", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/a/b/c/d/e", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/a/b/c/d/e/f", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/a/b/c/d/e/f/g", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/a/b/c/d/e/f/g/h", FSIMG_ENTRY_DIRECTORY, 0, 0, &owned_metadata, NULL },
	{ "/a/b/c/d/e/f/g/h/leaf", FSIMG_ENTRY_FILE, 5001, 7, NULL, NULL },
	{ "/a/b/c/d/e/f/g/i", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/a/b/c/d/e/f/g/i/leaf", FSIMG_ENTRY_FILE, 33, 8, NULL, NULL },
	{ "/RR_MOVED", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/big", FSIMG_ENTRY_FILE, 70001, 11, NULL, NULL }
};
#define RELOCATION_COUNT (sizeof(relocation_tree) / sizeof(relocation_tree[0]))

/* Case-insensitive consumers must distinguish every derived name. */
static const spec_t case_tree[] = {
	{ "/A", FSIMG_ENTRY_FILE, 1, 1, NULL, NULL },
	{ "/a", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/B", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/b", FSIMG_ENTRY_FILE, 2, 2, NULL, NULL },
	{ "/C", FSIMG_ENTRY_FILE, 3, 3, NULL, NULL },
	{ "/c", FSIMG_ENTRY_FILE, 4, 4, NULL, NULL },
	{ "/D", FSIMG_ENTRY_FILE, 5, 5, NULL, NULL },
	{ "/d", FSIMG_ENTRY_FILE, 6, 6, NULL, NULL },
	{ "/E", FSIMG_ENTRY_FILE, 7, 7, NULL, NULL },
	{ "/e", FSIMG_ENTRY_FILE, 8, 8, NULL, NULL },
	{ "/F", FSIMG_ENTRY_FILE, 9, 9, NULL, NULL },
	{ "/f", FSIMG_ENTRY_FILE, 10, 10, NULL, NULL },
	{ "/G", FSIMG_ENTRY_FILE, 11, 11, NULL, NULL },
	{ "/g", FSIMG_ENTRY_FILE, 12, 12, NULL, NULL },
	{ "/H", FSIMG_ENTRY_FILE, 13, 13, NULL, NULL },
	{ "/h", FSIMG_ENTRY_FILE, 14, 14, NULL, NULL },
	{ "/I", FSIMG_ENTRY_FILE, 15, 15, NULL, NULL },
	{ "/i", FSIMG_ENTRY_FILE, 16, 16, NULL, NULL },
	{ "/X", FSIMG_ENTRY_FILE, 17, 17, NULL, NULL },
	{ "/x", FSIMG_ENTRY_FILE, 18, 18, NULL, NULL },
	{ "/x_000000a", FSIMG_ENTRY_FILE, 19, 19, NULL, NULL },
	{ "/\xc3\x89", FSIMG_ENTRY_FILE, 20, 20, NULL, NULL },
	{ "/\xc3\xa9", FSIMG_ENTRY_FILE, 21, 21, NULL, NULL },
	{ "/ztrail", FSIMG_ENTRY_FILE, 22, 22, NULL, NULL },
	{ "/ztrail.", FSIMG_ENTRY_FILE, 23, 23, NULL, NULL },
	{ "/zzdir.", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
	{ "/zzmany..", FSIMG_ENTRY_FILE, 24, 24, NULL, NULL }
};
#define CASE_COUNT (sizeof(case_tree) / sizeof(case_tree[0]))

/*
 * Tests.
 */

/* The tree at every level passes the checker, the reader in every view
 * and the verifier. At level 3 the image is byte-identical across catalog
 * order, metadata cache size, scratch placement and time zone.
 */
static void
test_tree(void)
{
	fsimg_write_options_t options;
	memory_sink_t sink, reference;
	fixture_t fixture;
	chk_t c;

	for (unsigned level = 1; level <= 3; level++) {
		reproducible_options(&options);
		options.iso_level = level;
		options.label = "Test Volume-1";
		build_checked(&options, tree, TREE_COUNT, false, &sink, &c);
		chk_free(&c);
		free(sink.bytes);
	}

	reproducible_options(&options);
	build_checked(&options, tree, TREE_COUNT, false, &reference, &c);
	chk_free(&c);
	build_checked(&options, tree, TREE_COUNT, true, &sink, &c);
	chk_free(&c);
	CHECK(sink.length == reference.length &&
	    bcmp(sink.bytes, reference.bytes, (size_t)sink.length) == 0);
	free(sink.bytes);

	setenv("TZ", "Pacific/Chatham", 1);
	tzset();
	CHECK(fixture_open_with(&fixture, &options, 1024, false) == FSIMG_OK);
	add_specs(&fixture, tree, TREE_COUNT, true);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	unsetenv("TZ");
	tzset();
	CHECK(sink.length == reference.length &&
	    bcmp(sink.bytes, reference.bytes, (size_t)sink.length) == 0);
	free(sink.bytes);
	free(reference.bytes);
}

/* Relocation permits both deep trees and paths that exceed the primary
 * hierarchy's physical path bound.
 */
static void
test_relocation_planning(void)
{
	static const spec_t nine[] = {
		{ "/1", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		{ "/1/2", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		{ "/1/2/3", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		{ "/1/2/3/4", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		{ "/1/2/3/4/5", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		{ "/1/2/3/4/5/6", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		{ "/1/2/3/4/5/6/7", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		{ "/1/2/3/4/5/6/7/8", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL }
	};
#define D31	"ABCDEFGHIJKLMNOPQRSTUVWXYZ01234"
#define D30	"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123"
	/* Paths of 31, 63, 95, 127, 159, 191 and 223 bytes: the last
	 * leaves less than 255 - 1 - 33 = 221 at level 3.
	 */
	static const spec_t wide[] = {
		{ "/" D31, FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		{ "/" D31 "/" D31, FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		{ "/" D31 "/" D31 "/" D31, FSIMG_ENTRY_DIRECTORY, 0, 0, NULL,
		    NULL },
		{ "/" D31 "/" D31 "/" D31 "/" D31, FSIMG_ENTRY_DIRECTORY, 0, 0,
		    NULL, NULL },
		{ "/" D31 "/" D31 "/" D31 "/" D31 "/" D31,
		    FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		{ "/" D31 "/" D31 "/" D31 "/" D31 "/" D31 "/" D31,
		    FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		{ "/" D31 "/" D31 "/" D31 "/" D31 "/" D31 "/" D31 "/" D31,
		    FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		{ "/" D31 "/" D31 "/" D31 "/" D31 "/" D31 "/" D31 "/" D31 "/" D30,
		    FSIMG_ENTRY_FILE, 1, 20, NULL, NULL }
	};
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	chk_expect_t expect = { 0 };
	chk_t c;
	const fsimg_plan_t *base;
	iso_row_t moved;

	reproducible_options(&options);
	CHECK(plan_status(&options, nine, 7, NULL) == FSIMG_OK);
	CHECK(plan_status(&options, nine, 8, NULL) == FSIMG_OK);
	CHECK(plan_status(&options, wide, 6, NULL) == FSIMG_OK);
	CHECK(plan_status(&options, wide, 7, NULL) == FSIMG_OK);
	/* Seven 31-byte directories plus a 33-byte file identifier would
	 * exceed the physical bound without path-length relocation.
	 */
	fixture_open(&fixture, &options);
	add_specs(&fixture, wide, sizeof(wide) / sizeof(wide[0]), false);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	chk_image(&c, sink.bytes, sink.length, &expect);
	CHECK(c.errors == 0);
	CHECK(fsimg_writer_plan(fixture.writer, &base, NULL) == FSIMG_OK);
	CHECK(iso_row_get((iso_plan_t *)base, 7, &moved, NULL) == FSIMG_OK);
	CHECK(moved.relocation_alias != 0);
	chk_free(&c);
	free(sink.bytes);
	fixture_close(&fixture);
	/* At level 1 the same names are aliases of 8 characters. */
	options.iso_level = 1;
	CHECK(plan_status(&options, wide, 7, NULL) == FSIMG_OK);
#undef D31
#undef D30
}

/* Deep logical paths survive relocation in Rock Ridge and Joliet. A
 * 117-byte final component with all TF times makes the CL record continue
 * while the physical RE record fits, exercising their separate plans.
 */
static void
test_relocation_roundtrip(void)
{
	static const unsigned depths[] = { 7, 8, 9, 16, 256 };
	static const fsimg_iso_view_t views[] = { FSIMG_ISO_VIEW_ROCKRIDGE,
	    FSIMG_ISO_VIEW_JOLIET, FSIMG_ISO_VIEW_PRIMARY };
	fsimg_write_options_t options;
	fsimg_read_options_t read_options;
	fsimg_verify_report_t report;
	const fsimg_entry_t *entry;
	const fsimg_plan_t *base;
	fsimg_reader_t *reader;
	fsimg_io_t *io;
	iso_row_t row;
	chk_expect_t expect = { 0 };
	chk_t check;
	fixture_t fixture;
	memory_sink_t sink;
	char (*paths)[1024];
	spec_t *specs;
	unsigned char bytes[5001];
	size_t length;
	unsigned depth, directories, files, count;
	bool relocated;
	fsimg_status_t status;

	for (unsigned d = 0; d < sizeof(depths) / sizeof(depths[0]); d++) {
		depth = depths[d];
		/* The depth limit counts every path component. Keep the
		 * deepest directory empty when it reaches that limit.
		 */
		count = depth + (depth < 256 ? 1 : 0);
		paths = calloc(depth + 1, sizeof(*paths));
		specs = calloc(depth + 1, sizeof(*specs));
		CHECK(paths != NULL && specs != NULL);
		for (unsigned i = 0; i < depth; i++) {
			if (i != 0)
				strcpy(paths[i], paths[i - 1]);
			strcat(paths[i], "/");
			length = strlen(paths[i]);
			memset(paths[i] + length, 'd', i + 1 == depth ? 117 : 1);
			specs[i] = (spec_t){ paths[i], FSIMG_ENTRY_DIRECTORY,
			    0, 0, i + 1 == depth ? &owned_metadata : NULL, NULL };
		}
		snprintf(paths[depth], sizeof(paths[depth]), "%s/leaf", paths[depth - 1]);
		specs[depth] = (spec_t){ paths[depth], FSIMG_ENTRY_FILE,
		    sizeof(bytes), 7, NULL, NULL };
		reproducible_options(&options);
		fixture_open(&fixture, &options);
		add_specs(&fixture, specs, count, false);
		CHECK(fsimg_writer_plan(fixture.writer, &base, NULL) == FSIMG_OK);
		relocated = ((const iso_plan_t *)base)->relocation_id != 0;
		CHECK(relocated == (depth >= 8));
		if (depth == 8) {
			CHECK(iso_row_get((iso_plan_t *)base, depth, &row, NULL) == FSIMG_OK);
			CHECK(row.child.fragments == 0 && row.placeholder.fragments != 0);
		}
		CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
		chk_image(&check, sink.bytes, sink.length, &expect);
		CHECK(check.errors == 0);
		chk_free(&check);
		CHECK(memory_io(fixture.ctx, &sink, false, &io) == FSIMG_OK);
		for (unsigned v = 0; v < 3; v++) {
			fsimg_read_options_init(&read_options);
			read_options.iso_view = views[v];
			CHECK(fsimg_reader_open(fixture.ctx, io, &read_options,
			    &reader, NULL) == FSIMG_OK);
			directories = files = 0;
			while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
				if (entry->kind == FSIMG_ENTRY_DIRECTORY) {
					directories++;
					continue;
				}
				files++;
				if (entry->size == 0)
					continue; /* primary CL placeholder */
				CHECK(entry->size == sizeof(bytes));
				if (views[v] == FSIMG_ISO_VIEW_ROCKRIDGE)
					CHECK(strcmp(entry->path, paths[depth]) == 0);
				CHECK(fsimg_reader_read_at(reader, 0, bytes, sizeof(bytes),
				    &length, NULL) == FSIMG_OK && length == sizeof(bytes));
				for (unsigned i = 0; i < sizeof(bytes); i++)
					CHECK(bytes[i] == content_byte(7, i));
			}
			CHECK(status == FSIMG_DONE);
			if (views[v] != FSIMG_ISO_VIEW_PRIMARY) {
				CHECK(files == count - depth);
				CHECK(directories == depth +
				    (views[v] == FSIMG_ISO_VIEW_ROCKRIDGE && relocated ? 1 : 0));
			}
			fsimg_reader_free(reader);
			CHECK(fsimg_reader_open(fixture.ctx, io, &read_options,
			    &reader, NULL) == FSIMG_OK);
			CHECK(fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report,
			    NULL) == FSIMG_OK && report.complete);
			fsimg_reader_free(reader);
		}
		fsimg_io_free(io);
		free(sink.bytes);
		fixture_close(&fixture);
		free(specs);
		free(paths);
	}
}

/* Relocation is reproducible across source order, cache and scratch
 * choices. An empty source container is adopted; a conflicting one fails
 * only when the physical hierarchy actually needs relocation.
 */
static void
test_relocation_container(void)
{
	spec_t container = { "/.rr_moved", FSIMG_ENTRY_DIRECTORY,
	    0, 0, &owned_metadata, NULL };
	spec_t child = { "/.rr_moved/child", FSIMG_ENTRY_FILE, 1, 1, NULL, NULL };
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t reference, sink;
	const fsimg_plan_t *base;
	iso_row_t row;
	chk_expect_t expect = { 0 };
	chk_t check;

	reproducible_options(&options);
	fixture_open(&fixture, &options);
	add_specs(&fixture, relocation_tree, RELOCATION_COUNT, false);
	CHECK(emit_memory(&fixture, &reference, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	CHECK(fixture_open_with(&fixture, &options, 1024, false) == FSIMG_OK);
	add_specs(&fixture, relocation_tree, RELOCATION_COUNT, true);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	CHECK(sink.length == reference.length &&
	    bcmp(sink.bytes, reference.bytes, (size_t)sink.length) == 0);
	free(sink.bytes);
	free(reference.bytes);
	fixture_close(&fixture);

	fixture_open(&fixture, &options);
	add_specs(&fixture, relocation_tree, RELOCATION_COUNT, false);
	CHECK(add_one(&fixture, &container) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &base, NULL) == FSIMG_OK);
	CHECK(((const iso_plan_t *)base)->relocation_entry.id == 0);
	CHECK(iso_row_get((iso_plan_t *)base,
	    ((const iso_plan_t *)base)->relocation_id, &row, NULL) == FSIMG_OK);
	CHECK(row.uid == owned_metadata.uid && row.gid == owned_metadata.gid);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	chk_image(&check, sink.bytes, sink.length, &expect);
	CHECK(check.errors == 0);
	CHECK(chk_find(&check, 0, "/.rr_moved") != NULL);
	CHECK(chk_find(&check, 1, "/.rr_moved") == NULL);
	chk_free(&check);
	free(sink.bytes);
	fixture_close(&fixture);

	fixture_open(&fixture, &options);
	add_specs(&fixture, relocation_tree, RELOCATION_COUNT, false);
	CHECK(add_one(&fixture, &container) == FSIMG_OK);
	CHECK(add_one(&fixture, &child) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &base, NULL) == FSIMG_EUNSUPPORTED);
	fixture_close(&fixture);
	container.kind = FSIMG_ENTRY_FILE;
	CHECK(plan_status(&options, &container, 1, NULL) == FSIMG_OK);
	fixture_open(&fixture, &options);
	add_specs(&fixture, relocation_tree, RELOCATION_COUNT, false);
	CHECK(add_one(&fixture, &container) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &base, NULL) == FSIMG_EUNSUPPORTED);
	fixture_close(&fixture);
}

/* Extracting the logical tree leaves an empty relocation container. A
 * writer adopts it when the extracted tree is scanned and created again.
 * Safe extraction intentionally does not restore privileged metadata.
 */
static void
test_relocation_extract_create(void)
{
	fsimg_write_options_t options;
	fsimg_read_options_t read_options;
	fsimg_extract_options_t extract_options;
	fsimg_reader_t *reader;
	fsimg_io_t *io;
	const fsimg_entry_t *entry;
	const spec_t *spec;
	fixture_t original, recreated;
	memory_sink_t source, result;
	unsigned char bytes[1024];
	uint64_t offset;
	size_t length, found = 0;
	int extracted;
	fsimg_status_t status;

	reproducible_options(&options);
	options.flags |= FSIMG_WRITE_ALLOW_LOSS;
	fixture_open(&original, &options);
	add_specs(&original, relocation_tree, RELOCATION_COUNT, false);
	CHECK(emit_memory(&original, &source, NULL) == FSIMG_OK);
	CHECK(memory_io(original.ctx, &source, false, &io) == FSIMG_OK);
	fsimg_read_options_init(&read_options);
	CHECK(fsimg_reader_open(original.ctx, io, &read_options, &reader, NULL) == FSIMG_OK);
	fsimg_extract_options_init(&extract_options);
	extract_options.flags |= FSIMG_EXTRACT_ALLOW_LOSS;
	CHECK(fsimg_extract_at(reader, original.parent, "extracted", &extract_options,
	    NULL) == FSIMG_OK);
	fsimg_reader_free(reader);
	fsimg_io_free(io);
	free(source.bytes);
	extracted = openat(original.parent, "extracted",
	    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(extracted != -1);
	fixture_open(&recreated, &options);
	CHECK(fsimg_writer_add_tree(recreated.writer, extracted, "/", NULL) == FSIMG_OK);
	CHECK(emit_memory(&recreated, &result, NULL) == FSIMG_OK);
	CHECK(memory_io(recreated.ctx, &result, false, &io) == FSIMG_OK);
	CHECK(fsimg_reader_open(recreated.ctx, io, &read_options, &reader, NULL) == FSIMG_OK);
	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		if (entry->kind != FSIMG_ENTRY_FILE)
			continue;
		spec = NULL;
		for (size_t i = 0; i < RELOCATION_COUNT; i++)
			if (strcmp(entry->path, relocation_tree[i].path) == 0)
				spec = &relocation_tree[i];
		CHECK(spec != NULL && entry->size == spec->size);
		for (offset = 0; offset < entry->size; offset += length) {
			CHECK(fsimg_reader_read_at(reader, offset, bytes, sizeof(bytes),
			    &length, NULL) == FSIMG_OK && length != 0);
			for (size_t i = 0; i < length; i++)
				CHECK(bytes[i] == content_byte(spec->source, offset + i));
		}
		found++;
	}
	CHECK(status == FSIMG_DONE && found == 3);
	fsimg_reader_free(reader);
	fsimg_io_free(io);
	free(result.bytes);
	fixture_close(&recreated);
	/* These fixed fixture paths contain only files and directories.
	 * Remove children before parents beneath the private extracted root.
	 */
	for (size_t i = RELOCATION_COUNT; i > 0; i--)
		CHECK(unlinkat(extracted, relocation_tree[i - 1].path + 1,
		    relocation_tree[i - 1].kind == FSIMG_ENTRY_DIRECTORY ? AT_REMOVEDIR : 0) == 0);
	CHECK(unlinkat(extracted, ".rr_moved", AT_REMOVEDIR) == 0);
	CHECK(close(extracted) == 0);
	CHECK(unlinkat(original.parent, "extracted", AT_REMOVEDIR) == 0);
	fixture_close(&original);
}

/* The identifier of the entry at path in hierarchy h, as text: primary
 * bytes, or Joliet decoded to UTF-8.
 */
static void
check_identifier(const chk_t *c, unsigned h, const char *path,
    const char *expected)
{
	const chk_entry_t *e = chk_find(c, h, path);
	char text[NAME_BYTES];

	CHECK(e != NULL);
	if (h == 0) {
		bcopy(e->identifier, text, e->identifier_length);
		text[e->identifier_length] = '\0';
	} else {
		joliet_utf8(e->identifier, e->identifier_length, text);
	}
	if (strcmp(text, expected) != 0)
		fprintf(stderr, "%s: %s, expected %s\n", path, text, expected);
	CHECK(strcmp(text, expected) == 0);
}

/* Independent key for this ASCII/Latin-1 fixture, without the writer's
 * Unicode mapping or name functions. Version suffixes do not distinguish
 * file and directory lookups in case-insensitive consumers.
 */
static bool
case_identifier_equal(const chk_entry_t *a, const chk_entry_t *b)
{
	size_t an = a->identifier_length / 2 - (a->directory ? 0 : 2);
	size_t bn = b->identifier_length / 2 - (b->directory ? 0 : 2);
	uint16_t ac, bc;

	if (an != bn)
		return (false);
	for (size_t i = 0; i < an; i++) {
		ac = be16(a->identifier + 2 * i);
		bc = be16(b->identifier + 2 * i);
		if ((ac >= 'a' && ac <= 'z') ||
		    (ac >= 0xe0 && ac <= 0xfe && ac != 0xf7))
			ac -= 0x20;
		if ((bc >= 'a' && bc <= 'z') ||
		    (bc >= 0xe0 && bc <= 0xfe && bc != 0xf7))
			bc -= 0x20;
		if (ac != bc)
			return (false);
	}
	return (true);
}

/* Native primary lookups remove the version and an empty extension's
 * period. File and directory keys must stay distinct under that view.
 * This decoder uses only bytes from the emitted records.
 */
static size_t
primary_lookup_length(const chk_entry_t *entry)
{
	size_t length = entry->identifier_length;

	if (!entry->directory) {
		CHECK(length >= 3 && entry->identifier[length - 2] == ';' &&
		    entry->identifier[length - 1] == '1');
		length -= 2;
		if (entry->identifier[length - 1] == '.')
			length--;
	}
	return (length);
}

static void
test_primary_relocation_alias(void)
{
	const spec_t file = { "/rr_moved", FSIMG_ENTRY_FILE, 1, 1, NULL, NULL };
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	chk_expect_t expect = { 0 };
	chk_t c;
	const chk_entry_t *entry;

	reproducible_options(&options);
	fixture_open(&fixture, &options);
	add_specs(&fixture, relocation_tree, RELOCATION_COUNT, false);
	CHECK(add_one(&fixture, &file) == FSIMG_OK);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	chk_image(&c, sink.bytes, sink.length, &expect);
	CHECK(c.errors == 0);
	entry = chk_find(&c, 0, file.path);
	CHECK(entry != NULL);
	CHECK(primary_lookup_length(entry) != 8 ||
	    bcmp(entry->identifier, "RR_MOVED", 8) != 0);
	chk_free(&c);
	free(sink.bytes);
	fixture_close(&fixture);
}

static void
test_primary_lookup_aliases(void)
{
	fsimg_write_options_t options;
	memory_sink_t sink;
	chk_t c;

	for (unsigned level = 1; level <= 3; level++) {
		reproducible_options(&options);
		options.iso_level = level;
		build_checked(&options, case_tree, CASE_COUNT, false, &sink, &c);
		for (size_t i = 0; i < c.count[0]; i++) {
			size_t length = primary_lookup_length(&c.entries[0][i]);

			for (size_t j = i + 1; j < c.count[0]; j++)
				CHECK(length != primary_lookup_length(&c.entries[0][j]) ||
				    bcmp(c.entries[0][i].identifier,
				    c.entries[0][j].identifier, length) != 0);
		}
		chk_free(&c);
		free(sink.bytes);
	}
}

static void
test_joliet_case_aliases(void)
{
	fsimg_write_options_t options;
	memory_sink_t sink;
	chk_t c;

	reproducible_options(&options);
	build_checked(&options, case_tree, CASE_COUNT, false, &sink, &c);
	for (size_t i = 0; i < c.count[1]; i++) {
		size_t units = c.entries[1][i].identifier_length / 2 -
		    (c.entries[1][i].directory ? 0 : 2);

		CHECK(units != 0 && be16(c.entries[1][i].identifier + 2 * (units - 1)) != '.');
		for (size_t j = i + 1; j < c.count[1]; j++)
			CHECK(!case_identifier_equal(&c.entries[1][i], &c.entries[1][j]));
	}
	check_identifier(&c, 1, "/x_000000B", "x_000000B;1");
	check_identifier(&c, 1, "/x_000000a", "x_000000a;1");
	chk_free(&c);
	free(sink.bytes);
}

static void
test_joliet_alias_family(unsigned fixture)
{
	static const spec_t cases[][2] = {
		{ { "/x..", FSIMG_ENTRY_FILE, 1, 1, NULL, NULL },
		  { "/x._0000001", FSIMG_ENTRY_FILE, 2, 2, NULL, NULL } },
		{ { "/x.", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		  { "/x._0000001", FSIMG_ENTRY_FILE, 2, 2, NULL, NULL } },
		{ { "/a?.txt", FSIMG_ENTRY_FILE, 1, 1, NULL, NULL },
		  { "/a__0000001.txt", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL } }
	};
	fsimg_write_options_t options;
	memory_sink_t sink;
	chk_t c;

	CHECK(fixture < sizeof(cases) / sizeof(cases[0]));
	reproducible_options(&options);
	build_checked(&options, cases[fixture], 2, false, &sink, &c);
	for (size_t i = 0; i < c.count[1]; i++)
		for (size_t j = i + 1; j < c.count[1]; j++)
			CHECK(!case_identifier_equal(&c.entries[1][i], &c.entries[1][j]));
	chk_free(&c);
	free(sink.bytes);
}

/* The tree's identifiers at level 3 and level 1: natural ones uppercased,
 * the first of two names differing in case keeping its natural one,
 * aliases numbered in catalog order and skipping the number /dir/X_0000001
 * reserves, extensions cut to 5 (3 at level 1), names cut to fit, every
 * byte outside the d-characters replaced, and a root RR_MOVED aliased; in
 * Joliet, names kept exactly unless a character or the length forbids.
 */
static void
test_identifiers(void)
{
	fsimg_write_options_t options;
	memory_sink_t sink;
	chk_t c;

	reproducible_options(&options);
	build_checked(&options, tree, TREE_COUNT, false, &sink, &c);
	check_identifier(&c, 0, "/A.TXT", "A.TXT;1");
	check_identifier(&c, 0, "/a.txt", "A_0000004.TXT;1");
	check_identifier(&c, 0, "/README", "README.;1");
	check_identifier(&c, 0, "/a*b", "A_B_0000002.;1");
	check_identifier(&c, 0,
	    "/a-very-long-file-name-beyond-thirty-characters.data",
	    "A_VERY_LONG_FILE_N_0000003.DATA;1");
	check_identifier(&c, 0, "/caf\xc3\xa9.txt", "CAF___0000005.TXT;1");
	check_identifier(&c, 0, "/rr_moved", "RR_MOVED_0000007");
	check_identifier(&c, 0, "/smile\xf0\x9f\x98\x80",
	    "SMILE_____0000008.;1");
	check_identifier(&c, 0, "/" N255, "0123456789012345678901_0000001.;1");
	check_identifier(&c, 0, "/dir/X_0000001", "X_0000001");
	check_identifier(&c, 0, "/dir/Hello World.txt",
	    "HELLO_WORLD_0000003.TXT;1");
	check_identifier(&c, 0, "/dir/hello world.txt",
	    "HELLO_WORLD_0000004.TXT;1");
	check_identifier(&c, 0, "/deep/a/b/c/d/e/f", "F");
	check_identifier(&c, 1, "/a_0000003.txt", "a_0000003.txt;1");
	check_identifier(&c, 1, "/a_b_0000002", "a_b_0000002;1");
	check_identifier(&c, 1, "/smile__0000004", "smile__0000004;1");
	check_identifier(&c, 1, "/caf\xc3\xa9.txt", "caf\xc3\xa9.txt;1");
	check_identifier(&c, 1, "/dir/Hello World.txt", "Hello World.txt;1");
	check_identifier(&c, 1, "/dir/hello world_0000002.txt",
	    "hello world_0000002.txt;1");
	check_identifier(&c, 1, "/rr_moved", "rr_moved");
	check_identifier(&c, 1,
	    "/012345678901234567890123456789012345678901234567890123_0000001",
	    "012345678901234567890123456789012345678901234567890123_0000001;1");
	chk_free(&c);
	free(sink.bytes);

	options.iso_level = 1;
	build_checked(&options, tree, TREE_COUNT, false, &sink, &c);
	check_identifier(&c, 0, "/A.TXT", "A.TXT;1");
	check_identifier(&c, 0, "/a.txt", "_0000004.TXT;1");
	check_identifier(&c, 0, "/hello.txt", "HELLO.TXT;1");
	check_identifier(&c, 0,
	    "/a-very-long-file-name-beyond-thirty-characters.data",
	    "_0000003.DAT;1");
	check_identifier(&c, 0, "/dir/X_0000001", "_0000003");
	check_identifier(&c, 0, "/dir/sub/deep.bin", "DEEP.BIN;1");
	chk_free(&c);
	free(sink.bytes);
}

/* Records of every length near a block's end and Rock Ridge streams near
 * every split boundary: names of 1 to 255 bytes in one directory, so
 * records end and NM entries continue at every position, and the
 * continuation region spans several blocks.
 */
static void
test_record_boundaries(void)
{
	static char names[255][300];
	spec_t specs[256];
	fsimg_write_options_t options;
	memory_sink_t sink;
	chk_t c;

	specs[0] = (spec_t){ "/d", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL };
	for (unsigned i = 0; i < 255; i++) {
		snprintf(names[i], sizeof(names[i]), "/d/");
		memset(names[i] + 3, 'a' + i % 26, i + 1);
		names[i][3 + i + 1] = '\0';
		specs[i + 1] = (spec_t){ names[i], i % 3 == 0 ?
		    FSIMG_ENTRY_SYMLINK : FSIMG_ENTRY_FILE, i, i + 1, NULL,
		    i % 3 == 0 ? names[254 - i] : NULL };
	}
	/* Names sharing a letter at different lengths are distinct. */
	reproducible_options(&options);
	build_checked(&options, specs, 256, false, &sink, &c);
	chk_free(&c);
	free(sink.bytes);
}

/* A sparse sink: a volume of length bytes whose first SPARSE_BYTES are
 * stored; every byte past them must be zero, as the large-file image's
 * data is, and reads there return zeros.
 */
#define SPARSE_BYTES	(1024u * 1024u)

typedef struct sparse_sink {
	unsigned char	*bytes;
	uint64_t	 length;
} sparse_sink_t;

static fsimg_status_t
sparse_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((sparse_sink_t *)argument)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
sparse_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	sparse_sink_t *sink = argument;
	unsigned char *bytes = buffer;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	for (size_t i = 0; i < length; i++)
		bytes[i] = offset + i < SPARSE_BYTES ?
		    sink->bytes[offset + i] : 0;
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sparse_write(void *argument, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	sparse_sink_t *sink = argument;
	const unsigned char *bytes = buffer;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	for (size_t i = 0; i < length; i++) {
		if (offset + i < SPARSE_BYTES)
			sink->bytes[offset + i] = bytes[i];
		else
			CHECK(bytes[i] == 0);
	}
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sparse_zero(void *argument, uint64_t offset, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	sparse_sink_t *sink = argument;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	for (uint64_t i = offset; i < offset + length && i < SPARSE_BYTES; i++)
		sink->bytes[i] = 0;
	return (FSIMG_OK);
}

static fsimg_status_t
sparse_flush(void *argument FSIMG_UNUSED, fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

/* Flatten the independently framed SUSP chain of one sparse record.
 * Continuation areas are metadata in the stored prefix and must advance
 * across sections; sharing one would alias two metadata claims.
 */
static size_t
sparse_rr_entries(const sparse_sink_t *sink, const unsigned char *record,
    unsigned char *out, uint64_t *claimed_end)
{
	const unsigned char *area;
	size_t used, room, at, length, total = 0;
	uint64_t position, bytes;
	bool continued;

	used = 33 + record[32] + (record[32] % 2 == 0);
	CHECK(used <= record[0]);
	area = record + used;
	room = record[0] - used;
	for (unsigned fragments = 0;; fragments++) {
		CHECK(fragments <= 16);
		continued = false;
		for (at = 0; at + 4 <= room && area[at] != 0; at += length) {
			length = area[at + 2];
			CHECK(length >= 4 && length <= room - at && area[at + 3] == 1);
			if (bcmp(area + at, "CE", 2) == 0) {
				CHECK(length == 28);
				CHECK(le32(area + at + 4) == be32(area + at + 8));
				CHECK(le32(area + at + 12) == be32(area + at + 16));
				CHECK(le32(area + at + 20) == be32(area + at + 24));
				position = (uint64_t)le32(area + at + 4) * BLOCK +
				    le32(area + at + 12);
				bytes = le32(area + at + 20);
				CHECK(bytes != 0 && le32(area + at + 12) < BLOCK &&
				    bytes <= BLOCK - le32(area + at + 12));
				CHECK(position >= *claimed_end && position <= SPARSE_BYTES &&
				    bytes <= SPARSE_BYTES - position);
				*claimed_end = position + bytes;
				area = sink->bytes + position;
				room = (size_t)bytes;
				continued = true;
				break;
			}
			CHECK(length <= 4096 - total);
			bcopy(area + at, out + total, length);
			total += length;
		}
		if (!continued)
			return (total);
	}
}

/* Independently check the large file's directory-record run, without
 * materializing or hashing its all-zero multi-gigabyte payload. The sink
 * stores all metadata in its first MiB. Both endian copies, the section
 * lengths, flags, identifiers, dates and contiguous addresses must agree.
 */
static void
check_large_sections(const sparse_sink_t *sink, unsigned hierarchy)
{
	static const uint32_t lengths[] = { 0xfffff800u, 0xfffff800u, 4096 };
	const unsigned char *descriptor, *record, *first = NULL;
	uint64_t start, end, at, next_extent = 0;
	unsigned section = 0;
	uint32_t length, extent;
	unsigned char expected_rr[4096], observed_rr[4096];
	size_t rr_length = 0, observed;
	uint64_t claimed_end = 0;

	descriptor = sink->bytes + (16 + hierarchy) * BLOCK;
	start = (uint64_t)le32(descriptor + 158) * BLOCK;
	end = start + le32(descriptor + 166);
	CHECK(end <= SPARSE_BYTES);
	for (at = start; at < end;) {
		record = sink->bytes + at;
		if (record[0] == 0) {
			at += BLOCK - at % BLOCK;
			continue;
		}
		CHECK(record[0] >= 34 && at % BLOCK + record[0] <= BLOCK);
		CHECK(at + record[0] <= end);
		CHECK(33u + record[32] + (record[32] % 2 == 0) <= record[0]);
		length = le32(record + 10);
		extent = le32(record + 2);
		if (length == lengths[0] ||
		    (first != NULL && section < 3)) {
			CHECK(section < 3 && length == lengths[section]);
			CHECK(length == be32(record + 14));
			CHECK(extent == be32(record + 6));
			CHECK(record[25] == (section < 2 ? 0x80 : 0));
			if (hierarchy == 0) {
				observed = sparse_rr_entries(sink, record, observed_rr, &claimed_end);
				if (section == 0) {
					CHECK(observed != 0);
					rr_length = observed;
					bcopy(observed_rr, expected_rr, observed);
				} else {
					CHECK(observed == rr_length &&
					    bcmp(observed_rr, expected_rr, observed) == 0);
				}
			}
			if (first == NULL) {
				first = record;
				next_extent = extent;
			} else {
				CHECK(record[32] == first[32]);
				CHECK(bcmp(record + 33, first + 33, first[32]) == 0);
				CHECK(bcmp(record + 18, first + 18, 7) == 0);
				if (hierarchy != 0)
					CHECK(record[0] == 33 + record[32] +
					    (record[32] % 2 == 0));
			}
			CHECK(extent == next_extent);
			next_extent += ((uint64_t)length + BLOCK - 1) / BLOCK;
			section++;
		}
		at += record[0];
	}
	CHECK(section == 3);
}

/* A file above 4 GiB is refused at levels 1 and 2 and written at level 3
 * as sections of 0xfffff800 bytes and a final one with the rest; a file
 * of 2^32 - 1 bytes is one section at every level. The level 3 image is
 * read back in every view, the file whole, and verified.
 */
static void
test_large_file(void)
{
	static const spec_t large[] = {
		{ "/z" FIFTY FIFTY FIFTY FIFTY FIFTY "abcd", FSIMG_ENTRY_FILE,
		    UINT64_C(2) * 0xfffff800u + 4096, 0, NULL, NULL },
		{ "/a-small", FSIMG_ENTRY_FILE, 3, 1, NULL, NULL },
		{ "/exact", FSIMG_ENTRY_FILE, UINT32_MAX, 0, NULL, NULL }
	};
	static const spec_t beyond[] = {
		{ "/near-max", FSIMG_ENTRY_FILE, UINT64_C(0xffffffff) * 2048 + 1,
		    0, NULL, NULL },
		{ "/wrapping", FSIMG_ENTRY_FILE, UINT64_C(0xfffffffffffff801),
		    0, NULL, NULL }
	};
	static const fsimg_iso_view_t views[] = { FSIMG_ISO_VIEW_ROCKRIDGE,
	    FSIMG_ISO_VIEW_JOLIET, FSIMG_ISO_VIEW_PRIMARY };
	fsimg_write_options_t options;
	fsimg_read_options_t read_options;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_verify_report_t report;
	const fsimg_entry_t *entry;
	fsimg_reader_t *reader;
	fsimg_io_ops_t ops;
	fsimg_io_t *io;
	fsimg_error_t error;
	sparse_sink_t sink;
	fixture_t fixture;
	unsigned char tail[16];
	size_t length;
	unsigned found;

	reproducible_options(&options);
	/* Above the largest volume, whose section and block counts would
	 * wrap or loop, a file is refused before any is computed.
	 */
	CHECK(plan_status(&options, beyond, 1, &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "/near-max") == 0);
	CHECK(plan_status(&options, beyond + 1, 1, &error) ==
	    FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "/wrapping") == 0);
	options.iso_level = 1;
	CHECK(plan_status(&options, large, 1, &error) == FSIMG_EUNSUPPORTED);
	options.iso_level = 2;
	CHECK(plan_status(&options, large, 1, NULL) == FSIMG_EUNSUPPORTED);
	CHECK(plan_status(&options, large + 2, 1, NULL) == FSIMG_OK);

	options.iso_level = 3;
	fixture_open(&fixture, &options);
	add_specs(&fixture, large, 3, false);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	sink.length = info.volume_bytes;
	sink.bytes = malloc(SPARSE_BYTES);
	CHECK(sink.bytes != NULL);
	memset(sink.bytes, POISON, SPARSE_BYTES);
	fsimg_io_ops_init(&ops);
	ops.get_size = sparse_size;
	ops.read_at = sparse_read;
	ops.write_at = sparse_write;
	ops.zero_range = sparse_zero;
	ops.flush = sparse_flush;
	CHECK(fsimg_io_from_callbacks(fixture.ctx, &ops, &sink, &io, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_writer_emit(fixture.writer, io, NULL) == FSIMG_OK);
	check_large_sections(&sink, 0);
	check_large_sections(&sink, 1);

	for (size_t v = 0; v < sizeof(views) / sizeof(views[0]); v++) {
		fsimg_read_options_init(&read_options);
		read_options.iso_view = views[v];
		CHECK(fsimg_reader_open(fixture.ctx, io, &read_options, &reader,
		    NULL) == FSIMG_OK);
		found = 0;
		while (fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK) {
			if (entry->size != large[0].size)
				continue;
			found++;
			CHECK(fsimg_reader_read_at(reader, entry->size - 16, tail,
			    sizeof(tail), &length, NULL) == FSIMG_OK);
			CHECK(length == 16 && tail[0] == 0 && tail[15] == 0);
		}
		CHECK(found == 1);
		fsimg_reader_free(reader);
		CHECK(fsimg_reader_open(fixture.ctx, io, &read_options, &reader,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_verify(reader, 0, &report, NULL) == FSIMG_OK &&
		    report.complete && report.files == 3);
		fsimg_reader_free(reader);
	}
	fsimg_io_free(io);
	free(sink.bytes);
	fixture_close(&fixture);
}

/* Find a file extent in the primary root using only on-disk offsets. */
static uint32_t
boot_file_extent(const memory_sink_t *sink, const char *name)
{
	const unsigned char *descriptor = sink->bytes + 16 * BLOCK;
	const unsigned char *record;
	uint64_t at = (uint64_t)le32(descriptor + 158) * BLOCK;
	uint64_t end = at + le32(descriptor + 166);

	CHECK(end <= sink->length);
	while (at < end) {
		record = sink->bytes + at;
		if (record[0] == 0) {
			at += BLOCK - at % BLOCK;
			continue;
		}
		CHECK(record[0] >= 34 && at + record[0] <= end);
		CHECK(33u + record[32] <= record[0]);
		if (record[32] == strlen(name) &&
		    bcmp(record + 33, name, record[32]) == 0)
			return (le32(record + 2));
		at += record[0];
	}
	CHECK(false);
	return (0);
}

/* BIOS, EFI and combined catalogs point at ordinary files, in every
 * hierarchy. Check their wire format independently before using the reader.
 */
static void
test_boot_catalogs(void)
{
	static const spec_t files[] = {
		{ "/bios.img", FSIMG_ENTRY_FILE, 4096, 1, NULL, NULL },
		{ "/efi.img", FSIMG_ENTRY_FILE, 8193, 2, NULL, NULL }
	};
	static const fsimg_iso_view_t views[] = { FSIMG_ISO_VIEW_ROCKRIDGE,
	    FSIMG_ISO_VIEW_JOLIET, FSIMG_ISO_VIEW_PRIMARY };
	fsimg_write_options_t options;
	fsimg_read_options_t read_options;
	fsimg_verify_report_t report;
	fsimg_reader_t *reader;
	fsimg_io_t *io;
	memory_sink_t sink;
	fixture_t fixture;
	const unsigned char *record, *catalog, *entry;
	uint32_t catalog_block, sum;

	for (unsigned kinds = 1; kinds <= 3; kinds++) {
		reproducible_options(&options);
		options.boot_bios = (kinds & 1) != 0 ? "/bios.img" : NULL;
		options.boot_efi = (kinds & 2) != 0 ? "/efi.img" : NULL;
		fixture_open(&fixture, &options);
		add_specs(&fixture, files, 2, false);
		CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
		record = sink.bytes + 17 * BLOCK;
		CHECK(record[0] == 0 && bcmp(record + 1, "CD001", 5) == 0);
		CHECK(record[6] == 1 && bcmp(record + 7,
		    "EL TORITO SPECIFICATION", 23) == 0);
		catalog_block = le32(record + 71);
		CHECK(catalog_block > 19 && catalog_block < sink.length / BLOCK);
		catalog = sink.bytes + (uint64_t)catalog_block * BLOCK;
		CHECK(catalog[0] == 1 && catalog[1] == (kinds == 2 ? 0xef : 0));
		CHECK(catalog[30] == 0x55 && catalog[31] == 0xaa);
		sum = 0;
		for (unsigned i = 0; i < 32; i += 2)
			sum += le16(catalog + i);
		CHECK((sum & 0xffff) == 0);
		entry = catalog + 32;
		CHECK(entry[0] == 0x88 && entry[1] == 0);
		CHECK(le16(entry + 6) == (kinds == 2 ? 17 : 4));
		CHECK(le32(entry + 8) > catalog_block);
		CHECK(le32(entry + 8) == boot_file_extent(&sink,
		    kinds == 2 ? "EFI.IMG;1" : "BIOS.IMG;1"));
		if (kinds == 3) {
			CHECK(catalog[64] == 0x91 && catalog[65] == 0xef);
			CHECK(le16(catalog + 66) == 1);
			CHECK(catalog[96] == 0x88 && le16(catalog + 102) == 17);
			CHECK(le32(catalog + 104) > le32(entry + 8));
			CHECK(le32(catalog + 104) ==
			    boot_file_extent(&sink, "EFI.IMG;1"));
		}
		for (unsigned f = 0; f < 2; f++) {
			uint64_t offset = (uint64_t)boot_file_extent(&sink,
			    f == 0 ? "BIOS.IMG;1" : "EFI.IMG;1") * BLOCK;

			CHECK(offset + files[f].size <= sink.length);
			for (uint64_t i = 0; i < files[f].size; i++)
				CHECK(sink.bytes[offset + i] == content_byte(f + 1, i));
		}
		CHECK(all_zero(catalog + (kinds == 3 ? 128 : 64),
		    BLOCK - (kinds == 3 ? 128 : 64)));
		CHECK(memory_io(fixture.ctx, &sink, false, &io) == FSIMG_OK);
		for (unsigned v = 0; v < 3; v++) {
			fsimg_read_options_init(&read_options);
			read_options.iso_view = views[v];
			CHECK(fsimg_reader_open(fixture.ctx, io, &read_options,
			    &reader, NULL) == FSIMG_OK);
			CHECK(fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report,
			    NULL) == FSIMG_OK);
			CHECK(report.complete && report.files == 2);
			fsimg_reader_free(reader);
		}
		fsimg_io_free(io);
		free(sink.bytes);
		fixture_close(&fixture);
	}
}

/* Settings and image geometry fail during planning, before emission. */
static void
test_boot_refusals(void)
{
	static const spec_t images[] = {
		{ "/boot", FSIMG_ENTRY_FILE, 0, 0, NULL, NULL },
		{ "/boot", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL },
		{ "/boot", FSIMG_ENTRY_SYMLINK, 0, 0, NULL, "file" },
		{ "/boot", FSIMG_ENTRY_FILE, UINT64_C(0x100000000), 0, NULL, NULL }
	};
	spec_t small = { "/boot", FSIMG_ENTRY_FILE, 1, 0, NULL, NULL };
	fsimg_write_options_t options;
	fixture_t fixture;

	reproducible_options(&options);
	options.boot_bios = "/boot";
	CHECK(plan_status(&options, &small, 0, NULL) == FSIMG_EINVAL);
	for (unsigned i = 0; i < sizeof(images) / sizeof(images[0]); i++)
		CHECK(plan_status(&options, images + i, 1, NULL) == FSIMG_EINVAL);
	CHECK(plan_status(&options, &small, 1, NULL) == FSIMG_OK);
	options.boot_load_sectors = 5;
	CHECK(plan_status(&options, &small, 1, NULL) == FSIMG_EINVAL);
	options.boot_load_sectors = 4;
	CHECK(plan_status(&options, &small, 1, NULL) == FSIMG_OK);
	options.boot_bios = NULL;
	CHECK(fixture_open_with(&fixture, &options, 1, true) == FSIMG_EINVAL);
	fixture_close(&fixture);
	options.boot_bios = "/boot";
	options.boot_load_sectors = 65536;
	CHECK(fixture_open_with(&fixture, &options, 1, true) == FSIMG_EINVAL);
	fixture_close(&fixture);
	options.boot_load_sectors = 0;
	options.boot_bios = NULL;
	options.boot_efi = "/boot";
	CHECK(plan_status(&options, &small, 0, NULL) == FSIMG_EINVAL);
	for (unsigned i = 0; i < sizeof(images) / sizeof(images[0]); i++)
		CHECK(plan_status(&options, images + i, 1, NULL) == FSIMG_EINVAL);
}

/* EFI counts include a partial final sector and become zero above the
 * catalog's 16-bit field. The sparse sink keeps the test payload bounded.
 */
static void
test_efi_sector_counts(void)
{
	fsimg_write_options_t options;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_ops_t ops;
	fsimg_io_t *io;
	fixture_t fixture;
	sparse_sink_t sink;
	spec_t file = { "/boot", FSIMG_ENTRY_FILE, 0, 0, NULL, NULL };
	uint32_t catalog;

	for (unsigned extra = 0; extra < 2; extra++) {
		reproducible_options(&options);
		options.boot_efi = "/boot";
		file.size = UINT64_C(65535) * 512 + extra;
		fixture_open(&fixture, &options);
		CHECK(add_one(&fixture, &file) == FSIMG_OK);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		sink.length = info.volume_bytes;
		sink.bytes = malloc(SPARSE_BYTES);
		CHECK(sink.bytes != NULL);
		memset(sink.bytes, POISON, SPARSE_BYTES);
		fsimg_io_ops_init(&ops);
		ops.get_size = sparse_size;
		ops.read_at = sparse_read;
		ops.write_at = sparse_write;
		ops.zero_range = sparse_zero;
		ops.flush = sparse_flush;
		CHECK(fsimg_io_from_callbacks(fixture.ctx, &ops, &sink, &io,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_writer_emit(fixture.writer, io, NULL) == FSIMG_OK);
		catalog = le32(sink.bytes + 17 * BLOCK + 71);
		CHECK(le16(sink.bytes + (uint64_t)catalog * BLOCK + 38) ==
		    (extra == 0 ? 65535 : 0));
		fsimg_io_free(io);
		free(sink.bytes);
		fixture_close(&fixture);
	}
}

/* Settings outside the profile are refused at construction. */
static void
test_validation(void)
{
	fsimg_write_options_t options, o;
	fixture_t fixture;

	reproducible_options(&options);
	o = options;
	o.sector_bytes = 512;
	CHECK(fixture_open_with(&fixture, &o, 1, true) == FSIMG_EINVAL);
	fixture_close(&fixture);
	o = options;
	o.volume_bytes = 2048 * 100 + 512;
	CHECK(fixture_open_with(&fixture, &o, 1, true) == FSIMG_EINVAL);
	fixture_close(&fixture);
	o = options;
	o.min_free_bytes = 2048;
	CHECK(fixture_open_with(&fixture, &o, 1, true) == FSIMG_EINVAL);
	fixture_close(&fixture);
	o = options;
	o.iso_level = 4;
	CHECK(fixture_open_with(&fixture, &o, 1, true) == FSIMG_EINVAL);
	fixture_close(&fixture);
	o = options;
	o.epoch = INT64_C(5869584000);
	CHECK(fixture_open_with(&fixture, &o, 1, true) == FSIMG_EINVAL);
	fixture_close(&fixture);
	o = options;
	o.label = "seventeen chars!!";
	CHECK(fixture_open_with(&fixture, &o, 1, true) == FSIMG_EINVAL);
	fixture_close(&fixture);
	o = options;
	o.label = "tab\there";
	CHECK(fixture_open_with(&fixture, &o, 1, true) == FSIMG_EINVAL);
	fixture_close(&fixture);
	o = options;
	o.label = "\xf0\x9f\x98\x80";
	CHECK(fixture_open_with(&fixture, &o, 1, true) == FSIMG_EINVAL);
	fixture_close(&fixture);
	o = options;
	o.boot_bios = "/boot.img";
	CHECK(fixture_open_with(&fixture, &o, 1, true) == FSIMG_OK);
	fixture_close(&fixture);
	o = options;
	o.label = "sixteen chars ok";
	CHECK(fixture_open_with(&fixture, &o, 1, true) == FSIMG_OK);
	fixture_close(&fixture);
}

/* Planning refusals: a 256-byte name, a 1024-byte link target, a target
 * component of 256 bytes, a requested volume below the image; a larger
 * requested volume is padded with zero blocks.
 */
static void
test_refusals(void)
{
	static char long_name[300], long_target[1100], wide_target[300];
	fsimg_write_options_t options;
	memory_sink_t sink;
	fixture_t fixture;
	chk_expect_t expect;
	chk_t c;
	spec_t specs[2];
	fsimg_error_t error;

	snprintf(long_name, sizeof(long_name), "/%s", N255 "x");
	memset(long_target, 'a', 1024);
	memset(wide_target, 'b', 256);
	reproducible_options(&options);
	specs[0] = (spec_t){ long_name, FSIMG_ENTRY_FILE, 1, 1, NULL, NULL };
	CHECK(plan_status(&options, specs, 1, &error) == FSIMG_EUNSUPPORTED);
	specs[0] = (spec_t){ "/l", FSIMG_ENTRY_SYMLINK, 0, 0, NULL,
	    long_target };
	CHECK(plan_status(&options, specs, 1, NULL) == FSIMG_EUNSUPPORTED);
	specs[0].target = wide_target;
	CHECK(plan_status(&options, specs, 1, &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "/l") == 0);
	wide_target[255] = '\0';
	CHECK(plan_status(&options, specs, 1, NULL) == FSIMG_OK);

	specs[0] = (spec_t){ "/f", FSIMG_ENTRY_FILE, 5000, 1, NULL, NULL };
	options.volume_bytes = 2048 * 25;
	CHECK(plan_status(&options, specs, 1, NULL) == FSIMG_ERANGE);
	options.volume_bytes = 2048 * 100;
	fixture_open(&fixture, &options);
	add_specs(&fixture, specs, 1, false);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	CHECK(sink.length == 2048 * 100);
	bzero(&expect, sizeof(expect));
	chk_image(&c, sink.bytes, sink.length, &expect);
	CHECK(c.errors == 0);
	chk_free(&c);
	free(sink.bytes);
}

/* Times: an absent modification time takes the fallback, which
 * reproducible mode makes the epoch; a time outside 1900 to 2155, with a
 * fraction or with a basis other than UTC is a counted loss when loss is
 * allowed and refused otherwise; a creation time, which the writer does
 * not store, likewise; a required one always refuses.
 */
static void
test_times(void)
{
	static const fsimg_metadata_t far = {
		.valid = FSIMG_META_MODIFIED,
		.modified = { .seconds = INT64_C(5869584000),
		    .precision_ns = 1000000000, .basis = FSIMG_TIME_UTC }
	};
	static const fsimg_metadata_t fraction = {
		.valid = FSIMG_META_ACCESSED,
		.accessed = { .seconds = 1000, .nanoseconds = 5,
		    .precision_ns = 1, .basis = FSIMG_TIME_UTC }
	};
	static const fsimg_metadata_t created = {
		.valid = FSIMG_META_CREATED,
		.created = { .seconds = 1000, .precision_ns = 1000000000,
		    .basis = FSIMG_TIME_UTC }
	};
	static const fsimg_metadata_t required = {
		.valid = FSIMG_META_CREATED, .required = FSIMG_META_CREATED,
		.created = { .seconds = 1000, .precision_ns = 1000000000,
		    .basis = FSIMG_TIME_UTC }
	};
	static const fsimg_metadata_t exact = {
		.valid = FSIMG_META_MODIFIED | FSIMG_META_ACCESSED,
		.modified = { .seconds = 1000, .precision_ns = 1000000000,
		    .basis = FSIMG_TIME_UTC },
		.accessed = { .seconds = 2000, .precision_ns = 1000000000,
		    .basis = FSIMG_TIME_UTC }
	};
	spec_t specs[] = {
		{ "/far", FSIMG_ENTRY_FILE, 1, 1, &far, NULL },
		{ "/fraction", FSIMG_ENTRY_FILE, 1, 1, &fraction, NULL },
		{ "/created", FSIMG_ENTRY_FILE, 1, 1, &created, NULL },
		{ "/exact", FSIMG_ENTRY_FILE, 1, 1, &exact, NULL }
	};
	fsimg_write_options_t options;
	memory_sink_t sink;
	fixture_t fixture;
	chk_expect_t expect;
	chk_t c;
	unsigned losses;

	fsimg_write_options_init(&options);
	options.flags &= ~FSIMG_WRITE_ALLOW_LOSS;
	CHECK(plan_status(&options, specs, 1, NULL) == FSIMG_EUNSUPPORTED);
	CHECK(plan_status(&options, specs + 3, 1, NULL) == FSIMG_OK);
	options.flags |= FSIMG_WRITE_ALLOW_LOSS;
	specs[2].metadata = &required;
	CHECK(plan_status(&options, specs + 2, 1, NULL) == FSIMG_EUNSUPPORTED);
	specs[2].metadata = &created;

	fixture_open(&fixture, &options);
	add_specs(&fixture, specs, 4, false);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	losses = fixture.losses;
	fixture_close(&fixture);
	CHECK(losses == 3);
	bzero(&expect, sizeof(expect));
	chk_image(&c, sink.bytes, sink.length, &expect);
	CHECK(c.errors == 0);
	CHECK(chk_find(&c, 0, "/exact")->modified == 1000);
	/* The fallback is the planning clock, within a minute of now. */
	CHECK(chk_find(&c, 0, "/far")->modified > time(NULL) - 60);
	chk_free(&c);
	free(sink.bytes);

	/* Reproducible: absent and present times alike become the epoch,
	 * and a present time other than the epoch is a loss.
	 */
	reproducible_options(&options);
	options.flags |= FSIMG_WRITE_ALLOW_LOSS;
	fixture_open(&fixture, &options);
	add_specs(&fixture, specs + 3, 1, false);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	losses = fixture.losses;
	fixture_close(&fixture);
	CHECK(losses == 2);
	chk_image(&c, sink.bytes, sink.length, &expect);
	CHECK(c.errors == 0 && chk_find(&c, 0, "/exact")->modified ==
	    1700000000);
	chk_free(&c);
	free(sink.bytes);
}

/* Both path tables span several blocks, including records split across
 * block boundaries; the independent checker walks their exact byte sizes.
 */
static void
test_multiblock_path_tables(void)
{
	char names[256][16];
	spec_t specs[256];
	fsimg_write_options_t options;
	memory_sink_t sink;
	chk_t c;

	for (unsigned i = 0; i < 256; i++) {
		snprintf(names[i], sizeof(names[i]), "/DIR%04u", i);
		specs[i] = (spec_t){ names[i], FSIMG_ENTRY_DIRECTORY,
		    0, 0, NULL, NULL };
	}
	reproducible_options(&options);
	build_checked(&options, specs, 256, false, &sink, &c);
	CHECK(c.path_bytes[0] > BLOCK && c.path_bytes[1] > BLOCK);
	CHECK(c.dir_count[0] == 257 && c.dir_count[1] == 257);
	chk_free(&c);
	free(sink.bytes);
}

/* Each BMP character occupies one primary character and one Joliet unit. */
static void
test_unicode_label(void)
{
	fsimg_write_options_t options;
	memory_sink_t sink;
	chk_t c;

	reproducible_options(&options);
	options.label = "caf\xc3\xa9-\xe6\x97\xa5";
	build_checked(&options, tree, 0, false, &sink, &c);
	CHECK(bcmp(sink.bytes + 16 * BLOCK + 40, "CAF___", 6) == 0);
	chk_free(&c);
	free(sink.bytes);
}

/* Path-table directory numbers include the root and fit in 16 bits. */
static void
test_directory_count_limit(void)
{
	fsimg_write_options_t options;
	const fsimg_plan_t *plan;
	fixture_t fixture;
	char path[16];
	spec_t directory = { path, FSIMG_ENTRY_DIRECTORY, 0, 0, NULL, NULL };

	reproducible_options(&options);
	for (unsigned count = 65534; count <= 65535; count++) {
		fixture_open(&fixture, &options);
		for (unsigned i = 0; i < count; i++) {
			snprintf(path, sizeof(path), "/D%05u", i);
			CHECK(add_one(&fixture, &directory) == FSIMG_OK);
		}
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) ==
		    (count == 65534 ? FSIMG_OK : FSIMG_EUNSUPPORTED));
		fixture_close(&fixture);
	}
}

/* A row read from scratch must bound a file before section arithmetic. */
static void
test_corrupt_plan_size(void)
{
	static const spec_t file = {
		"/file", FSIMG_ENTRY_FILE, 1, 1, NULL, NULL
	};
	fsimg_write_options_t options;
	const fsimg_plan_t *base;
	unsigned char raw[ISO_ROW_BYTES];
	iso_plan_t *plan;
	iso_row_t row;
	fixture_t fixture;

	reproducible_options(&options);
	fixture_open(&fixture, &options);
	CHECK(add_one(&fixture, &file) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &base, NULL) == FSIMG_OK);
	plan = (iso_plan_t *)base;
	CHECK(fsimg_array_get(plan->rows, 1, raw, NULL) == FSIMG_OK);
	memset(raw, 0xff, 8);
	CHECK(fsimg_array_set(plan->rows, 1, raw, NULL) == FSIMG_OK);
	CHECK(iso_row_get(plan, 1, &row, NULL) == FSIMG_ECORRUPT);
	fixture_close(&fixture);
}

/* A continuation position must be checked before next-fit rounding can
 * wrap it back into an apparently valid region.
 */
static void
test_corrupt_continuation_position(void)
{
	fsimg_write_options_t options;
	const fsimg_plan_t *base;
	fsimg_error_t error;
	iso_plan_t *plan;
	iso_row_t root;
	memory_sink_t sink;
	fixture_t fixture;

	reproducible_options(&options);
	fixture_open(&fixture, &options);
	CHECK(fsimg_writer_plan(fixture.writer, &base, NULL) == FSIMG_OK);
	plan = (iso_plan_t *)base;
	CHECK(iso_row_get(plan, 0, &root, NULL) == FSIMG_OK);
	CHECK(root.child.fragments != 0);
	root.child.fragment_position = UINT64_MAX - 3;
	CHECK(iso_row_set(plan, 0, &root, NULL) == FSIMG_OK);
	CHECK(emit_memory(&fixture, &sink, &error) == FSIMG_ECORRUPT);
	free(sink.bytes);
	fixture_close(&fixture);
}

/* Source callbacks retain their error class when a target is reread. */
static void
test_target_source_error(void)
{
	static const spec_t link = {
		"/link", FSIMG_ENTRY_SYMLINK, 0, 0, NULL, "file"
	};
	fsimg_write_options_t options;
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	fixture_t fixture;
	memory_sink_t sink;

	reproducible_options(&options);
	fixture_open(&fixture, &options);
	CHECK(add_one(&fixture, &link) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	fixture.refuse_target_read = true;
	CHECK(emit_memory(&fixture, &sink, &error) == FSIMG_EUNSUPPORTED);
	CHECK(error.status == FSIMG_EUNSUPPORTED);
	free(sink.bytes);
	fixture_close(&fixture);
}

/* A target reread can join two components without changing its byte
 * length. Crossing the profile's component bound is then a source change.
 */
static void
test_target_component_growth(void)
{
	static const spec_t link = {
		"/link", FSIMG_ENTRY_SYMLINK, 0, 0, NULL, C255 "/b"
	};
	fsimg_write_options_t options;
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	fixture_t fixture;
	memory_sink_t sink;

	reproducible_options(&options);
	fixture_open(&fixture, &options);
	CHECK(add_one(&fixture, &link) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	fixture.join_target_components = true;
	CHECK(emit_memory(&fixture, &sink, &error) == FSIMG_ESOURCE_CHANGED);
	CHECK(strcmp(error.path, "/link") == 0);
	free(sink.bytes);
	fixture_close(&fixture);
}

/* Reader capabilities include fields the writer omits. Explicit losses
 * must remain counted and required fields must remain refusals.
 */
static void
test_writer_omissions(void)
{
	fsimg_metadata_t metadata = {
		.valid = FSIMG_META_CREATED | FSIMG_META_ATTRIBUTES,
		.attributes = FSIMG_ATTR_HIDDEN,
		.created = { .seconds = 1000, .precision_ns = 1000000000,
		    .basis = FSIMG_TIME_UTC }
	};
	spec_t file = { "/file", FSIMG_ENTRY_FILE, 1, 1, &metadata, NULL };
	fsimg_write_options_t options;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_read_options_t read_options;
	const fsimg_entry_t *entry;
	fsimg_reader_t *reader;
	fsimg_io_t *io;
	fixture_t fixture;
	memory_sink_t sink;

	reproducible_options(&options);
	options.flags &= ~FSIMG_WRITE_ALLOW_LOSS;
	CHECK(plan_status(&options, &file, 1, NULL) == FSIMG_EUNSUPPORTED);
	options.flags |= FSIMG_WRITE_ALLOW_LOSS;
	metadata.required = FSIMG_META_CREATED;
	CHECK(plan_status(&options, &file, 1, NULL) == FSIMG_EUNSUPPORTED);
	metadata.required = FSIMG_META_ATTRIBUTES;
	CHECK(plan_status(&options, &file, 1, NULL) == FSIMG_EUNSUPPORTED);
	metadata.required = 0;
	fixture_open(&fixture, &options);
	CHECK(add_one(&fixture, &file) == FSIMG_OK);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(info.metadata_losses == 2 && fixture.losses == 1);
	CHECK(fixture.lost_fields == (FSIMG_META_CREATED | FSIMG_META_ATTRIBUTES));
	CHECK(memory_io(fixture.ctx, &sink, false, &io) == FSIMG_OK);
	fsimg_read_options_init(&read_options);
	CHECK(fsimg_reader_open(fixture.ctx, io, &read_options, &reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK);
	CHECK((entry->metadata.valid & FSIMG_META_CREATED) == 0);
	CHECK(entry->metadata.attributes == 0);
	fsimg_reader_free(reader);
	fsimg_io_free(io);
	free(sink.bytes);
	fixture_close(&fixture);
	/* Even an explicitly supplied zero attributes field is an omission. */
	metadata.valid = FSIMG_META_ATTRIBUTES;
	for (unsigned i = 0; i < 2; i++) {
		metadata.attributes = i == 0 ? 0 : FSIMG_ATTR_HIDDEN;
		fixture_open(&fixture, &options);
		CHECK(add_one(&fixture, &file) == FSIMG_OK);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		CHECK(info.metadata_losses == 1 && fixture.losses == 1);
		CHECK(fixture.lost_fields == FSIMG_META_ATTRIBUTES);
		fixture_close(&fixture);
	}
}

/* The format's read bound is wider than its write profile. Refuse a
 * declared 1024-byte target before a source read; accept 1023 bytes.
 */
static void
test_writer_target_bound(void)
{
	char target[1025];
	spec_t link = { "/link", FSIMG_ENTRY_SYMLINK, 0, 0, NULL, target };
	fsimg_write_options_t options;
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	fixture_t fixture;

	memset(target, '/', 1024);
	target[1024] = '\0';
	reproducible_options(&options);
	fixture_open(&fixture, &options);
	CHECK(add_one(&fixture, &link) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, &error) == FSIMG_EUNSUPPORTED);
	CHECK(fixture.target_reads == 0 && strcmp(error.path, "/link") == 0);
	fixture_close(&fixture);
	target[1023] = '\0';
	fixture_open(&fixture, &options);
	CHECK(add_one(&fixture, &link) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fixture.target_reads != 0);
	fixture_close(&fixture);
}

/* An empty catalog: the root alone in both hierarchies. */
static void
test_empty(void)
{
	fsimg_write_options_t options;
	memory_sink_t sink;
	chk_t c;

	reproducible_options(&options);
	build_checked(&options, tree, 0, false, &sink, &c);
	CHECK(c.dir_count[0] == 1 && c.dir_count[1] == 1);
	chk_free(&c);
	free(sink.bytes);
}

/* Emission failures: a short-writing sink gives the reference bytes; a
 * failing write or flush, a source that changes or ends early, and a
 * link target changed between planning and emission leave the output
 * incomplete.
 */
static void
test_faults(void)
{
	enum { INCOMPLETE, REFERENCE };
	static const struct {
		bool		 fail_write;
		bool		 fail_flush;
		bool		 changed;
		bool		 eof;
		bool		 flip_target;
		size_t		 max_write;
		int		 outcome;
	} cases[] = {
		{ .fail_write = true, .outcome = INCOMPLETE },
		{ .fail_flush = true, .outcome = INCOMPLETE },
		{ .changed = true, .outcome = INCOMPLETE },
		{ .eof = true, .outcome = INCOMPLETE },
		{ .flip_target = true, .outcome = INCOMPLETE },
		{ .max_write = 700, .outcome = REFERENCE }
	};
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink, reference;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *output;
	fsimg_error_t error;
	fsimg_status_t status;

	reproducible_options(&options);
	options.boot_bios = "/big";
	options.boot_efi = "/big";
	fixture_open(&fixture, &options);
	add_specs(&fixture, tree, TREE_COUNT, false);
	CHECK(emit_memory(&fixture, &reference, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fixture_open(&fixture, &options);
		add_specs(&fixture, tree, TREE_COUNT, false);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		bzero(&sink, sizeof(sink));
		sink.length = info.volume_bytes;
		sink.bytes = calloc(1, (size_t)sink.length);
		CHECK(sink.bytes != NULL);
		CHECK(memory_io(fixture.ctx, &sink, true, &output) == FSIMG_OK);
		sink.fail_write = cases[i].fail_write;
		sink.fail_flush = cases[i].fail_flush;
		sink.max_write = cases[i].max_write;
		fixture.changed = cases[i].changed;
		fixture.eof = cases[i].eof;
		fixture.flip_target = cases[i].flip_target;
		status = fsimg_writer_emit(fixture.writer, output, &error);
		if (cases[i].outcome == REFERENCE) {
			CHECK(status == FSIMG_OK);
			CHECK(bcmp(sink.bytes, reference.bytes,
			    (size_t)sink.length) == 0);
		} else {
			CHECK(status < FSIMG_OK);
			CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
			if (cases[i].flip_target)
				CHECK(status == FSIMG_ESOURCE_CHANGED);
		}
		fsimg_io_free(output);
		free(sink.bytes);
		fixture_close(&fixture);
	}
	free(reference.bytes);
}

/* Allocation, metadata-storage and cancellation failures at every point
 * of planning and emission fail cleanly; closing the fixture checks that
 * nothing is left behind.
 */
static void
check_injected(const spec_t *specs, size_t count)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_status_t status;
	bool succeeded;

	reproducible_options(&options);
	options.boot_bios = "/big";
	options.boot_efi = "/big";
	succeeded = false;
	for (uint64_t attempt = 1; attempt < 8192 && !succeeded; attempt++) {
		fixture_open(&fixture, &options);
		add_specs(&fixture, specs, count, false);
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = emit_memory(&fixture, &sink, NULL);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		CHECK(status == FSIMG_OK || status == FSIMG_ENOMEM);
		succeeded = status == FSIMG_OK;
		free(sink.bytes);
		fixture_close(&fixture);
	}
	CHECK(succeeded);
	for (int fault = FSIMG_META_OPEN; fault <= FSIMG_META_WRITE; fault++) {
		succeeded = false;
		for (uint64_t attempt = 1; attempt < 8192 && !succeeded;
		    attempt++) {
			fixture_open(&fixture, &options);
			add_specs(&fixture, specs, count, false);
			fsimg_test_meta_fail_after(fixture.ctx,
			    (fsimg_meta_fault_t)fault, attempt, EIO);
			status = emit_memory(&fixture, &sink, NULL);
			fsimg_test_meta_fail_after(fixture.ctx,
			    (fsimg_meta_fault_t)fault, 0, 0);
			CHECK(status == FSIMG_OK || status == FSIMG_EIO);
			succeeded = status == FSIMG_OK;
			free(sink.bytes);
			fixture_close(&fixture);
		}
		CHECK(succeeded);
	}
	succeeded = false;
	for (uint64_t attempt = 1; attempt < 100000 && !succeeded;
	    attempt += attempt / 16 + 1) {
		fixture_open(&fixture, &options);
		add_specs(&fixture, specs, count, false);
		fixture.cancel_after = attempt;
		fixture.polls = 0;
		status = emit_memory(&fixture, &sink, NULL);
		CHECK(status == FSIMG_OK || status == FSIMG_ECANCELLED);
		succeeded = status == FSIMG_OK;
		free(sink.bytes);
		fixture_close(&fixture);
	}
	CHECK(succeeded);
}

static void
test_injected(void)
{
	check_injected(tree, TREE_COUNT);
	check_injected(relocation_tree, RELOCATION_COUNT);
}

/* --create PATH LEVEL [short]: write the tree at a level to a new file
 * for the external readers. short leaves out the entries whose names or
 * targets pass 200 bytes, which isovfy's fixed buffers cannot hold.
 */
static int
create_external(const char *path, const char *level, bool short_names,
    const spec_t *specs, size_t count)
{
	fsimg_write_options_t options;
	memory_sink_t sink;
	fixture_t fixture;
	FILE *out;

	reproducible_options(&options);
	options.iso_level = (uint32_t)atoi(level);
	options.label = "fsimg tree";
	fixture_open(&fixture, &options);
	for (size_t i = 0; i < count; i++) {
		if (short_names && (strlen(specs[i].path) > 200 ||
		    (specs[i].target != NULL && strlen(specs[i].target) > 200)))
			continue;
		CHECK(add_one(&fixture, &specs[i]) == FSIMG_OK);
	}
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	out = fopen(path, "wbx");
	CHECK(out != NULL);
	CHECK(fwrite(sink.bytes, 1, (size_t)sink.length, out) == sink.length);
	CHECK(fclose(out) == 0);
	free(sink.bytes);
	return (0);
}

/* Firmware qualification uses real files, scanned through common sources.
 * A dash omits a platform; boot paths are image paths within directory.
 */
static int
create_boot_external(const char *path, const char *directory,
    const char *bios, const char *efi)
{
	fsimg_write_options_t options;
	memory_sink_t sink;
	fixture_t fixture;
	FILE *out;
	int source;

	reproducible_options(&options);
	options.flags |= FSIMG_WRITE_ALLOW_LOSS;
	options.boot_bios = strcmp(bios, "-") == 0 ? NULL : bios;
	options.boot_efi = strcmp(efi, "-") == 0 ? NULL : efi;
	fixture_open(&fixture, &options);
	source = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(source != -1);
	CHECK(fsimg_writer_add_tree(fixture.writer, source, "/", NULL) == FSIMG_OK);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	CHECK(close(source) == 0);
	fixture_close(&fixture);
	out = fopen(path, "wbx");
	CHECK(out != NULL);
	CHECK(fwrite(sink.bytes, 1, (size_t)sink.length, out) == sink.length);
	CHECK(fclose(out) == 0);
	free(sink.bytes);
	return (0);
}

/* A real sparse image lets independent readers and kernel mounts qualify
 * multi-extent files without allocating a payload-sized memory sink.
 */
static int
create_large_external(const char *path)
{
	static const spec_t files[] = {
		{ "/huge", FSIMG_ENTRY_FILE,
		    UINT64_C(2) * 0xfffff800u + 4096, 0, NULL, NULL },
		{ "/exact", FSIMG_ENTRY_FILE, UINT32_MAX, 0, NULL, NULL },
		{ "/small", FSIMG_ENTRY_FILE, 3, 1, NULL, NULL }
	};
	fsimg_write_options_t options;
	fixture_t fixture;
	char *directory, *name;
	int parent;

	directory = strdup(path);
	CHECK(directory != NULL);
	name = strrchr(directory, '/');
	CHECK(name != NULL && name != directory && name[1] != '\0');
	*name++ = '\0';
	parent = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(parent != -1);
	reproducible_options(&options);
	fixture_open(&fixture, &options);
	add_specs(&fixture, files, sizeof(files) / sizeof(files[0]), false);
	CHECK(fsimg_writer_write_at(fixture.writer, parent, name, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	CHECK(close(parent) == 0);
	free(directory);
	return (0);
}

/* Run one test; ISO9660_WRITE_DEBUG names each as it starts. */
#define RUN(test) do { \
	if (getenv("ISO9660_WRITE_DEBUG") != NULL) \
		fprintf(stderr, "%s\n", #test); \
	test(); \
} while (0)

int
main(int argc, char **argv)
{
	if (argc == 6 && strcmp(argv[1], "--create-boot") == 0)
		return (create_boot_external(argv[2], argv[3], argv[4], argv[5]));
	if (argc == 4 && strcmp(argv[1], "--create-case") == 0)
		return (create_external(argv[2], argv[3], false, case_tree, CASE_COUNT));
	if (argc == 3 && strcmp(argv[1], "--create-large") == 0)
		return (create_large_external(argv[2]));
	if ((argc == 4 || argc == 5) && strcmp(argv[1], "--create") == 0)
		return (create_external(argv[2], argv[3], argc == 5 &&
		    strcmp(argv[4], "short") == 0, tree, TREE_COUNT));
	if (argc == 4 && strcmp(argv[1], "--create-relocated") == 0)
		return (create_external(argv[2], argv[3], false,
		    relocation_tree, RELOCATION_COUNT));
	CHECK(argc == 1);
	RUN(test_validation);
	RUN(test_writer_omissions);
	RUN(test_writer_target_bound);
	RUN(test_corrupt_plan_size);
	RUN(test_corrupt_continuation_position);
	RUN(test_multiblock_path_tables);
	RUN(test_unicode_label);
	RUN(test_directory_count_limit);
	RUN(test_boot_catalogs);
	RUN(test_boot_refusals);
	RUN(test_efi_sector_counts);
	RUN(test_target_source_error);
	RUN(test_target_component_growth);
	RUN(test_empty);
	RUN(test_relocation_planning);
	RUN(test_relocation_roundtrip);
	RUN(test_relocation_container);
	RUN(test_relocation_extract_create);
	RUN(test_tree);
	RUN(test_primary_relocation_alias);
	RUN(test_primary_lookup_aliases);
	RUN(test_joliet_case_aliases);
	for (unsigned fixture = 0; fixture < 3; fixture++)
		test_joliet_alias_family(fixture);
	RUN(test_identifiers);
	RUN(test_record_boundaries);
	RUN(test_large_file);
	RUN(test_refusals);
	RUN(test_times);
	RUN(test_faults);
	RUN(test_injected);
	return (0);
}
