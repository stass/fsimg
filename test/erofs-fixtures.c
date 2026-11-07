/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "erofs.h"
#include "fsimg.h"
#include "sha256.h"
#include "test.h"

/*
 * Compressed-file tests against images made by mkfs.erofs. The corpus
 * directory named by FSIMG_EROFS_FIXTURES holds one directory per image:
 *
 *   image.erofs  the volume
 *   extents.txt  `dump.erofs -e` of every file, the independent oracle for
 *                where each compressed cluster is and what it covers
 *   manifest.txt tab-separated entries, path first (some are quoted) and, for
 *                a regular file, the SHA-256 of its content last
 *
 * For every regular file stored in a compressed layout, the test walks the
 * reader's compressed lookup (erofs_zmap) across the file and requires each
 * physical cluster to equal the dump's row: logical start and length,
 * physical start and length.
 * It then reads the file through fsimg_reader_read_at in odd-sized chunks and
 * requires the SHA-256 to equal the manifest's, and finally issues seeded
 * random reads, half straddling the cluster boundaries the lookup reported,
 * each of which must equal the same bytes of the sequential read. The run is
 * skipped (exit status 77) without the corpus or a fixture; a fixture
 * without extents.txt is passed over with a note.
 */

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

enum {
	EXIT_SKIP = 77,
	LINE_BYTES = 8192,
	SEQUENTIAL_CHUNK = 65521,	/* prime: never block aligned */
	READS_PER_FILE = 32,		/* half boundary, half uniform */
	READS_PER_FIXTURE = 20000,
	MAX_UNIFORM_READ = 65536,
	BOUNDARY_SLACK = 24,		/* bytes either side of a boundary */
	REPORTED_MISMATCHES = 4,
	LAYOUT_COMPRESSED_FULL = 1,
	LAYOUT_COMPRESSED_COMPACT = 3
};

static const char *const fixture_names[] = {
	"e8-lz4-compact", "e8-lz4-compact-counts", "e8-lz4-compact-xattr",
	"e8-lz4-big16k", "e8-lz4-big64k", "e8-lz4-big1m",
	"e8-lz4-words6m-big1m", "e8-lz4-zeros40m", "e8-lz4-zeros40m-big1m",
	"e8-lz4hc-legacy", "e8-lz4-ztail", "e8-lz4-frag", "e8-lz4-allfrag",
	"e8-lz4-dedupe", "e8-lz4-zdir", "e8-lz4-zdir-legacy", "e8-lz4-big16k-frag",
	"e9-config-four", "e9-mixed-lz4-lzma",
	"e1-cfg-lz4-b4096", "e1-cfg-lz4-b512",
	/*
	 * The E9 fixtures with an extents.txt; the zeros40m ones dump only
	 * one of their two files, so the integration test alone reads them.
	 * The library codecs run in a build that has the library.
	 */
	"e9-deflate-words6m-big1m",
#ifdef FSIMG_MICROLZMA
	"e9-lzma-words6m-big1m",
#endif
#ifdef FSIMG_ZSTD
	"e9-zstd-words6m-big1m",
#endif
};

/* ---- The oracle: dump.erofs -e rows and the manifest ---- */

/* One `dump.erofs -e` row: a logical range and where it is stored. */
typedef struct dump_row {
	uint64_t	 logical_start;
	uint64_t	 logical_end;
	uint64_t	 logical_length;
	uint64_t	 physical_start;
	uint64_t	 physical_end;
	uint64_t	 physical_length;
} dump_row_t;

typedef struct dump_file {
	char		*path;
	int		 layout;
	dump_row_t	*rows;
	size_t		 row_count;
	size_t		 row_capacity;
	bool		 walked;	/* the reader reached this file */
} dump_file_t;

typedef struct manifest_entry {
	char		*path;
	char		 digest[FSIMG_SHA256_DIGEST_STRING_LENGTH];
} manifest_entry_t;

typedef struct oracle {
	dump_file_t	*files;
	size_t		 file_count;
	size_t		 file_capacity;
	manifest_entry_t *manifest;
	size_t		 manifest_count;
	size_t		 manifest_capacity;
} oracle_t;

/* What one fixture's run counted. */
typedef struct tally {
	const char	*fixture;
	uint64_t	 files;
	uint64_t	 extents;
	uint64_t	 extent_mismatches;
	uint64_t	 reads;
	uint64_t	 read_mismatches;
	uint64_t	 digest_mismatches;
	uint64_t	 errors;
	unsigned	 reported;
} tally_t;

static void *
checked_realloc(void *pointer, size_t count, size_t size)
{
	CHECK(count <= SIZE_MAX / size);
	pointer = realloc(pointer, count * size);
	CHECK(pointer != NULL);
	return (pointer);
}

static char *
duplicate(const char *text)
{
	char *copy = strdup(text);

	CHECK(copy != NULL);
	return (copy);
}

/* Print a mismatch, but only the first few of a fixture, to keep logs short. */
static void
report(tally_t *tally, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

static void
report(tally_t *tally, const char *format, ...)
{
	va_list ap;

	if (tally->reported >= REPORTED_MISMATCHES)
		return;
	tally->reported++;
	fprintf(stderr, "%s: ", tally->fixture);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static dump_file_t *
dump_file_add(oracle_t *oracle, const char *path)
{
	dump_file_t *file;

	if (oracle->file_count == oracle->file_capacity) {
		oracle->file_capacity = oracle->file_capacity * 2 + 16;
		oracle->files = checked_realloc(oracle->files,
		    oracle->file_capacity, sizeof(*oracle->files));
	}
	file = &oracle->files[oracle->file_count++];
	bzero(file, sizeof(*file));
	file->path = duplicate(path);
	return (file);
}

static void
dump_file_add_row(dump_file_t *file, const dump_row_t *row)
{
	if (file->row_count == file->row_capacity) {
		file->row_capacity = file->row_capacity * 2 + 8;
		file->rows = checked_realloc(file->rows, file->row_capacity,
		    sizeof(*file->rows));
	}
	file->rows[file->row_count++] = *row;
}

static void
strip_newline(char *line)
{
	line[strcspn(line, "\r\n")] = '\0';
}

/*
 * Parse extents.txt: a "Path : /name" line starts a file, its "NID: ...
 * Layout: n" line gives the layout, and each row after that is
 *   index: start..end | length : start..end | length
 * Only files in a compressed layout (1 or 3) are kept.
 */
static void
oracle_load_extents(oracle_t *oracle, const char *path)
{
	static const char path_prefix[] = "Path : ";
	char line[LINE_BYTES];
	dump_file_t *current = NULL;
	FILE *file = fopen(path, "r");

	CHECK(file != NULL);
	while (fgets(line, sizeof(line), file) != NULL) {
		const char *layout;
		dump_row_t row;

		strip_newline(line);
		if (strncmp(line, path_prefix, sizeof(path_prefix) - 1) == 0) {
			current = dump_file_add(oracle,
			    line + sizeof(path_prefix) - 1);
		} else if (current != NULL &&
		    (layout = strstr(line, "Layout: ")) != NULL) {
			current->layout = atoi(layout + strlen("Layout: "));
		} else if (current != NULL &&
		    sscanf(line, " %*u: %" SCNu64 " .. %" SCNu64 " | %" SCNu64
		    " : %" SCNu64 " .. %" SCNu64 " | %" SCNu64,
		    &row.logical_start, &row.logical_end, &row.logical_length,
		    &row.physical_start, &row.physical_end,
		    &row.physical_length) == 6) {
			dump_file_add_row(current, &row);
		}
	}
	CHECK(!ferror(file));
	fclose(file);
}

/* Parse manifest.txt: the path is the first tab-separated column. */
static void
oracle_load_manifest(oracle_t *oracle, const char *path)
{
	char line[LINE_BYTES];
	FILE *file = fopen(path, "r");

	/*
	 * Every listed fixture carries a manifest; one without it is a
	 * broken corpus, not a fixture to skip.
	 */
	CHECK(file != NULL);
	while (fgets(line, sizeof(line), file) != NULL) {
		manifest_entry_t *entry;
		char *last_tab, *first_tab;

		strip_newline(line);
		first_tab = strchr(line, '\t');
		last_tab = strrchr(line, '\t');
		CHECK(first_tab != NULL);
		if (oracle->manifest_count == oracle->manifest_capacity) {
			oracle->manifest_capacity =
			    oracle->manifest_capacity * 2 + 16;
			oracle->manifest = checked_realloc(oracle->manifest,
			    oracle->manifest_capacity,
			    sizeof(*oracle->manifest));
		}
		entry = &oracle->manifest[oracle->manifest_count++];
		bzero(entry, sizeof(*entry));
		*first_tab = '\0';
		/* Some manifests quote the path. */
		if (first_tab - line >= 2 && line[0] == '"' &&
		    first_tab[-1] == '"') {
			first_tab[-1] = '\0';
			entry->path = duplicate(line + 1);
		} else {
			entry->path = duplicate(line);
		}
		/* A directory or special file has "-" or no digest. */
		if (strlen(last_tab + 1) ==
		    FSIMG_SHA256_DIGEST_STRING_LENGTH - 1)
			strcpy(entry->digest, last_tab + 1);
	}
	CHECK(!ferror(file));
	fclose(file);
}

static void
oracle_free(oracle_t *oracle)
{
	for (size_t i = 0; i < oracle->file_count; i++) {
		free(oracle->files[i].path);
		free(oracle->files[i].rows);
	}
	for (size_t i = 0; i < oracle->manifest_count; i++)
		free(oracle->manifest[i].path);
	free(oracle->files);
	free(oracle->manifest);
	bzero(oracle, sizeof(*oracle));
}

static dump_file_t *
oracle_find_file(oracle_t *oracle, const char *path)
{
	for (size_t i = 0; i < oracle->file_count; i++) {
		if (strcmp(oracle->files[i].path, path) == 0)
			return (&oracle->files[i]);
	}
	return (NULL);
}

static const manifest_entry_t *
oracle_find_manifest(const oracle_t *oracle, const char *path)
{
	for (size_t i = 0; i < oracle->manifest_count; i++) {
		if (strcmp(oracle->manifest[i].path, path) == 0)
			return (&oracle->manifest[i]);
	}
	return (NULL);
}

/* ---- Deterministic randomness ---- */

typedef struct prng {
	uint64_t	 state;
} prng_t;

/* xorshift64*: small, seedable and the same on every host. */
static uint64_t
prng_next(prng_t *prng)
{
	prng->state ^= prng->state >> 12;
	prng->state ^= prng->state << 25;
	prng->state ^= prng->state >> 27;
	return (prng->state * UINT64_C(0x2545F4914F6CDD1D));
}

/* A value in [0, bound), bound > 0. */
static uint64_t
prng_below(prng_t *prng, uint64_t bound)
{
	return (prng_next(prng) % bound);
}

/* ---- Checks on one compressed file ---- */

typedef struct file_case {
	oracle_t	*oracle;
	tally_t		*tally;
	fsimg_reader_t	*reader;
	prng_t		*prng;
	const char	*path;		/* with a leading slash */
	const erofs_inode_t *inode;
	dump_file_t	*dumped;
	uint64_t	*boundaries;	/* logical ends of the mapped extents */
	size_t		 boundary_count;
} file_case_t;

static void
note_error(tally_t *tally, const char *path, const char *what,
    fsimg_status_t status, const fsimg_error_t *error)
{
	tally->errors++;
	report(tally, "%s: %s failed with status %d: %s", path, what,
	    (int)status, error->message);
}

/*
 * Does a physical cluster agree with the dump's row? The dump prints a
 * fragment, whose bytes lie in the packed inode, as physical 0..0 of length
 * 0, and an inline tail at its absolute volume offset with its stored size.
 */
static bool
cluster_matches_row(const erofs_cluster_t *cluster, const dump_row_t *row)
{
	if (cluster->logical_offset != row->logical_start ||
	    cluster->logical_length != row->logical_length)
		return (false);
	if (cluster->source == EROFS_SOURCE_PACKED)
		return (row->physical_start == 0 && row->physical_length == 0);
	return (cluster->storage_offset == row->physical_start &&
	    cluster->stored_bytes == row->physical_length);
}

/*
 * Walk the compressed lookup (erofs_zmap) from offset 0 to the file's size,
 * comparing each physical cluster with the dump's row of the same index,
 * and remember the cluster ends as the boundaries the random reads aim at.
 * The clusters, not erofs_map's extents, are what the dump prints: the
 * mapper clips a fragment to the packed inode's extents.
 */
static void
compare_extents(file_case_t *c)
{
	const dump_file_t *dumped = c->dumped;
	uint64_t offset = 0;
	size_t index = 0;

	c->boundary_count = 0;
	c->boundaries = checked_realloc(NULL, dumped->row_count + 1,
	    sizeof(*c->boundaries));
	while (offset < c->inode->size) {
		const dump_row_t *row;
		erofs_cluster_t cluster;
		fsimg_error_t error;
		fsimg_status_t status;

		status = erofs_zmap(c->reader, c->inode, offset, &cluster,
		    &error);
		if (status != FSIMG_OK) {
			note_error(c->tally, c->path, "erofs_zmap", status,
			    &error);
			return;
		}
		if (cluster.logical_length == 0 ||
		    cluster.logical_offset != offset) {
			c->tally->extent_mismatches++;
			report(c->tally, "%s: offset %" PRIu64 " maps to an "
			    "empty or misplaced cluster at %" PRIu64, c->path,
			    offset, cluster.logical_offset);
			return;
		}
		c->tally->extents++;
		if (index >= dumped->row_count) {
			c->tally->extent_mismatches++;
			report(c->tally, "%s: cluster %zu at %" PRIu64
			    " has no dump row (%zu rows)", c->path, index,
			    cluster.logical_offset, dumped->row_count);
			return;
		}
		row = &dumped->rows[index];
		if (!cluster_matches_row(&cluster, row)) {
			c->tally->extent_mismatches++;
			report(c->tally, "%s row %zu: dump logical %" PRIu64
			    "..%" PRIu64 " (%" PRIu64 ") physical %" PRIu64
			    "..%" PRIu64 " (%" PRIu64 "); cluster logical %"
			    PRIu64 " (%" PRIu64 ") source %d physical %"
			    PRIu64 " (%" PRIu32 ")", c->path, index,
			    row->logical_start, row->logical_end,
			    row->logical_length, row->physical_start,
			    row->physical_end, row->physical_length,
			    cluster.logical_offset, cluster.logical_length,
			    (int)cluster.source, cluster.storage_offset,
			    cluster.stored_bytes);
		}
		offset = cluster.logical_offset + cluster.logical_length;
		if (c->boundary_count < dumped->row_count + 1)
			c->boundaries[c->boundary_count++] = offset;
		index++;
	}
	if (index < dumped->row_count) {
		c->tally->extent_mismatches++;
		report(c->tally, "%s: lookup stopped after %zu clusters, dump "
		    "has %zu rows", c->path, index, dumped->row_count);
	}
}

/* Read exactly length bytes at offset, looping over short answers. */
static fsimg_status_t
read_exact(fsimg_reader_t *reader, uint64_t offset, void *buffer,
    size_t length, fsimg_error_t *error)
{
	unsigned char *bytes = buffer;
	size_t done = 0;

	while (done < length) {
		size_t transferred = 0;
		fsimg_status_t status;

		status = fsimg_reader_read_at(reader, offset + done,
		    bytes + done, length - done, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		if (transferred == 0)
			return (FSIMG_ECORRUPT);
		done += transferred;
	}
	return (FSIMG_OK);
}

/* Read the whole file in odd-sized chunks and hash it. */
static bool
read_sequentially(file_case_t *c, unsigned char *content, char *hex)
{
	unsigned char digest[FSIMG_SHA256_DIGEST_LENGTH];
	fsimg_sha256_t sha;
	uint64_t offset;

	fsimg_sha256_init(&sha);
	for (offset = 0; offset < c->inode->size; offset += SEQUENTIAL_CHUNK) {
		uint64_t left = c->inode->size - offset;
		size_t chunk = left < SEQUENTIAL_CHUNK ? (size_t)left :
		    SEQUENTIAL_CHUNK;
		fsimg_error_t error;
		fsimg_status_t status;

		status = read_exact(c->reader, offset, content + offset, chunk,
		    &error);
		if (status != FSIMG_OK) {
			note_error(c->tally, c->path, "sequential read",
			    status, &error);
			return (false);
		}
		fsimg_sha256_update(&sha, content + offset, chunk);
	}
	fsimg_sha256_final(digest, &sha);
	for (size_t i = 0; i < sizeof(digest); i++)
		snprintf(hex + 2 * i, 3, "%02x", digest[i]);
	return (true);
}

static void
compare_digest(file_case_t *c, const char *hex)
{
	const manifest_entry_t *entry;

	entry = oracle_find_manifest(c->oracle, c->path);
	if (entry == NULL || entry->digest[0] == '\0') {
		c->tally->digest_mismatches++;
		report(c->tally, "%s: no manifest digest", c->path);
	} else if (strcmp(entry->digest, hex) != 0) {
		c->tally->digest_mismatches++;
		report(c->tally, "%s: digest %s, manifest %s", c->path, hex,
		    entry->digest);
	}
}

/* One random read, compared with the same range of the sequential bytes. */
static void
random_read(file_case_t *c, const unsigned char *content, uint64_t offset,
    uint64_t length)
{
	fsimg_error_t error;
	fsimg_status_t status;
	unsigned char *buffer;

	buffer = checked_realloc(NULL, (size_t)length, 1);
	c->tally->reads++;
	status = read_exact(c->reader, offset, buffer, (size_t)length, &error);
	if (status != FSIMG_OK) {
		note_error(c->tally, c->path, "random read", status, &error);
	} else if (bcmp(buffer, content + offset, (size_t)length) != 0) {
		c->tally->read_mismatches++;
		report(c->tally, "%s: read of %" PRIu64 " bytes at %" PRIu64
		    " differs from the sequential bytes", c->path, length,
		    offset);
	}
	free(buffer);
}

/*
 * A read that starts a few bytes before the end of one cluster's logical
 * range and ends a few bytes after the next one's, clipped to the file.
 */
static void
boundary_read(file_case_t *c, const unsigned char *content)
{
	uint64_t size = c->inode->size;
	uint64_t slack = prng_below(c->prng, BOUNDARY_SLACK) + 1;
	size_t pick = (size_t)prng_below(c->prng, c->boundary_count);
	uint64_t start, end;

	start = c->boundaries[pick] > slack ? c->boundaries[pick] - slack : 0;
	end = pick + 1 < c->boundary_count ? c->boundaries[pick + 1] :
	    c->boundaries[pick];
	end += prng_below(c->prng, BOUNDARY_SLACK) + 1;
	if (end > size)
		end = size;
	if (start >= end)
		start = end - 1;
	random_read(c, content, start, end - start);
}

static void
uniform_read(file_case_t *c, const unsigned char *content)
{
	uint64_t size = c->inode->size;
	uint64_t start = prng_below(c->prng, size);
	uint64_t length = prng_below(c->prng, MAX_UNIFORM_READ) + 1;

	if (length > size - start)
		length = size - start;
	random_read(c, content, start, length);
}

static void
random_reads(file_case_t *c, const unsigned char *content)
{
	for (int i = 0; i < READS_PER_FILE; i++) {
		if (c->tally->reads >= READS_PER_FIXTURE)
			return;
		if (i % 2 == 0 && c->boundary_count > 0)
			boundary_read(c, content);
		else
			uniform_read(c, content);
	}
}

/* Check one compressed regular file against the oracle. */
static void
check_file(file_case_t *c)
{
	char hex[FSIMG_SHA256_DIGEST_STRING_LENGTH];
	unsigned char *content;

	c->tally->files++;
	compare_extents(c);
	if (c->inode->size == 0) {
		free(c->boundaries);
		return;
	}
	CHECK(c->inode->size <= SIZE_MAX);
	content = checked_realloc(NULL, (size_t)c->inode->size, 1);
	if (read_sequentially(c, content, hex)) {
		compare_digest(c, hex);
		if (c->boundary_count > 0 || c->inode->size > 1)
			random_reads(c, content);
	}
	free(content);
	free(c->boundaries);
}

/* ---- One fixture ---- */

static bool
is_compressed(const erofs_inode_t *inode)
{
	return (inode->layout == EROFS_LAYOUT_COMPRESSED_FULL ||
	    inode->layout == EROFS_LAYOUT_COMPRESSED_COMPACT);
}

/* The mapper path a reader entry would have in dump.erofs: leading slash. */
static void
dump_path(char *buffer, size_t size, const char *entry_path)
{
	snprintf(buffer, size, "%s%s", entry_path[0] == '/' ? "" : "/",
	    entry_path);
}

/* Walk every entry of the image and check each compressed regular file. */
static void
walk_image(oracle_t *oracle, tally_t *tally, fsimg_reader_t *reader)
{
	prng_t prng = { UINT64_C(0x9E3779B97F4A7C15) };

	for (;;) {
		const fsimg_entry_t *entry;
		fsimg_error_t error;
		fsimg_status_t status;
		const erofs_inode_t *inode;
		file_case_t c;
		char path[LINE_BYTES];

		status = fsimg_reader_next(reader, &entry, &error);
		if (status == FSIMG_DONE)
			return;
		if (status != FSIMG_OK) {
			note_error(tally, "(walk)", "fsimg_reader_next", status,
			    &error);
			return;
		}
		inode = &erofs_state(reader)->current;
		if (entry->kind != FSIMG_ENTRY_FILE || !is_compressed(inode))
			continue;

		dump_path(path, sizeof(path), entry->path);
		bzero(&c, sizeof(c));
		c.oracle = oracle;
		c.tally = tally;
		c.reader = reader;
		c.prng = &prng;
		c.path = path;
		c.inode = inode;
		c.dumped = oracle_find_file(oracle, path);
		if (c.dumped == NULL) {
			tally->extent_mismatches++;
			report(tally, "%s: compressed in the image, absent "
			    "from the dump", path);
			continue;
		}
		c.dumped->walked = true;
		check_file(&c);
	}
}

/* Every compressed file the dump lists must have been reached. */
static void
check_dump_covered(oracle_t *oracle, tally_t *tally)
{
	for (size_t i = 0; i < oracle->file_count; i++) {
		const dump_file_t *file = &oracle->files[i];

		if (file->walked || (file->layout != LAYOUT_COMPRESSED_FULL &&
		    file->layout != LAYOUT_COMPRESSED_COMPACT))
			continue;
		tally->extent_mismatches++;
		report(tally, "%s: compressed in the dump, not seen as a "
		    "compressed file by the reader", file->path);
	}
}

static void
drop_uncompressed(oracle_t *oracle)
{
	size_t kept = 0;

	for (size_t i = 0; i < oracle->file_count; i++) {
		dump_file_t *file = &oracle->files[i];

		if (file->layout == LAYOUT_COMPRESSED_FULL ||
		    file->layout == LAYOUT_COMPRESSED_COMPACT) {
			oracle->files[kept++] = *file;
		} else {
			free(file->path);
			free(file->rows);
		}
	}
	oracle->file_count = kept;
}

static void
print_summary(const tally_t *tally)
{
	printf("%s: files %" PRIu64 ", extents %" PRIu64 ", extent mismatches "
	    "%" PRIu64 ", random reads %" PRIu64 ", read mismatches %" PRIu64
	    ", digest mismatches %" PRIu64 ", errors %" PRIu64 "\n",
	    tally->fixture, tally->files, tally->extents,
	    tally->extent_mismatches, tally->reads, tally->read_mismatches,
	    tally->digest_mismatches, tally->errors);
	fflush(stdout);
}

static bool
tally_clean(const tally_t *tally)
{
	return (tally->extent_mismatches == 0 && tally->read_mismatches == 0 &&
	    tally->digest_mismatches == 0 && tally->errors == 0);
}

static void
join(char *out, size_t size, const char *directory, const char *name,
    const char *file)
{
	int n = snprintf(out, size, "%s/%s/%s", directory, name, file);

	CHECK(n > 0 && (size_t)n < size);
}

static bool
file_exists(const char *path)
{
	return (access(path, R_OK) == 0);
}

/* Run one fixture; returns false when it had a mismatch. */
static bool
run_fixture(const char *corpus, const char *name)
{
	char image_path[1024], extents_path[1024], manifest_path[1024];
	oracle_t oracle = { 0 };
	tally_t tally = { .fixture = name };
	fsimg_options_t options;
	fsimg_reader_t *reader = NULL;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	int fd;

	join(image_path, sizeof(image_path), corpus, name, "image.erofs");
	join(extents_path, sizeof(extents_path), corpus, name, "extents.txt");
	join(manifest_path, sizeof(manifest_path), corpus, name,
	    "manifest.txt");
	if (!file_exists(extents_path)) {
		fprintf(stderr, "%s: no extents.txt, skipped\n", name);
		return (true);
	}

	oracle_load_extents(&oracle, extents_path);
	oracle_load_manifest(&oracle, manifest_path);
	drop_uncompressed(&oracle);

	fd = open(image_path, O_RDONLY);
	CHECK(fd >= 0);
	fsimg_options_init(&options);
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_io_from_fd(ctx, fd, FSIMG_IO_READ, &io, NULL) == FSIMG_OK);
	if (fsimg_reader_open(ctx, io, NULL, &reader, &error) != FSIMG_OK) {
		note_error(&tally, "(open)", "fsimg_reader_open", error.status,
		    &error);
	} else {
		walk_image(&oracle, &tally, reader);
		check_dump_covered(&oracle, &tally);
		fsimg_reader_free(reader);
	}
	fsimg_io_free(io);
	fsimg_ctx_free(ctx);
	close(fd);
	oracle_free(&oracle);

	print_summary(&tally);
	return (tally_clean(&tally));
}

int
main(void)
{
	const char *corpus = getenv("FSIMG_EROFS_FIXTURES");
	bool clean = true;

	if (corpus == NULL || corpus[0] == '\0') {
		fprintf(stderr, "FSIMG_EROFS_FIXTURES is not set\n");
		return (EXIT_SKIP);
	}
	for (size_t i = 0; i < ARRAY_SIZE(fixture_names); i++) {
		char image_path[1024];

		join(image_path, sizeof(image_path), corpus, fixture_names[i],
		    "image.erofs");
		if (!file_exists(image_path)) {
			fprintf(stderr, "fixture %s is missing\n",
			    fixture_names[i]);
			return (EXIT_SKIP);
		}
	}
	for (size_t i = 0; i < ARRAY_SIZE(fixture_names); i++) {
		if (!run_fixture(corpus, fixture_names[i]))
			clean = false;
	}
	return (clean ? 0 : 1);
}
