/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "fsimg.h"
#include "host.h"
#include "scale.h"
#include "sha256.h"
#include "test.h"

/*
 * The E13 scale fixtures read under the minimum budget. The corpus named by
 * FSIMG_EROFS_FIXTURES holds each fixture's image.erofs and manifest.txt;
 * `test-erofs-scale NAME` opens one image in a 16 MiB context with a
 * scratch directory, lists every entry and reads every regular file through
 * fsimg_reader_next_range and fsimg_reader_read_at, hashing data ranges as
 * read and zero ranges without reading; data_bytes counts the content the
 * reader delivered, decoded for a compressed file. Each entry count, file
 * size and nonempty file's SHA-256 must equal the manifest's.
 * `test-erofs-scale --list` prints the fixture names, which check-scale
 * runs one by one.
 *
 * The limits images are 1.6 to 3.3 TiB apparent and a few KiB allocated, and
 * the plain 5 GiB image is sparse: nothing here copies, hashes or walks an
 * image file. The volume callback counts every byte the reader asks for and
 * stops the run at the case's bound, so a reader that strayed into an
 * image's hole, or read a chunked file's hole chunks, fails at the bound
 * rather than reading terabytes. While the reader refuses the 48-bit layout
 * and the metadata box, their cases must fail at open with
 * FSIMG_EUNSUPPORTED inside the same bound.
 *
 * The process is the measurement: one fixture per run, charged memory
 * bounded by the metadata cache's maximum, one sort run and the driver's
 * state, the peak resident set's growth from the start of the operation
 * bounded with it, and both printed beside the I/O and scratch figures.
 * Exit status 77 means the corpus variable is unset or the fixture's image
 * does not exist.
 */

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIB (UINT64_C(1) << 20)
/* Process growth beyond charged memory: libc, the test's buffers, pages. */
#define RSS_GROWTH (UINT64_C(32) << 20)
/*
 * Charged state beside the metadata cache and a sort run: the reader, the
 * walker and a decode window. The wide directory fills the cache and adds
 * about 90 KiB, so per-entry state in memory, even 8 bytes a name, exceeds
 * the bound; the LZ4 case's windows fit inside the cache it barely uses.
 */
#define DRIVER_STATE (UINT64_C(512) << 10)

enum {
	EXIT_SKIP = 77,
	LINE_BYTES = 8192,
	READ_CHUNK = 1 << 20,
	MAX_SIZED_FILES = 8
};

/*
 * One fixture: what open must return; the most image bytes it may read,
 * length_factor times the image's length plus slack bytes; and whether its
 * listing must spool children to scratch.
 */
typedef struct scale_case {
	const char	*name;
	fsimg_status_t	 open_status;
	uint64_t	 length_factor;
	uint64_t	 slack;
	bool		 spools;
} scale_case_t;

/*
 * The bounds are what each image needs read once, with room for metadata
 * read again. The common reader serves a read in 64 KiB steps, and each
 * step maps its range afresh: the plain image reads its own length, 5 GiB
 * of stored zeros, once; the chunked image its own length and a 4-byte
 * chunk entry again per step, within 1 MiB more, and nothing for a hole
 * chunk; the LZ4 image its input once and one compact index group for each
 * of its 1310721 logical clusters, within four times its length; the wide
 * directory its blocks once to prepare and its 100000 inodes once to
 * replay, within twice its length; and each limits image, whose hole spans
 * terabytes, at most 1 MiB whatever its length.
 */
static const scale_case_t cases[] = {
	{ "e13-big5g", FSIMG_OK, 1, MIB, false },
	{ "e13-big5g-chunk1m", FSIMG_OK, 1, MIB, false },
	{ "e13-big5g-lz4", FSIMG_OK, 4, 0, false },
	{ "e13-dir100k", FSIMG_OK, 2, 0, true },
	{ "e13-limits-startblk", FSIMG_OK, 0, MIB, false },
	{ "e13-limits-nid", FSIMG_OK, 0, MIB, false },
	{ "e13-limits-48bit", FSIMG_EUNSUPPORTED, 0, MIB, false },
	{ "e13-limits-metabox", FSIMG_EUNSUPPORTED, 0, MIB, false }
};

/* A manifest file row with content, which the walk must meet once. */
typedef struct sized_file {
	char		 path[256];
	uint64_t	 size;
	char		 digest[FSIMG_SHA256_DIGEST_STRING_LENGTH];
	bool		 seen;
} sized_file_t;

/* What the manifest promises: entries below the root, and its files. */
typedef struct manifest {
	uint64_t	 entries;
	uint64_t	 empty_files;
	sized_file_t	 sized[MAX_SIZED_FILES];
	size_t		 sized_count;
} manifest_t;

/* The image as the reader sees it, counting what it asks for up to limit. */
typedef struct volume {
	int		 fd;
	uint64_t	 length;
	uint64_t	 limit;
	uint64_t	 calls;
	uint64_t	 bytes;
	uint64_t	 max_end;
} volume_t;

/* One run's figures. */
typedef struct figures {
	uint64_t	 entries;
	uint64_t	 files;
	uint64_t	 data_bytes;
	uint64_t	 zero_bytes;
	uint64_t	 rss;
	uint64_t	 rss_growth;
	double		 seconds;
	fsimg_stats_t	 stats;
} figures_t;

static const scale_case_t *case_find(const char *);
static void	manifest_load(const char *, manifest_t *);
static sized_file_t *manifest_take(manifest_t *, const char *);
static fsimg_status_t volume_read(void *, uint64_t, void *, size_t, size_t *,
		    fsimg_error_t *);
static fsimg_status_t volume_size(void *, uint64_t *, fsimg_error_t *);
static void	content_hash(fsimg_reader_t *, const fsimg_entry_t *,
		    unsigned char *, figures_t *, char *);
static fsimg_status_t tree_read_against_manifest(fsimg_reader_t *,
		    manifest_t *, figures_t *, fsimg_error_t *);
static fsimg_ctx_t *context_open(const char *);
static void	context_close(fsimg_ctx_t *, const fsimg_stats_t *);
static void	fixture_measure(const scale_case_t *, const char *,
		    volume_t *, manifest_t *, figures_t *);
static void	scratch_remove(void);

/* The scratch directory's name, removed at exit even after a failure. */
static char scratch_path[] = "erofs-scale-XXXXXX";

static const scale_case_t *
case_find(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		if (strcmp(cases[i].name, name) == 0)
			return (&cases[i]);
	}
	return (NULL);
}

/*
 * Read manifest.txt: tab-separated rows of a quoted path, kind and size
 * first and, for a regular file, its SHA-256 last. Empty files are only
 * counted, so the 100000 of the wide directory cost nothing here.
 */
static void
manifest_load(const char *path, manifest_t *manifest)
{
	char line[LINE_BYTES];
	FILE *file;

	bzero(manifest, sizeof(*manifest));
	file = fopen(path, "r");
	CHECK(file != NULL);
	while (fgets(line, sizeof(line), file) != NULL) {
		char *cursor = line, *name, *kind, *size, *digest, *end;
		sized_file_t *sized;
		size_t length;

		line[strcspn(line, "\n")] = '\0';
		name = strsep(&cursor, "\t");
		kind = strsep(&cursor, "\t");
		size = strsep(&cursor, "\t");
		CHECK(kind != NULL && size != NULL);
		/* The digest is the last column, after the rest's last tab. */
		digest = cursor != NULL ? strrchr(cursor, '\t') : NULL;
		length = strlen(name);
		CHECK(length >= 2 && name[0] == '"' && name[length - 1] == '"');
		name[length - 1] = '\0';
		name++;
		/* The fixtures' names need no unquoting. */
		CHECK(strchr(name, '\\') == NULL);
		if (strcmp(name, "/") == 0)
			continue;
		manifest->entries++;
		if (strcmp(kind, "file") != 0)
			continue;
		if (strcmp(size, "0") == 0) {
			manifest->empty_files++;
			continue;
		}
		CHECK(manifest->sized_count < MAX_SIZED_FILES);
		CHECK(digest != NULL && strlen(digest + 1) ==
		    FSIMG_SHA256_DIGEST_STRING_LENGTH - 1);
		CHECK(strlen(name) < sizeof(sized->path));
		sized = &manifest->sized[manifest->sized_count++];
		strcpy(sized->path, name);
		errno = 0;
		sized->size = strtoull(size, &end, 10);
		CHECK(errno == 0 && *end == '\0' && sized->size != 0);
		strcpy(sized->digest, digest + 1);
	}
	CHECK(!ferror(file));
	CHECK(fclose(file) == 0);
}

/* The manifest row for path, which must not have been met before. */
static sized_file_t *
manifest_take(manifest_t *manifest, const char *path)
{
	for (size_t i = 0; i < manifest->sized_count; i++) {
		sized_file_t *sized = &manifest->sized[i];

		if (strcmp(sized->path, path) == 0) {
			CHECK(!sized->seen);
			sized->seen = true;
			return (sized);
		}
	}
	fprintf(stderr, "scale: %s has content the manifest lacks\n", path);
	exit(1);
}

static fsimg_status_t
volume_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	volume_t *v = argument;
	size_t done = 0;

	CHECK(offset <= v->length && length <= v->length - offset);
	if (length > v->limit - v->bytes) {
		fprintf(stderr, "scale: a read of %zu bytes at %" PRIu64
		    " passes the bound of %" PRIu64 " image bytes\n", length,
		    offset, v->limit);
		exit(1);
	}
	while (done < length) {
		ssize_t n = pread(v->fd, (char *)buffer + done, length - done,
		    (off_t)(offset + done));

		CHECK(n > 0);
		done += (size_t)n;
	}
	v->calls++;
	v->bytes += length;
	if (offset + length > v->max_end)
		v->max_end = offset + length;
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
volume_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((volume_t *)argument)->length;
	return (FSIMG_OK);
}

/*
 * Hash the current file into hex: data ranges read through the reader in
 * chunks of buffer's READ_CHUNK bytes, zero ranges hashed from zeros
 * without a read.
 */
static void
content_hash(fsimg_reader_t *reader, const fsimg_entry_t *entry,
    unsigned char *buffer, figures_t *figures, char *hex)
{
	static const unsigned char zeros[READ_CHUNK];
	unsigned char digest[FSIMG_SHA256_DIGEST_LENGTH];
	fsimg_sha256_t sha;
	fsimg_range_t range;
	fsimg_error_t error;
	uint64_t offset = 0;

	fsimg_sha256_init(&sha);
	while (offset < entry->size) {
		CHECK(fsimg_reader_next_range(reader, offset, &range,
		    &error) == FSIMG_OK);
		CHECK(range.offset == offset && range.length != 0);
		if (range.length > entry->size - offset)
			range.length = entry->size - offset;
		for (uint64_t left = range.length; left != 0;) {
			size_t batch = left > READ_CHUNK ? READ_CHUNK :
			    (size_t)left, got;

			if (range.kind == FSIMG_RANGE_ZERO) {
				fsimg_sha256_update(&sha, zeros, batch);
				figures->zero_bytes += batch;
			} else {
				CHECK(fsimg_reader_read_at(reader, offset, buffer,
				    batch, &got, &error) == FSIMG_OK &&
				    got == batch);
				fsimg_sha256_update(&sha, buffer, batch);
				figures->data_bytes += batch;
			}
			offset += batch;
			left -= batch;
		}
	}
	fsimg_sha256_final(digest, &sha);
	for (size_t i = 0; i < sizeof(digest); i++)
		snprintf(hex + 2 * i, 3, "%02x", digest[i]);
}

/* List every entry, reading each regular file against the manifest. */
static fsimg_status_t
tree_read_against_manifest(fsimg_reader_t *reader, manifest_t *manifest,
    figures_t *figures, fsimg_error_t *error)
{
	char hex[FSIMG_SHA256_DIGEST_STRING_LENGTH];
	const fsimg_entry_t *entry;
	unsigned char *buffer;
	uint64_t empty = 0;
	fsimg_status_t status;

	buffer = malloc(READ_CHUNK);
	CHECK(buffer != NULL);
	while ((status = fsimg_reader_next(reader, &entry, error)) ==
	    FSIMG_OK) {
		sized_file_t *sized;

		figures->entries++;
		if (entry->kind != FSIMG_ENTRY_FILE)
			continue;
		figures->files++;
		if (entry->size == 0) {
			empty++;
			continue;
		}
		sized = manifest_take(manifest, entry->path);
		CHECK(entry->size == sized->size);
		content_hash(reader, entry, buffer, figures, hex);
		if (strcmp(hex, sized->digest) != 0) {
			fprintf(stderr, "scale: %s: sha256 %s != %s\n",
			    entry->path, hex, sized->digest);
			exit(1);
		}
	}
	free(buffer);
	if (status != FSIMG_DONE)
		return (status);
	CHECK(figures->entries == manifest->entries);
	CHECK(empty == manifest->empty_files);
	for (size_t i = 0; i < manifest->sized_count; i++)
		CHECK(manifest->sized[i].seen);
	return (FSIMG_OK);
}

/* A 16 MiB context whose scratch directory is the one at path. */
static fsimg_ctx_t *
context_open(const char *path)
{
	fsimg_options_t options;
	fsimg_ctx_t *ctx;
	int scratch;

	scratch = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = SCALE_BUDGET;
	options.scratch_directory_fd = scratch;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	CHECK(close(scratch) == 0);
	return (ctx);
}

/* Free the context once everything the run held has come back. */
static void
context_close(fsimg_ctx_t *ctx, const fsimg_stats_t *baseline)
{
	fsimg_stats_t after;

	CHECK(fsimg_ctx_get_stats(ctx, &after, NULL) == FSIMG_OK);
	CHECK(after.memory_bytes == baseline->memory_bytes);
	CHECK(after.scratch_bytes == 0);
	fsimg_ctx_free(ctx);
}

/*
 * Open the image under the minimum budget and, if it opens, list and read
 * it, timing the operation and sampling the resident set before and after
 * teardown.
 */
static void
fixture_measure(const scale_case_t *c, const char *scratch, volume_t *volume,
    manifest_t *manifest, figures_t *figures)
{
	fsimg_io_ops_t ops;
	fsimg_stats_t baseline;
	fsimg_error_t error = {0};
	fsimg_reader_t *reader = NULL;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_status_t status;
	struct timespec start, end;
	uint64_t rss_baseline;

	ctx = context_open(scratch);
	CHECK(fsimg_ctx_get_stats(ctx, &baseline, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = volume_read;
	ops.get_size = volume_size;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, volume, &io, NULL) ==
	    FSIMG_OK);

	rss_baseline = scale_peak_rss();
	figures->rss = rss_baseline;
	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	status = fsimg_reader_open(ctx, io, NULL, &reader, &error);
	if (status != c->open_status) {
		fprintf(stderr, "scale: %s: open status %d: %s\n", c->name,
		    (int)status, error.message);
		exit(1);
	}
	if (status == FSIMG_OK) {
		status = tree_read_against_manifest(reader, manifest, figures,
		    &error);
		if (status != FSIMG_OK) {
			fprintf(stderr, "scale: %s: status %d: %s\n", c->name,
			    (int)status, error.message);
			exit(1);
		}
	}
	CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	scale_rss_sample(&figures->rss);
	CHECK(fsimg_ctx_get_stats(ctx, &figures->stats, NULL) == FSIMG_OK);
	fsimg_reader_free(reader);
	fsimg_io_free(io);
	scale_rss_sample(&figures->rss);
	figures->rss_growth = figures->rss - rss_baseline;
	figures->seconds = (double)(end.tv_sec - start.tv_sec) +
	    (double)(end.tv_nsec - start.tv_nsec) / 1e9;
	context_close(ctx, &baseline);
}

/* An empty directory once the context has unlinked its scratch files. */
static void
scratch_remove(void)
{
	(void)rmdir(scratch_path);
}

int
main(int argc, char **argv)
{
	const char *corpus = getenv("FSIMG_EROFS_FIXTURES");
	char image[1024], manifest_path[1024];
	const scale_case_t *c;
	manifest_t manifest;
	figures_t figures = {0};
	volume_t volume = {0};
	struct stat st;
	uint64_t memory_bound;

	CHECK(argc == 2);
	if (strcmp(argv[1], "--list") == 0) {
		for (size_t i = 0; i < ARRAY_SIZE(cases); i++)
			printf("%s\n", cases[i].name);
		return (0);
	}
	CHECK((c = case_find(argv[1])) != NULL);
	if (corpus == NULL || corpus[0] == '\0') {
		printf("SKIP: FSIMG_EROFS_FIXTURES is not set\n");
		return (EXIT_SKIP);
	}
	CHECK(snprintf(image, sizeof(image), "%s/%s/image.erofs", corpus,
	    c->name) < (int)sizeof(image));
	CHECK(snprintf(manifest_path, sizeof(manifest_path),
	    "%s/%s/manifest.txt", corpus, c->name) <
	    (int)sizeof(manifest_path));
	volume.fd = open(image, O_RDONLY | O_CLOEXEC);
	if (volume.fd == -1 && errno == ENOENT) {
		printf("SKIP: %s: fixture %s is missing\n", c->name, image);
		return (EXIT_SKIP);
	}
	CHECK(volume.fd != -1);
	CHECK(fstat(volume.fd, &st) == 0 && S_ISREG(st.st_mode));
	volume.length = (uint64_t)st.st_size;
	CHECK(volume.length > 0);
	CHECK(c->length_factor <= (UINT64_MAX - c->slack) / volume.length);
	volume.limit = c->length_factor * volume.length + c->slack;
	manifest_load(manifest_path, &manifest);
	CHECK(mkdtemp(scratch_path) != NULL);
	CHECK(atexit(scratch_remove) == 0);

	fixture_measure(c, scratch_path, &volume, &manifest, &figures);
	CHECK(close(volume.fd) == 0);

	memory_bound = scale_memory(DRIVER_STATE);
	printf("erofs %s apparent=%" PRIu64 " entries=%" PRIu64
	    " files=%" PRIu64 " image_calls=%" PRIu64 " image_bytes=%" PRIu64
	    " image_limit=%" PRIu64 " image_max_end=%" PRIu64
	    " data_bytes=%" PRIu64 " zero_bytes=%" PRIu64 " io_calls=%" PRIu64
	    " memory_peak=%" PRIu64 " memory_bound=%" PRIu64
	    " scratch_peak=%" PRIu64 " rss=%" PRIu64 " rss_growth=%" PRIu64
	    " seconds=%.3f\n", c->name, volume.length, figures.entries,
	    figures.files, volume.calls, volume.bytes, volume.limit,
	    volume.max_end, figures.data_bytes, figures.zero_bytes,
	    figures.stats.io_calls, figures.stats.memory_peak, memory_bound,
	    figures.stats.scratch_peak, figures.rss, figures.rss_growth,
	    figures.seconds);
	CHECK(fflush(stdout) == 0);
	CHECK(figures.stats.memory_peak <= memory_bound);
	CHECK(c->spools == (figures.stats.scratch_peak > 0));
	CHECK(figures.rss_growth <= memory_bound + RSS_GROWTH);
	return (0);
}
