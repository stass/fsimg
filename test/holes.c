/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fsimg.h"
#include "host.h"
#include "internal.h"
#include "io.h"
#include "test.h"

/*
 * Hole knowledge: the
 * written-range record of a fresh file, the zero-range answers of fresh
 * files, slices and callback objects, zeroing that writes only where the
 * record cannot prove zeros, and the transcode sparing the reads of the
 * ranges its disk proves zero with identical output. Each passing case has
 * a seeded failure beside it that proves the code under test ran.
 */
#define KIB		UINT64_C(1024)
#define MIB		(UINT64_C(1024) * 1024)
#define GIB		(UINT64_C(1024) * MIB)
#define TIB		(UINT64_C(1024) * GIB)
#define GRANULE		(4 * KIB)
#define EPOCH		INT64_C(1700000000)

typedef struct counted counted_t;

/*
 * A context over a private directory. The rest drives the hooks that
 * change an input under an operation: victim is a read/write descriptor
 * of the input file. At the query's seek (between its two seeks, the
 * at_seek'th time) or at a cancellation poll once armed, the file is cut
 * to cut bytes; a poll after the cut restores it, data and all, when
 * restore is set. The operation is cancelled once cancel_answered has
 * answered a zero range lookup.
 */
typedef struct fixture {
	fsimg_ctx_t	*ctx;
	uint64_t	 baseline_memory;
	char		 directory[1024];
	int		 directory_fd;
	unsigned	 files;
	int		 victim;
	uint64_t	 cut;
	unsigned	 seeks;
	unsigned	 at_seek;	/* Cut at this seek; zero never */
	bool		 arm_at_seek;	/* Arm a poll cut at that seek instead */
	bool		 armed;
	bool		 cut_done;
	bool		 restore;
	bool		 restored;
	uint64_t	 restore_length;
	uint64_t	 data_offset;	/* Where restore rewrites the data */
	counted_t	*cancel_answered;
	unsigned	 queries;	/* host_query hook calls */
	bool		 lie;		/* host_query claims the file one hole */
	bool		 no_host;	/* host_query reports no SEEK_HOLE */
} fixture_t;

/* How a callback object answers next_zero_range. */
typedef enum claim {
	CLAIM_NONE,		/* No callback */
	CLAIM_ALL,		/* Everything from offset is zero */
	CLAIM_HALF,		/* The first half is zero, the rest is data */
	CLAIM_BEFORE,		/* start below offset */
	CLAIM_PAST_END,		/* end beyond the length */
	CLAIM_NO_PROGRESS	/* An empty answer at offset */
} claim_t;

/* A callback object of length bytes that stores nothing: reads give
 * zeros and are counted, writes and zero ranges are counted, and a
 * resize sets the length.
 */
struct counted {
	uint64_t	 length;
	claim_t		 claim;
	uint64_t	 read;
	uint64_t	 written;
	uint64_t	 zeroed;
	unsigned	 answers;
};

static void fixture_init(fixture_t *, uint64_t);
static void fixture_free(fixture_t *);
static void fixture_cut(fixture_t *);
static void fixture_seek(void *);
static bool fixture_poll(void *);
static int fixture_query(void *, int, uint64_t, uint64_t *, uint64_t *,
    uint64_t *);
static int sparse_file(fixture_t *, const char *, uint64_t, const uint64_t *,
    size_t);
static fsimg_io_t *read_only(fixture_t *, const char *);
static fsimg_status_t publish(fixture_t *, const char *, fsimg_io_t **,
    size_t, bool, counted_t *, const char *, fsimg_error_t *);
static bool exists(fixture_t *, const char *);
static void arm(fixture_t *, int, uint64_t, unsigned, bool);
static bool holds(fixture_t *, const char *, uint64_t, uint64_t,
    unsigned char);
static void remove_files(fixture_t *, const char *const *);
static void test_external_host_report(void);
static void test_shrink_seen_at_lookup(void);
static void test_truncated_input(void);
static void test_undetected_modification(void);
static void test_tail_cost(void);
static void test_cancellation_in_skipped_zeros(void);
static void test_sync_failure_fallback(void);
static void test_false_host_answer(void);
static void test_own_files_never_ask(void);
static void test_borrowed_position(void);
static bool host_reports_holes(void);
static int private_file(fixture_t *, bool);
static fsimg_io_t *fresh_file(fixture_t *, uint64_t);
static fsimg_io_t *external_file(fixture_t *, uint64_t);
static uint64_t stat_of(fixture_t *, size_t);
static void expect_zero_range(fsimg_io_t *, uint64_t, uint64_t, uint64_t);
static void write_byte(fsimg_io_t *, uint64_t, unsigned char);
static rlim_t file_size_limit(rlim_t);
static fsimg_status_t counted_read(void *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t counted_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t counted_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t counted_zero(void *, uint64_t, uint64_t,
    fsimg_error_t *);
static fsimg_status_t counted_next_zero(void *, uint64_t, uint64_t,
    uint64_t *, uint64_t *, fsimg_error_t *);
static fsimg_io_t *counted_io(fixture_t *, counted_t *, bool);
static uint64_t write_disk(fixture_t *, const char *, const char *, uint64_t,
    const char *);
static bool same_files(fixture_t *, const char *, const char *);
static bool same_sparse_files(fixture_t *, const char *, const char *);
static void test_record_marking(void);
static void test_failed_writes_stay_marked(void);
static void test_zeros_after_writes(void);
static void test_zeroing_never_consults_answers(void);
static void test_callback_answers(void);
static void test_fold_and_shrink(void);
static void test_record_growth(void);
static void test_resize_failures(void);
static void test_record_bounds(void);
static void test_transcode_of_fresh_files(void);
static void test_determinism(void);

#define STAT_WRITTEN	offsetof(fsimg_stats_t, written_bytes)
#define STAT_READ	offsetof(fsimg_stats_t, read_bytes)
#define STAT_MEMORY	offsetof(fsimg_stats_t, memory_bytes)

static void
fixture_init(fixture_t *f, uint64_t memory)
{
	fsimg_options_t options;

	bzero(f, sizeof(*f));
	snprintf(f->directory, sizeof(f->directory), "%s/fsimg-holes.XXXXXX",
	    getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	CHECK(mkdtemp(f->directory) != NULL);
	f->directory_fd = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->directory_fd != -1);
	f->victim = -1;
	fsimg_options_init(&options);
	if (memory != 0)
		options.memory_bytes = memory;
	options.scratch_directory_fd = f->directory_fd;
	options.cancelled = fixture_poll;
	options.cancel_argument = f;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	fsimg_test_holes(f->ctx)->argument = f;
	fsimg_test_holes(f->ctx)->between_seeks = fixture_seek;
	f->baseline_memory = stat_of(f, STAT_MEMORY);
}

/* Cut the victim file to f->cut bytes, once. */
static void
fixture_cut(fixture_t *f)
{
	if (f->cut_done)
		return;
	CHECK(ftruncate(f->victim, (off_t)f->cut) == 0);
	f->cut_done = true;
}

/* Between the host query's two seeks: cut, or arm a cut at the next
 * poll, at the configured seek.
 */
static void
fixture_seek(void *argument)
{
	fixture_t *f = argument;

	if (f->victim == -1 || ++f->seeks != f->at_seek)
		return;
	if (f->arm_at_seek)
		f->armed = true;
	else
		fixture_cut(f);
}

/*
 * At each cancellation poll: an armed cut happens; a cut file is restored
 * to restore_length with its 1 MiB of data back at data_offset; and the
 * operation is cancelled once cancel_answered has answered.
 */
static bool
fixture_poll(void *argument)
{
	static unsigned char data[MIB];
	fixture_t *f = argument;

	if (f->armed && f->victim != -1)
		fixture_cut(f);
	if (f->restore && f->cut_done && !f->restored) {
		memset(data, 0x5a, sizeof(data));
		CHECK(ftruncate(f->victim, (off_t)f->restore_length) == 0);
		CHECK(pwrite(f->victim, data, sizeof(data),
		    (off_t)f->data_offset) == (ssize_t)sizeof(data));
		f->restored = true;
	}
	return (f->cancel_answered != NULL && f->cancel_answered->answers != 0);
}

/* Stand-in for the host's query: no interface at all, or one hole over
 * the whole file, data included; otherwise the host's own answer.
 */
static int
fixture_query(void *argument, int fd, uint64_t offset, uint64_t *hole,
    uint64_t *data, uint64_t *observed)
{
	fixture_t *f = argument;
	struct stat st;

	f->queries++;
	if (f->no_host) {
		errno = EOPNOTSUPP;
		return (-1);
	}
	if (!f->lie)
		return (fsimg_host_next_hole(fd, offset, NULL, NULL, hole, data,
		    observed));
	CHECK(fstat(fd, &st) == 0);
	*hole = offset;
	*data = UINT64_MAX;
	*observed = (uint64_t)st.st_size;
	return (0);
}

/* Every charge is returned and no file is left behind. */
static void
fixture_free(fixture_t *f)
{
	struct dirent *entry;
	DIR *directory;

	CHECK(stat_of(f, STAT_MEMORY) == f->baseline_memory);
	fsimg_ctx_free(f->ctx);
	directory = fdopendir(f->directory_fd);
	CHECK(directory != NULL);
	while ((entry = readdir(directory)) != NULL)
		CHECK(strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0);
	CHECK(closedir(directory) == 0);
	CHECK(rmdir(f->directory) == 0);
}

/* A new read/write file in the fixture's directory, unlinked when
 * unlinked is set, as a handed-over file must be.
 */
static int
private_file(fixture_t *f, bool unlinked)
{
	char name[32];
	int fd;

	snprintf(name, sizeof(name), "file%u", f->files++);
	fd = openat(f->directory_fd, name, O_RDWR | O_CREAT | O_EXCL |
	    O_CLOEXEC, 0600);
	CHECK(fd != -1);
	if (unlinked)
		CHECK(unlinkat(f->directory_fd, name, 0) == 0);
	return (fd);
}

/* A fresh file, adopted empty as the spool is, then sized. */
static fsimg_io_t *
fresh_file(fixture_t *f, uint64_t length)
{
	fsimg_io_t *io;

	CHECK(fsimg_io_adopt_handed_over(f->ctx, private_file(f, true), "test",
	    &io, NULL) == FSIMG_OK);
	CHECK(fsimg_io_resize(io, length, NULL) == FSIMG_OK);
	return (io);
}

/* A file the library did not create, as an application hands it over. */
static fsimg_io_t *
external_file(fixture_t *f, uint64_t length)
{
	fsimg_io_t *io;
	int fd;

	fd = private_file(f, true);
	CHECK(ftruncate(fd, (off_t)length) == 0);
	CHECK(fsimg_io_from_fd(f->ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE, &io,
	    NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	return (io);
}

static uint64_t
stat_of(fixture_t *f, size_t field)
{
	fsimg_stats_t stats;
	uint64_t value;

	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	bcopy((unsigned char *)&stats + field, &value, sizeof(value));
	return (value);
}

static void
expect_zero_range(fsimg_io_t *io, uint64_t offset, uint64_t start,
    uint64_t end)
{
	uint64_t found_start, found_end;

	CHECK(fsimg_io_next_zero_range(io, offset, &found_start, &found_end,
	    NULL) == FSIMG_OK);
	if (found_start != start || found_end != end)
		fprintf(stderr, "zero range at %llu: [%llu, %llu), expected "
		    "[%llu, %llu)\n", (unsigned long long)offset,
		    (unsigned long long)found_start,
		    (unsigned long long)found_end, (unsigned long long)start,
		    (unsigned long long)end);
	CHECK(found_start == start && found_end == end);
}

static void
write_byte(fsimg_io_t *io, uint64_t offset, unsigned char value)
{
	size_t transferred;

	CHECK(fsimg_io_write_exact(io, offset, &value, 1, &transferred,
	    NULL) == FSIMG_OK);
}

/* Set the soft file size limit, which fails writes and growth past it,
 * returning the previous one for the caller to restore.
 */
static rlim_t
file_size_limit(rlim_t limit)
{
	struct rlimit current;
	rlim_t previous;

	CHECK(getrlimit(RLIMIT_FSIZE, &current) == 0);
	previous = current.rlim_cur;
	current.rlim_cur = limit;
	CHECK(setrlimit(RLIMIT_FSIZE, &current) == 0);
	return (previous);
}

static fsimg_status_t
counted_read(void *argument, uint64_t offset FSIMG_UNUSED, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	((counted_t *)argument)->read += length;
	bzero(buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
counted_resize(void *argument, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	((counted_t *)argument)->length = length;
	return (FSIMG_OK);
}

static fsimg_status_t
counted_write(void *argument, uint64_t offset FSIMG_UNUSED,
    const void *buffer FSIMG_UNUSED, size_t length, size_t *transferred,
    fsimg_error_t *error FSIMG_UNUSED)
{
	((counted_t *)argument)->written += length;
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
counted_size(void *argument, uint64_t *length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((counted_t *)argument)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
counted_zero(void *argument, uint64_t offset FSIMG_UNUSED, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	((counted_t *)argument)->zeroed += length;
	return (FSIMG_OK);
}

static fsimg_status_t
counted_next_zero(void *argument, uint64_t offset, uint64_t length,
    uint64_t *start, uint64_t *end, fsimg_error_t *error FSIMG_UNUSED)
{
	counted_t *counted = argument;

	counted->answers++;
	*start = offset;
	*end = length;
	switch (counted->claim) {
	case CLAIM_BEFORE:
		*start = offset - 1;
		break;
	case CLAIM_PAST_END:
		*end = length + 1;
		break;
	case CLAIM_NO_PROGRESS:
		*end = offset;
		break;
	case CLAIM_HALF:
		if (offset < length / 2)
			*end = length / 2;
		else
			*start = length;
		break;
	case CLAIM_ALL:
	case CLAIM_NONE:
		break;
	}
	return (FSIMG_OK);
}

static fsimg_io_t *
counted_io(fixture_t *f, counted_t *counted, bool zero_range)
{
	fsimg_io_ops_t ops;
	fsimg_io_t *io;

	fsimg_io_ops_init(&ops);
	ops.read_at = counted_read;
	ops.write_at = counted_write;
	ops.get_size = counted_size;
	ops.resize = counted_resize;
	if (zero_range)
		ops.zero_range = counted_zero;
	if (counted->claim != CLAIM_NONE)
		ops.next_zero_range = counted_next_zero;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, counted, &io, NULL) ==
	    FSIMG_OK);
	return (io);
}

/*
 * Writes through two slices of one fresh file mark its root, so the root
 * answers exactly the runs of never-written granules: a one-byte write
 * makes its whole 4 KiB granule possibly nonzero, and so does a write of
 * zeros, since the record knows what was written, not what it holds. A
 * slice answers through the root, clipped and translated. Zeroing a range
 * disjoint from every write writes nothing; zeroing across a written
 * granule writes only the requested bytes inside it.
 */
static void
test_record_marking(void)
{
	static const unsigned char zeros[GRANULE];
	fsimg_io_t *root, *low, *high;
	uint64_t before;
	size_t transferred;
	fixture_t f;

	fixture_init(&f, 0);
	root = fresh_file(&f, MIB);
	expect_zero_range(root, 0, 0, MIB);
	CHECK(fsimg_io_slice(root, 0, MIB / 2, &low, NULL) == FSIMG_OK);
	CHECK(fsimg_io_slice(root, MIB / 2, MIB / 2, &high, NULL) == FSIMG_OK);
	write_byte(low, 3 * GRANULE + 5, 0x41);
	CHECK(fsimg_io_write_exact(high, 100 * GRANULE, zeros, sizeof(zeros),
	    &transferred, NULL) == FSIMG_OK);
	expect_zero_range(root, 0, 0, 3 * GRANULE);
	expect_zero_range(root, 3 * GRANULE, 4 * GRANULE, MIB / 2 +
	    100 * GRANULE);
	expect_zero_range(root, MIB / 2 + 100 * GRANULE, MIB / 2 +
	    101 * GRANULE, MIB);
	/* Inside a clear run the answer starts at the offset itself. */
	expect_zero_range(root, 7 * GRANULE + 1, 7 * GRANULE + 1, MIB / 2 +
	    100 * GRANULE);
	expect_zero_range(high, 0, 0, 100 * GRANULE);
	expect_zero_range(low, 4 * GRANULE, 4 * GRANULE, MIB / 2);
	expect_zero_range(low, MIB / 2, MIB / 2, MIB / 2);

	before = stat_of(&f, STAT_WRITTEN);
	CHECK(fsimg_io_zero_range(root, 20 * GRANULE, 60 * GRANULE, NULL) ==
	    FSIMG_OK);
	CHECK(stat_of(&f, STAT_WRITTEN) == before);
	CHECK(fsimg_io_zero_range(root, 3 * GRANULE - 100, 300, NULL) ==
	    FSIMG_OK);
	CHECK(stat_of(&f, STAT_WRITTEN) == before + 200);
	fsimg_io_free(low);
	fsimg_io_free(high);
	fsimg_io_free(root);
	fixture_free(&f);
}

/*
 * A write the host fails, and a short one, leave their whole requested
 * range marked: the file size limit fails a write past it outright, and
 * one across it is cut short or, on FreeBSD, failed outright; the bytes
 * on disk show which, and the transfer must report what the host did.
 * The answer then excludes both ranges and zeroing them writes zeros over
 * every requested byte.
 */
static void
test_failed_writes_stay_marked(void)
{
	static const unsigned char ones[3 * GRANULE] = { 1 };
	fsimg_io_t *root;
	uint64_t before;
	size_t transferred;
	rlim_t limit;
	fixture_t f;
	size_t landed;
	unsigned char byte;

	fixture_init(&f, 0);
	root = fresh_file(&f, MIB);
	limit = file_size_limit(512 * KIB);
	CHECK(fsimg_io_write_exact(root, 600 * KIB, ones, GRANULE,
	    &transferred, NULL) == FSIMG_EIO && transferred == 0);
	CHECK(fsimg_io_write_exact(root, 512 * KIB - 100, ones, sizeof(ones),
	    &transferred, NULL) == FSIMG_EIO);
	file_size_limit(limit);
	CHECK(fsimg_io_read_exact(root, 512 * KIB - 100, &byte, 1, &landed,
	    NULL) == FSIMG_OK);
	CHECK(transferred == (byte == ones[0] ? 100 : 0));
	expect_zero_range(root, 0, 0, 512 * KIB - GRANULE);
	expect_zero_range(root, 512 * KIB - GRANULE, 512 * KIB + 3 * GRANULE,
	    600 * KIB);
	expect_zero_range(root, 600 * KIB, 600 * KIB + GRANULE, MIB);
	before = stat_of(&f, STAT_WRITTEN);
	CHECK(fsimg_io_zero_range(root, 512 * KIB - 100, sizeof(ones), NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_io_zero_range(root, 600 * KIB, GRANULE, NULL) == FSIMG_OK);
	CHECK(stat_of(&f, STAT_WRITTEN) == before + sizeof(ones) + GRANULE);
	fsimg_io_free(root);
	fixture_free(&f);
}

/*
 * A table written at the end of a fresh disk, then the gap before it
 * zeroed: the context's written bytes count the table alone. The same
 * sequence on a file the library did not create writes every zero byte,
 * since nothing proves its contents.
 */
static void
test_zeros_after_writes(void)
{
	static const unsigned char table[16 * KIB] = { 0x45, 0x46, 0x49 };
	fsimg_io_t *disk;
	uint64_t before;
	size_t transferred;
	fixture_t f;

	fixture_init(&f, 0);
	for (unsigned fresh = 0; fresh < 2; fresh++) {
		disk = fresh ? fresh_file(&f, 64 * MIB) :
		    external_file(&f, 64 * MIB);
		before = stat_of(&f, STAT_WRITTEN);
		CHECK(fsimg_io_write_exact(disk, 64 * MIB - sizeof(table), table,
		    sizeof(table), &transferred, NULL) == FSIMG_OK);
		CHECK(fsimg_io_zero_range(disk, 0, 64 * MIB - sizeof(table),
		    NULL) == FSIMG_OK);
		CHECK(stat_of(&f, STAT_WRITTEN) - before == (fresh ?
		    sizeof(table) : 64 * MIB));
		fsimg_io_free(disk);
	}
	fixture_free(&f);
}

/*
 * Zeroing never takes an object's word for its zeros: a callback sink
 * whose next_zero_range claims every byte zero is zeroed in full, through
 * its zero-range callback or with written zeros, and zeroing asks the
 * callback nothing; a reused file whose host report, through the query
 * hook, would claim the whole file one hole is zeroed in full, and its
 * writable object asks the host nothing at all. Read-side lookups, on the
 * callback object and on a read-only object over the same file, ask them,
 * proving both are live.
 */
static void
test_zeroing_never_consults_answers(void)
{
	const uint64_t whole[] = { 0 };
	counted_t counted;
	fsimg_io_t *io, *reader;
	uint64_t before;
	int fd;
	fixture_t f;

	fixture_init(&f, 0);
	for (unsigned zero_range = 0; zero_range < 2; zero_range++) {
		counted = (counted_t){ .length = 4 * MIB, .claim = CLAIM_ALL };
		io = counted_io(&f, &counted, zero_range);
		CHECK(fsimg_io_zero_range(io, 0, 4 * MIB, NULL) == FSIMG_OK);
		CHECK(counted.zeroed + counted.written == 4 * MIB);
		CHECK(counted.answers == 0);
		expect_zero_range(io, 0, 0, 4 * MIB);
		CHECK(counted.answers == 1);
		fsimg_io_free(io);
	}

	fsimg_test_holes(f.ctx)->host_query = fixture_query;
	f.lie = true;
	fd = sparse_file(&f, "reused", 4 * MIB, whole, 1);
	CHECK(fsimg_io_from_fd(f.ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE, &io,
	    NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	before = stat_of(&f, STAT_WRITTEN);
	CHECK(fsimg_io_zero_range(io, 0, 4 * MIB, NULL) == FSIMG_OK);
	CHECK(stat_of(&f, STAT_WRITTEN) - before == 4 * MIB);
	expect_zero_range(io, 0, 4 * MIB, 4 * MIB);
	CHECK(f.queries == 0 && fsimg_test_holes(f.ctx)->discovery_syncs == 0);
	reader = read_only(&f, "reused");
	expect_zero_range(reader, 0, 0, 4 * MIB - 1);
	CHECK(f.queries == 1);
	fsimg_io_free(reader);
	fsimg_io_free(io);
	remove_files(&f, (const char *const[]){ "reused", NULL });
	fixture_free(&f);
}

/*
 * A callback's answer is checked for shape and trusted for contents: an
 * answer below the offset, past the length or making no progress is
 * FSIMG_EIO naming the inconsistent answer; a slice of the object asks in
 * the object's coordinates and receives the answer clipped to itself. A
 * callback requires read_at, since it describes readable contents.
 */
static void
test_callback_answers(void)
{
	static const claim_t broken[] = { CLAIM_BEFORE, CLAIM_PAST_END,
	    CLAIM_NO_PROGRESS };
	fsimg_io_ops_t ops;
	fsimg_error_t error;
	counted_t counted;
	fsimg_io_t *io, *slice;
	uint64_t start, end;
	fixture_t f;

	fixture_init(&f, 0);
	counted = (counted_t){ .length = MIB, .claim = CLAIM_ALL };
	io = counted_io(&f, &counted, false);
	CHECK(fsimg_io_slice(io, 64 * KIB, 128 * KIB, &slice, NULL) == FSIMG_OK);
	expect_zero_range(slice, 16, 16, 128 * KIB);
	fsimg_io_free(slice);
	for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); i++) {
		counted.claim = broken[i];
		CHECK(fsimg_io_next_zero_range(io, 4096, &start, &end, &error) ==
		    FSIMG_EIO);
		CHECK(strstr(error.message, "inconsistent callback answer") !=
		    NULL);
	}
	fsimg_io_free(io);

	fsimg_io_ops_init(&ops);
	ops.write_at = counted_write;
	ops.get_size = counted_size;
	ops.next_zero_range = counted_next_zero;
	CHECK(fsimg_io_from_callbacks(f.ctx, &ops, &counted, &io, NULL) ==
	    FSIMG_EINVAL);
	fixture_free(&f);
}

/*
 * A sparse 40 GiB fresh file needs more than 2^23 granules of 4 KiB, so
 * its record folds to 8 KiB granules: a pair whose odd half alone was
 * written reads possibly nonzero as a whole, and a clear pair reads
 * clear. An unaligned shrink keeps the granule it cuts, and its written
 * byte, while a wholly removed granule is cleared, so the regrown file
 * answers zero right after the cut granule. A host without files that
 * large skips the case and says so.
 */
static void
test_fold_and_shrink(void)
{
	const uint64_t pair = 2 * GRANULE;
	fsimg_io_t *root;
	unsigned char byte;
	unsigned shift;
	size_t bytes, transferred;
	fixture_t f;

	fixture_init(&f, 0);
	root = fresh_file(&f, 0);
	if (fsimg_io_resize(root, 32 * GIB, NULL) != FSIMG_OK) {
		puts("SKIP: host refuses a 32 GiB sparse file; fold untested");
		fsimg_io_free(root);
		fixture_free(&f);
		return;
	}
	fsimg_test_io_record(root, &shift, &bytes);
	CHECK(shift == 12 && bytes == MIB);
	write_byte(root, 21 * GRANULE + 7, 0x41);	/* odd half of pair 10 */
	write_byte(root, 40 * GRANULE, 0x42);		/* even half of pair 20 */
	write_byte(root, 60 * GRANULE, 0x43);		/* pair 30 */
	if (fsimg_io_resize(root, 40 * GIB, NULL) != FSIMG_OK) {
		puts("SKIP: host refuses a 40 GiB sparse file; fold untested");
		fsimg_io_free(root);
		fixture_free(&f);
		return;
	}
	fsimg_test_io_record(root, &shift, &bytes);
	CHECK(shift == 13 && bytes == 40 * GIB / pair / 8);
	expect_zero_range(root, 0, 0, 10 * pair);
	expect_zero_range(root, 10 * pair, 11 * pair, 20 * pair);
	expect_zero_range(root, 20 * pair, 21 * pair, 30 * pair);

	/* Cut pair 20 after its written byte, which removes pair 30. */
	CHECK(fsimg_io_resize(root, 20 * pair + 50, NULL) == FSIMG_OK);
	CHECK(fsimg_io_read_exact(root, 20 * pair, &byte, 1, &transferred,
	    NULL) == FSIMG_OK && byte == 0x42);
	CHECK(fsimg_io_resize(root, MIB, NULL) == FSIMG_OK);
	expect_zero_range(root, 20 * pair, 21 * pair, MIB);
	fsimg_io_free(root);
	fixture_free(&f);
}

/*
 * A file grown a granule at a time, as the spool grows by appends, keeps
 * a record whose capacity doubles: growing to one granule past 64 MiB
 * leaves room for 128 MiB rather than exactly what the length needs, so
 * the record is reallocated a logarithmic number of times, not once per
 * append. The growth keeps the marked granules.
 */
static void
test_record_growth(void)
{
	fsimg_io_t *root;
	unsigned shift;
	size_t bytes;
	fixture_t f;

	fixture_init(&f, 0);
	root = fresh_file(&f, 0);
	for (uint64_t length = GRANULE; length <= 64 * MIB + GRANULE;
	    length += GRANULE) {
		CHECK(fsimg_io_resize(root, length, NULL) == FSIMG_OK);
		if (length == GRANULE)
			write_byte(root, 0, 0x41);
	}
	fsimg_test_io_record(root, &shift, &bytes);
	CHECK(shift == 12 && bytes == 128 * MIB / GRANULE / 8);
	expect_zero_range(root, 0, GRANULE, 64 * MIB + GRANULE);
	fsimg_io_free(root);
	fixture_free(&f);
}

/*
 * A resize that cannot complete leaves the record and the file as they
 * were and the object usable: an allocation failure while the folded
 * record is prepared fails before the host is touched, and a host
 * truncation failure injected after the record was prepared discards the
 * prepared record. The failure is injected because FreeBSD's ftruncate
 * ignores the file size limit.
 */
static void
test_resize_failures(void)
{
	fsimg_io_t *root;
	struct stat st;
	fsimg_error_t error;
	unsigned shift;
	size_t bytes;
	int fd, observer;
	fixture_t f;

	fixture_init(&f, 0);
	fd = private_file(&f, true);
	/* The test keeps a descriptor only to observe the file's length. */
	observer = dup(fd);
	CHECK(observer != -1);
	CHECK(fsimg_io_adopt_handed_over(f.ctx, fd, "test", &root, NULL) ==
	    FSIMG_OK);
	if (fsimg_io_resize(root, 32 * GIB, NULL) != FSIMG_OK) {
		puts("SKIP: host refuses a 32 GiB sparse file; resize untested");
		CHECK(close(observer) == 0);
		fsimg_io_free(root);
		fixture_free(&f);
		return;
	}
	write_byte(root, 5 * GRANULE, 0x41);

	fsimg_test_fail_alloc_after(f.ctx, 1);
	CHECK(fsimg_io_resize(root, 64 * GIB, NULL) == FSIMG_ENOMEM);
	fsimg_test_fail_alloc_after(f.ctx, 0);
	fsimg_test_io_record(root, &shift, &bytes);
	CHECK(shift == 12 && bytes == MIB);

	fsimg_test_holes(f.ctx)->resize_errno = EFBIG;
	CHECK(fsimg_io_resize(root, 64 * GIB, &error) == FSIMG_EIO &&
	    error.os_error == EFBIG);
	fsimg_test_holes(f.ctx)->resize_errno = 0;
	fsimg_test_io_record(root, &shift, &bytes);
	CHECK(shift == 12 && bytes == MIB);
	CHECK(stat_of(&f, STAT_MEMORY) <= f.baseline_memory + 2 * MIB);
	CHECK(fstat(observer, &st) == 0 && (uint64_t)st.st_size == 32 * GIB);
	expect_zero_range(root, 0, 0, 5 * GRANULE);
	expect_zero_range(root, 5 * GRANULE, 6 * GRANULE, 32 * GIB);
	write_byte(root, 7 * GRANULE, 0x42);
	expect_zero_range(root, 6 * GRANULE, 6 * GRANULE, 7 * GRANULE);
	CHECK(close(observer) == 0);
	fsimg_io_free(root);
	fixture_free(&f);
}

/*
 * A 128 TiB fresh file's record is at most 1 MiB of charged memory, at a
 * 16 MiB granule, and the file answers its unwritten ranges zero in a
 * bounded scan. A host without files that large skips the case.
 */
static void
test_record_bounds(void)
{
	fsimg_io_t *root;
	uint64_t before;
	unsigned shift;
	size_t bytes;
	fixture_t f;

	fixture_init(&f, 0);
	root = fresh_file(&f, 0);
	before = stat_of(&f, STAT_MEMORY);
	if (fsimg_io_resize(root, 128 * TIB, NULL) != FSIMG_OK) {
		puts("SKIP: host refuses a 128 TiB sparse file; bound untested");
		fsimg_io_free(root);
		fixture_free(&f);
		return;
	}
	fsimg_test_io_record(root, &shift, &bytes);
	CHECK(shift == 24 && bytes == MIB);
	CHECK(stat_of(&f, STAT_MEMORY) - before <= MIB + 64);
	write_byte(root, 64 * TIB, 0x41);
	expect_zero_range(root, 0, 0, 64 * TIB);
	expect_zero_range(root, 64 * TIB, 64 * TIB + 16 * MIB, 128 * TIB);
	fsimg_io_free(root);
	fixture_free(&f);
}

/*
 * Write an empty volume of volume_bytes under stack, with the compression
 * named (or none), to the file name in the fixture's directory through a
 * raw intermediate, and return the bytes the context read doing it.
 */
static uint64_t
write_disk(fixture_t *f, const char *stack, const char *compression,
    uint64_t volume_bytes, const char *name)
{
	fsimg_write_options_t options;
	fsimg_layer_options_t layers;
	fsimg_writer_t *writer;
	fsimg_sink_t sink;
	uint64_t before;

	fsimg_layer_options_init(&layers);
	fsimg_write_options_init(&options);
	options.flags = FSIMG_WRITE_REPRODUCIBLE;
	options.epoch = EPOCH;
	options.layers = &layers;
	options.volume_bytes = volume_bytes;
	snprintf(options.compression, sizeof(options.compression), "%s",
	    compression);
	CHECK(fsimg_writer_new(f->ctx, stack, &options, &writer, NULL) ==
	    FSIMG_OK);
	fsimg_sink_init(&sink, FSIMG_SINK_FILE);
	sink.parent_fd = f->directory_fd;
	sink.name = name;
	/* An encoded output is staged raw first; a plain disk is not. */
	if (compression[0] != '\0' || strstr(stack, "dmg") != NULL ||
	    strstr(stack, "vhd") != NULL || strstr(stack, "simg") != NULL)
		sink.raw_fd = private_file(f, true);
	before = stat_of(f, STAT_READ);
	CHECK(fsimg_writer_output(writer, &sink, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	return (stat_of(f, STAT_READ) - before);
}

static bool
same_files(fixture_t *f, const char *left, const char *right)
{
	unsigned char a[64 * 1024], b[64 * 1024];
	ssize_t got, other;
	int one, two;
	bool equal;

	one = openat(f->directory_fd, left, O_RDONLY | O_CLOEXEC);
	two = openat(f->directory_fd, right, O_RDONLY | O_CLOEXEC);
	CHECK(one != -1 && two != -1);
	equal = true;
	do {
		got = read(one, a, sizeof(a));
		other = read(two, b, sizeof(b));
		CHECK(got >= 0 && other >= 0);
		if (got != other || bcmp(a, b, (size_t)got) != 0)
			equal = false;
	} while (equal && got > 0);
	CHECK(close(one) == 0 && close(two) == 0);
	return (equal);
}

/*
 * Whether two files written the same way are equal, reading only their
 * data: the host must report the same holes in both, and the bytes
 * between them must match. A 100 GiB image is compared in seconds.
 */
static bool
same_sparse_files(fixture_t *f, const char *left, const char *right)
{
	unsigned char a[64 * 1024], b[64 * 1024];
	uint64_t offset, hole[2], data[2], size[2];
	size_t step;
	int one, two;
	bool equal;

	one = openat(f->directory_fd, left, O_RDONLY | O_CLOEXEC);
	two = openat(f->directory_fd, right, O_RDONLY | O_CLOEXEC);
	CHECK(one != -1 && two != -1);
	equal = true;
	for (offset = 0; equal; offset = data[0]) {
		CHECK(fsimg_host_next_hole(one, offset, NULL, NULL, &hole[0],
		    &data[0], &size[0]) == 0);
		CHECK(fsimg_host_next_hole(two, offset, NULL, NULL, &hole[1],
		    &data[1], &size[1]) == 0);
		hole[0] = hole[0] < size[0] ? hole[0] : size[0];
		data[0] = data[0] < size[0] ? data[0] : size[0];
		equal = size[0] == size[1] && hole[0] == (hole[1] < size[1] ?
		    hole[1] : size[1]) && data[0] == (data[1] < size[1] ?
		    data[1] : size[1]);
		for (; equal && offset < hole[0]; offset += step) {
			step = hole[0] - offset < sizeof(a) ?
			    (size_t)(hole[0] - offset) : sizeof(a);
			CHECK(pread(one, a, step, (off_t)offset) == (ssize_t)step);
			CHECK(pread(two, b, step, (off_t)offset) == (ssize_t)step);
			equal = bcmp(a, b, step) == 0;
		}
		if (data[0] == size[0])
			break;
	}
	CHECK(close(one) == 0 && close(two) == 0);
	return (equal);
}

/*
 * fsimg's own files answer from their records. A 1 GiB FAT32 volume
 * staged in its raw intermediate and encoded as a DMG reads only the
 * chunks its emission wrote, and a sparse raw image spooled from gzip
 * and unwrapped reads only the chunks the spool wrote. With hole
 * knowledge ignored both read the whole disk, and the outputs are
 * identical.
 */
static void
test_transcode_of_fresh_files(void)
{
	fsimg_image_options_t open_options;
	fsimg_image_t *image;
	fsimg_sink_t sink;
	fsimg_io_t *input;
	uint64_t consulted, ignored, before;
	int fd;
	fixture_t f;

	fixture_init(&f, 0);
	consulted = write_disk(&f, "fat32/dmg", "", GIB, "consulted.dmg");
	fsimg_test_hole_discovery(f.ctx, false);
	ignored = write_disk(&f, "fat32/dmg", "", GIB, "ignored.dmg");
	fsimg_test_hole_discovery(f.ctx, true);
	CHECK(consulted < 64 * MIB && ignored >= GIB);
	CHECK(same_files(&f, "consulted.dmg", "ignored.dmg"));

	write_disk(&f, "fat32", "gzip", GIB, "volume.gz");
	for (unsigned consult = 0; consult < 2; consult++) {
		fd = openat(f.directory_fd, "volume.gz", O_RDONLY | O_CLOEXEC);
		CHECK(fd != -1);
		CHECK(fsimg_io_from_fd(f.ctx, fd, FSIMG_IO_READ, &input, NULL) ==
		    FSIMG_OK);
		CHECK(close(fd) == 0);
		fsimg_image_options_init(&open_options);
		open_options.spool_fd = private_file(&f, true);
		CHECK(fsimg_image_open(f.ctx, input, &open_options, &image, NULL) ==
		    FSIMG_OK);
		fsimg_test_hole_discovery(f.ctx, consult == 1);
		fsimg_sink_init(&sink, FSIMG_SINK_FILE);
		sink.parent_fd = f.directory_fd;
		sink.name = consult ? "consulted.raw" : "ignored.raw";
		before = stat_of(&f, STAT_READ);
		CHECK(fsimg_image_unwrap(image, &(fsimg_selector_t){ .kind =
		    FSIMG_SELECT_DISK }, NULL, &sink, NULL) == FSIMG_OK);
		if (consult)
			consulted = stat_of(&f, STAT_READ) - before;
		else
			ignored = stat_of(&f, STAT_READ) - before;
		fsimg_image_free(image);
		fsimg_io_free(input);
	}
	CHECK(consulted < 64 * MIB && ignored >= GIB);
	CHECK(same_files(&f, "consulted.raw", "ignored.raw"));
	/* fsimg's own files were never synced to ask the host about holes. */
	CHECK(fsimg_test_holes(f.ctx)->discovery_syncs == 0);
	for (const char *const *name = (const char *const[]){ "consulted.dmg",
	    "ignored.dmg", "volume.gz", "consulted.raw", "ignored.raw", NULL };
	    *name != NULL; name++)
		CHECK(unlinkat(f.directory_fd, *name, 0) == 0);
	fixture_free(&f);
}

/*
 * Output bytes do not depend on hole knowledge: every container and gzip
 * write the same image with it consulted and ignored, under a small
 * memory budget and a large one. An image of a different volume size
 * differs, so the comparison can fail.
 */
static void
test_determinism(void)
{
	static const char *const stacks[] = { "fat12/gpt", "fat12/gpt/dmg",
	    "fat12/gpt/vhd", "fat12/gpt/simg", "fat12/mbr/dmg" };
	static const uint64_t budgets[] = { 64 * MIB, GIB };
	char name[2][32];
	fixture_t f;

	for (size_t b = 0; b < sizeof(budgets) / sizeof(budgets[0]); b++) {
		fixture_init(&f, budgets[b]);
		for (size_t s = 0; s < sizeof(stacks) / sizeof(stacks[0]); s++) {
			for (unsigned gzip = 0; gzip < 2; gzip++) {
				for (unsigned consult = 0; consult < 2; consult++) {
					snprintf(name[consult], sizeof(name[0]),
					    "image%u", consult);
					fsimg_test_hole_discovery(f.ctx,
					    consult == 1);
					write_disk(&f, stacks[s], gzip ? "gzip" :
					    "", 8 * MIB, name[consult]);
				}
				fsimg_test_hole_discovery(f.ctx, true);
				CHECK(same_files(&f, name[0], name[1]));
				CHECK(unlinkat(f.directory_fd, name[0], 0) == 0);
				CHECK(unlinkat(f.directory_fd, name[1], 0) == 0);
			}
		}
		write_disk(&f, "fat12/gpt", "", 8 * MIB, "eight");
		write_disk(&f, "fat12/gpt", "", 9 * MIB, "nine");
		CHECK(!same_files(&f, "eight", "nine"));
		CHECK(unlinkat(f.directory_fd, "eight", 0) == 0);
		CHECK(unlinkat(f.directory_fd, "nine", 0) == 0);
		fixture_free(&f);
	}
}

/*
 * A sparse file of length bytes in the fixture's directory, holding 1 MiB
 * of 0x5a at each of the count data offsets and holes elsewhere. Returns
 * a read/write descriptor of it, which hooks use to change it. Tests keep
 * holes of 16 MiB or more: APFS fills smaller holes when the file is
 * synced, and the discovery sync would then find none.
 */
static int
sparse_file(fixture_t *f, const char *name, uint64_t length,
    const uint64_t *data, size_t count)
{
	static unsigned char bytes[MIB];
	int fd;

	memset(bytes, 0x5a, sizeof(bytes));
	fd = openat(f->directory_fd, name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
	    0600);
	CHECK(fd != -1 && ftruncate(fd, (off_t)length) == 0);
	for (size_t i = 0; i < count; i++)
		CHECK(pwrite(fd, bytes, sizeof(bytes), (off_t)data[i]) ==
		    (ssize_t)sizeof(bytes));
	return (fd);
}

/* A read-only object over the named file, as a reader's input is. */
static fsimg_io_t *
read_only(fixture_t *f, const char *name)
{
	fsimg_io_t *io;
	int fd;

	fd = openat(f->directory_fd, name, O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(fsimg_io_from_fd(f->ctx, fd, FSIMG_IO_READ, &io, NULL) ==
	    FSIMG_OK);
	CHECK(close(fd) == 0);
	return (io);
}

/*
 * Put the inputs, typed as Linux data, under stack: one input by wrap, or
 * any number by compose when compose is set. The output goes to sink
 * when one is given, else it is published as name in the directory.
 */
static fsimg_status_t
publish(fixture_t *f, const char *stack, fsimg_io_t **inputs, size_t count,
    bool compose, counted_t *sink, const char *name, fsimg_error_t *error)
{
	fsimg_write_options_t options;
	fsimg_layer_options_t layers;
	fsimg_part_t parts[4];
	fsimg_sink_t out;
	fsimg_status_t status;

	CHECK(count <= 4 && (compose || count == 1));
	fsimg_layer_options_init(&layers);
	fsimg_write_options_init(&options);
	options.flags = FSIMG_WRITE_REPRODUCIBLE;
	options.epoch = EPOCH;
	fsimg_sink_init(&out, sink != NULL ? FSIMG_SINK_IO : FSIMG_SINK_FILE);
	if (sink != NULL)
		out.io = counted_io(f, sink, true);
	out.parent_fd = f->directory_fd;
	out.name = name;
	if (compose) {
		for (size_t i = 0; i < count; i++)
			parts[i] = (fsimg_part_t){ .volume = inputs[i],
			    .type = "linux" };
		options.layers = &layers;
		status = fsimg_image_compose(f->ctx, stack, parts, count,
		    &options, &out, error);
	} else {
		snprintf(layers.gpt_type, sizeof(layers.gpt_type), "linux");
		options.layers = &layers;
		status = fsimg_image_wrap(f->ctx, inputs[0], stack, &options,
		    &out, error);
	}
	fsimg_io_free(out.io);
	return (status);
}

static bool
exists(fixture_t *f, const char *name)
{
	struct stat st;

	return (fstatat(f->directory_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0);
}

/* Whether length bytes at offset of the named file all hold value. */
static bool
holds(fixture_t *f, const char *name, uint64_t offset, uint64_t length,
    unsigned char value)
{
	unsigned char *bytes;
	bool match;
	int fd;

	bytes = malloc((size_t)length);
	CHECK(bytes != NULL);
	fd = openat(f->directory_fd, name, O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(pread(fd, bytes, (size_t)length, (off_t)offset) ==
	    (ssize_t)length);
	CHECK(close(fd) == 0);
	match = true;
	for (uint64_t i = 0; i < length; i++)
		match = match && bytes[i] == value;
	free(bytes);
	return (match);
}

static void
remove_files(fixture_t *f, const char *const *names)
{
	for (; *names != NULL; names++)
		CHECK(unlinkat(f->directory_fd, *names, 0) == 0);
}

/* Reset the hooks to cut the victim to cut bytes at the at_seek'th host
 * query, or at the poll after it when armed is set.
 */
static void
arm(fixture_t *f, int victim, uint64_t cut, unsigned at_seek, bool armed)
{
	f->victim = victim;
	f->cut = cut;
	f->seeks = 0;
	f->at_seek = at_seek;
	f->arm_at_seek = armed;
	f->armed = f->cut_done = f->restore = f->restored = false;
}

/*
 * A sparse read-only file fsimg did not write is wrapped reading only its
 * two data chunks and its final chunk, which the tail rule keeps out of
 * every answer, with one discovery sync before its first lookup however
 * many lookups follow; compose syncs and reads each input the same way.
 * A writable file the output is written to is never synced for
 * discovery. With hole knowledge ignored the whole file is read, and the
 * image is the same.
 */
static void
test_external_host_report(void)
{
	const uint64_t data[] = { 0, 32 * MIB };
	fsimg_hole_test_t *test;
	fsimg_io_t *inputs[2], *sink;
	uint64_t reads[2], syncs[2], before;
	fixture_t f;

	fixture_init(&f, 0);
	test = fsimg_test_holes(f.ctx);
	CHECK(close(sparse_file(&f, "input", 64 * MIB, data, 2)) == 0);
	for (unsigned consult = 0; consult < 2; consult++) {
		inputs[0] = read_only(&f, "input");
		fsimg_test_hole_discovery(f.ctx, consult == 1);
		before = stat_of(&f, STAT_READ);
		syncs[consult] = test->discovery_syncs;
		CHECK(publish(&f, "gpt", inputs, 1, false, NULL, consult ?
		    "consulted" : "ignored", NULL) == FSIMG_OK);
		reads[consult] = stat_of(&f, STAT_READ) - before;
		syncs[consult] = test->discovery_syncs - syncs[consult];
		fsimg_io_free(inputs[0]);
	}
	/*
	 * read_bytes counts an input byte at the file and again at the
	 * assembled disk; the tables add about 1 MiB. Three chunks of the
	 * file are read, so the whole of it is not.
	 */
	CHECK(reads[1] < 8 * MIB && reads[0] >= 2 * 64 * MIB);
	CHECK(syncs[1] == 1 && syncs[0] == 0 && reads[0] >= 64 * MIB);
	CHECK(same_files(&f, "consulted", "ignored"));

	inputs[0] = read_only(&f, "input");
	inputs[1] = read_only(&f, "input");
	before = stat_of(&f, STAT_READ);
	syncs[0] = test->discovery_syncs;
	CHECK(publish(&f, "gpt", inputs, 2, true, NULL, "composed", NULL) ==
	    FSIMG_OK);
	CHECK(stat_of(&f, STAT_READ) - before < 16 * MIB);
	CHECK(test->discovery_syncs - syncs[0] == 2);
	fsimg_io_free(inputs[1]);

	/* A sink fsimg did not create is written, never synced to ask. */
	sink = external_file(&f, 0);
	syncs[0] = test->discovery_syncs;
	{
		fsimg_write_options_t options;
		fsimg_layer_options_t layers;
		fsimg_sink_t out;

		fsimg_layer_options_init(&layers);
		snprintf(layers.gpt_type, sizeof(layers.gpt_type), "linux");
		fsimg_write_options_init(&options);
		options.flags = FSIMG_WRITE_REPRODUCIBLE;
		options.epoch = EPOCH;
		options.layers = &layers;
		fsimg_sink_init(&out, FSIMG_SINK_IO);
		out.io = sink;
		CHECK(fsimg_image_wrap(f.ctx, inputs[0], "gpt", &options, &out,
		    NULL) == FSIMG_OK);
	}
	CHECK(test->discovery_syncs - syncs[0] == 0);
	fsimg_io_free(sink);
	fsimg_io_free(inputs[0]);
	remove_files(&f, (const char *const[]){ "input", "consulted", "ignored",
	    "composed", NULL });
	fixture_free(&f);
}

/*
 * A file observed shorter than declared fails the lookup that sees it,
 * naming the input, even when it is restored, data and all, before the
 * copy would reach the range the answer covered: the cut lands between
 * the query's two seeks, so SEEK_DATA answers ENXIO, and a poll restores
 * it. With the length check disabled the same case publishes zeros over
 * the restored data, proving the check is what catches it. A file cut
 * before the first lookup fails the same way.
 */
static void
test_shrink_seen_at_lookup(void)
{
	const uint64_t data[] = { 32 * MIB };
	fsimg_error_t error;
	fsimg_io_t *input;
	int victim;
	fixture_t f;

	fixture_init(&f, 0);
	victim = sparse_file(&f, "input", 64 * MIB, data, 1);
	input = read_only(&f, "input");
	CHECK(publish(&f, "gpt", &input, 1, false, NULL, "intact", NULL) ==
	    FSIMG_OK);
	CHECK(holds(&f, "intact", 33 * MIB, MIB, 0x5a));
	fsimg_io_free(input);

	for (unsigned checked = 0; checked < 2; checked++) {
		fsimg_test_holes(f.ctx)->no_length_check = checked == 0;
		arm(&f, victim, 4 * MIB, 1, false);
		f.restore = true;
		f.restore_length = 64 * MIB;
		f.data_offset = 32 * MIB;
		input = read_only(&f, "input");
		if (checked) {
			CHECK(publish(&f, "gpt", &input, 1, false, NULL, "cut",
			    &error) == FSIMG_ESOURCE_CHANGED);
			CHECK(strcmp(error.path, "input 1") == 0 &&
			    !exists(&f, "cut"));
			fixture_poll(&f);
		} else {
			CHECK(publish(&f, "gpt", &input, 1, false, NULL, "cut",
			    NULL) == FSIMG_OK);
			CHECK(f.restored && holds(&f, "cut", 33 * MIB, MIB, 0));
			remove_files(&f, (const char *const[]){ "cut", NULL });
		}
		fsimg_io_free(input);
	}

	arm(&f, -1, 0, 0, false);
	input = read_only(&f, "input");
	CHECK(ftruncate(victim, 4 * MIB) == 0);
	CHECK(publish(&f, "gpt", &input, 1, false, NULL, "cut", &error) ==
	    FSIMG_ESOURCE_CHANGED && !exists(&f, "cut"));
	fsimg_io_free(input);
	CHECK(close(victim) == 0);
	remove_files(&f, (const char *const[]){ "input", "intact", NULL });
	fixture_free(&f);
}

/*
 * The tail rule: a file cut after its lookup, before the chunk holding
 * its last byte is read, fails at that exact read and publishes nothing,
 * for the whole file, a prefix slice of it, the first of two compose
 * inputs, and under simg in each of its two passes. With the tail clip
 * disabled the first three cases publish, zeros standing in for the lost
 * tail, and the simg counting-pass case fails at the writing pass's
 * lookup of the still-short input instead. A
 * compose input whose end falls inside a chunk fails at that chunk with or
 * without the clip, since only a wholly covered chunk is skipped; accepting
 * partial coverage makes it publish.
 */
static void
test_truncated_input(void)
{
	const uint64_t data[] = { 0 };
	fsimg_hole_test_t *test;
	fsimg_io_t *file, *inputs[2];
	counted_t sink;
	fsimg_status_t status;
	int victim, other;
	fixture_t f;

	fixture_init(&f, 0);
	test = fsimg_test_holes(f.ctx);
	victim = sparse_file(&f, "input", 1264 * MIB, data, 1);
	other = sparse_file(&f, "other", 64 * MIB, data, 1);
	CHECK(close(other) == 0);
	for (unsigned clip = 0; clip < 2; clip++) {
		test->no_tail_clip = clip == 0;
		/* The whole file. */
		CHECK(ftruncate(victim, 64 * MIB) == 0);
		arm(&f, victim, 4 * MIB, 1, true);
		file = read_only(&f, "input");
		status = publish(&f, "gpt", &file, 1, false, NULL, "out", NULL);
		CHECK(f.cut_done && status == (clip ? FSIMG_EIO : FSIMG_OK) &&
		    exists(&f, "out") == !clip);
		fsimg_io_free(file);
		/* A prefix slice, ending on a chunk boundary. */
		CHECK(ftruncate(victim, 1264 * MIB) == 0);
		arm(&f, victim, 4 * MIB, 1, true);
		file = read_only(&f, "input");
		CHECK(fsimg_io_slice(file, 0, 64 * MIB, &inputs[0], NULL) ==
		    FSIMG_OK);
		status = publish(&f, "gpt", inputs, 1, false, NULL, "slice", NULL);
		CHECK(f.cut_done && status == (clip ? FSIMG_EIO : FSIMG_OK) &&
		    exists(&f, "slice") == !clip);
		fsimg_io_free(inputs[0]);
		fsimg_io_free(file);
		/* The first of two compose inputs. */
		CHECK(ftruncate(victim, 64 * MIB) == 0);
		arm(&f, victim, 4 * MIB, 1, true);
		inputs[0] = read_only(&f, "input");
		inputs[1] = read_only(&f, "other");
		status = publish(&f, "gpt", inputs, 2, true, NULL, "pair", NULL);
		CHECK(f.cut_done && status == (clip ? FSIMG_EIO : FSIMG_OK) &&
		    exists(&f, "pair") == !clip);
		fsimg_io_free(inputs[0]);
		fsimg_io_free(inputs[1]);
		/*
		 * simg, cut after the counting pass's lookup: the failure is
		 * in that pass, so nothing reaches the sink. Without the clip
		 * the counting pass completes and the writing pass's lookup
		 * sees the shrink instead.
		 */
		CHECK(ftruncate(victim, 64 * MIB) == 0);
		arm(&f, victim, 4 * MIB, 1, true);
		file = read_only(&f, "input");
		sink = (counted_t){ .length = 0 };
		status = publish(&f, "gpt/simg", &file, 1, false, &sink, NULL,
		    NULL);
		CHECK(f.cut_done && status == (clip ? FSIMG_EIO :
		    FSIMG_ESOURCE_CHANGED));
		CHECK((sink.written == 0) == (clip == 1));
		fsimg_io_free(file);
		/* simg, cut after the writing pass's lookup. */
		CHECK(ftruncate(victim, 64 * MIB) == 0);
		arm(&f, victim, 4 * MIB, 2, true);
		file = read_only(&f, "input");
		sink = (counted_t){ .length = 0 };
		status = publish(&f, "gpt/simg", &file, 1, false, &sink, NULL,
		    NULL);
		CHECK(f.cut_done && sink.written != 0 &&
		    status == (clip ? FSIMG_EIO : FSIMG_OK));
		fsimg_io_free(file);
		if (!clip)
			remove_files(&f, (const char *const[]){ "out", "slice",
			    "pair", NULL });
	}

	/* An input ending inside a chunk: the clip does not matter. */
	for (unsigned partial = 0; partial < 2; partial++) {
		test->no_tail_clip = true;
		test->partial_chunks = partial == 1;
		CHECK(ftruncate(victim, 64 * MIB + 4 * KIB) == 0);
		arm(&f, victim, 4 * MIB, 1, true);
		inputs[0] = read_only(&f, "input");
		inputs[1] = read_only(&f, "other");
		status = publish(&f, "gpt", inputs, 2, true, NULL, "straddle",
		    NULL);
		CHECK(f.cut_done && status == (partial ? FSIMG_OK : FSIMG_EIO) &&
		    exists(&f, "straddle") == (partial == 1));
		fsimg_io_free(inputs[0]);
		fsimg_io_free(inputs[1]);
	}
	remove_files(&f, (const char *const[]){ "straddle", NULL });
	test->no_tail_clip = test->partial_chunks = false;
	CHECK(ftruncate(victim, 64 * MIB + 4 * KIB) == 0);
	arm(&f, victim, 4 * MIB, 1, true);
	inputs[0] = read_only(&f, "input");
	inputs[1] = read_only(&f, "other");
	CHECK(publish(&f, "gpt", inputs, 2, true, NULL, "straddle", NULL) ==
	    FSIMG_EIO && !exists(&f, "straddle"));
	fsimg_io_free(inputs[0]);
	fsimg_io_free(inputs[1]);
	CHECK(close(victim) == 0);
	remove_files(&f, (const char *const[]){ "input", "other", NULL });
	fixture_free(&f);
}

/*
 * The documented limit: the first of two compose inputs cut once its
 * tail chunk was read, while the second is being looked up, is not
 * detected, and the image holds the bytes read before the cut, as a copy
 * without hole knowledge would. Cut before its tail is read, it fails.
 */
static void
test_undetected_modification(void)
{
	const uint64_t data[] = { 0 };
	fsimg_io_t *inputs[2];
	int victim;
	fixture_t f;

	fixture_init(&f, 0);
	victim = sparse_file(&f, "input", 64 * MIB, data, 1);
	CHECK(close(sparse_file(&f, "other", 64 * MIB, data, 1)) == 0);
	inputs[0] = read_only(&f, "input");
	inputs[1] = read_only(&f, "other");
	CHECK(publish(&f, "gpt", inputs, 2, true, NULL, "intact", NULL) ==
	    FSIMG_OK);
	fsimg_io_free(inputs[0]);
	fsimg_io_free(inputs[1]);
	for (unsigned late = 0; late < 2; late++) {
		CHECK(ftruncate(victim, 64 * MIB) == 0);
		arm(&f, victim, 4 * MIB, late ? 2 : 1, late == 0);
		inputs[0] = read_only(&f, "input");
		inputs[1] = read_only(&f, "other");
		CHECK(publish(&f, "gpt", inputs, 2, true, NULL, "cut", NULL) ==
		    (late ? FSIMG_OK : FSIMG_EIO) && f.cut_done);
		fsimg_io_free(inputs[0]);
		fsimg_io_free(inputs[1]);
	}
	CHECK(same_files(&f, "intact", "cut"));
	CHECK(close(victim) == 0);
	remove_files(&f, (const char *const[]){ "input", "other", "intact",
	    "cut", NULL });
	fixture_free(&f);
}

/*
 * The tail rule's cost is one chunk: a 100 GiB sparse input is wrapped
 * reading its two data chunks and its final chunk. With hole knowledge
 * ignored the whole 100 GiB is read and the image is the same.
 */
static void
test_tail_cost(void)
{
	const uint64_t length = 100 * GIB;
	const uint64_t data[] = { 0, 50 * GIB };
	fsimg_io_t *input;
	uint64_t before, reads[2];
	int fd;
	fixture_t f;

	fixture_init(&f, 0);
	fd = openat(f.directory_fd, "big", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
	    0600);
	CHECK(fd != -1);
	if (ftruncate(fd, (off_t)length) != 0) {
		puts("SKIP: host refuses a 100 GiB sparse file; tail cost untested");
		CHECK(close(fd) == 0);
		remove_files(&f, (const char *const[]){ "big", NULL });
		fixture_free(&f);
		return;
	}
	CHECK(close(fd) == 0 && unlinkat(f.directory_fd, "big", 0) == 0);
	CHECK(close(sparse_file(&f, "big", length, data, 2)) == 0);
	for (unsigned consult = 0; consult < 2; consult++) {
		fsimg_test_hole_discovery(f.ctx, consult == 1);
		input = read_only(&f, "big");
		before = stat_of(&f, STAT_READ);
		CHECK(publish(&f, "gpt", &input, 1, false, NULL, consult ?
		    "consulted" : "ignored", NULL) == FSIMG_OK);
		reads[consult] = stat_of(&f, STAT_READ) - before;
		fsimg_io_free(input);
	}
	fsimg_test_hole_discovery(f.ctx, true);
	CHECK(reads[1] < 8 * MIB);
	CHECK(reads[0] >= length);
	CHECK(same_sparse_files(&f, "consulted", "ignored"));
	remove_files(&f, (const char *const[]){ "big", "consulted", "ignored",
	    NULL });
	fixture_free(&f);
}

/*
 * A run of chunks a zero answer covers costs no read, but each chunk is
 * still one charged and polled step. A volume whose first half is
 * answered zero requests cancellation when it answers, inside the run:
 * the pass stops at the run's first chunk, asking nothing more. With
 * answered chunks left unpolled by the test control, the run finishes
 * and the pass asks about the second half before its first poll. DMG
 * encodes the run without touching the sink, so no sink poll intervenes.
 */
static void
test_cancellation_in_skipped_zeros(void)
{
	counted_t volume;
	fsimg_io_t *input;
	fixture_t f;

	fixture_init(&f, 0);
	for (unsigned polled = 0; polled < 2; polled++) {
		fsimg_test_holes(f.ctx)->unpolled_zeros = polled == 0;
		volume = (counted_t){ .length = 64 * MIB, .claim = CLAIM_HALF };
		input = counted_io(&f, &volume, false);
		f.cancel_answered = &volume;
		CHECK(publish(&f, "gpt/dmg", &input, 1, true, NULL, "out", NULL) ==
		    FSIMG_ECANCELLED && !exists(&f, "out"));
		CHECK(volume.read == 0 && volume.answers == (polled ? 1 : 2));
		f.cancel_answered = NULL;
		fsimg_io_free(input);
	}
	fsimg_test_holes(f.ctx)->unpolled_zeros = false;
	fixture_free(&f);
}

/*
 * A discovery sync that fails is not an error: the file is read in full,
 * the image is the same, and host answers stay off for good, so a later
 * operation on the same object neither syncs nor asks. A synced object is
 * never synced again. A host without SEEK_HOLE turns answers off the same
 * way after the one sync.
 */
static void
test_sync_failure_fallback(void)
{
	const uint64_t data[] = { 0 };
	fsimg_hole_test_t *test;
	fsimg_io_t *input;
	uint64_t before, syncs;
	fixture_t f;

	fixture_init(&f, 0);
	test = fsimg_test_holes(f.ctx);
	CHECK(close(sparse_file(&f, "input", 32 * MIB, data, 1)) == 0);
	input = read_only(&f, "input");
	syncs = test->discovery_syncs;
	CHECK(publish(&f, "gpt", &input, 1, false, NULL, "reference", NULL) ==
	    FSIMG_OK);
	CHECK(publish(&f, "gpt", &input, 1, false, NULL, "again", NULL) ==
	    FSIMG_OK);
	CHECK(test->discovery_syncs - syncs == 1);
	fsimg_io_free(input);

	input = read_only(&f, "input");
	test->sync_errno = EIO;
	for (unsigned run = 0; run < 2; run++) {
		before = stat_of(&f, STAT_READ);
		syncs = test->discovery_syncs;
		remove_files(&f, (const char *const[]){ "again", NULL });
		CHECK(publish(&f, "gpt", &input, 1, false, NULL, "again", NULL) ==
		    FSIMG_OK);
		CHECK(stat_of(&f, STAT_READ) - before >= 32 * MIB);
		CHECK(test->discovery_syncs - syncs == (run == 0 ? 1 : 0));
		CHECK(same_files(&f, "reference", "again"));
		test->sync_errno = 0;
	}
	fsimg_io_free(input);

	test->host_query = fixture_query;
	f.no_host = true;
	input = read_only(&f, "input");
	for (unsigned run = 0; run < 2; run++) {
		before = stat_of(&f, STAT_READ);
		remove_files(&f, (const char *const[]){ "again", NULL });
		CHECK(publish(&f, "gpt", &input, 1, false, NULL, "again", NULL) ==
		    FSIMG_OK);
		CHECK(stat_of(&f, STAT_READ) - before >= 32 * MIB);
		CHECK(f.queries == 1);
	}
	test->host_query = NULL;
	fsimg_io_free(input);
	remove_files(&f, (const char *const[]){ "input", "reference", "again",
	    NULL });
	fixture_free(&f);
}

/*
 * The host's report is trusted once the file is synced: a query hook
 * that calls a data range a hole yields an image with zeros there, the
 * accepted outcome the design documents. The tail rule still reads the
 * last chunk.
 */
static void
test_false_host_answer(void)
{
	const uint64_t data[] = { 0, 7 * MIB };
	fsimg_io_t *input;
	fixture_t f;

	fixture_init(&f, 0);
	CHECK(close(sparse_file(&f, "input", 8 * MIB, data, 2)) == 0);
	fsimg_test_holes(f.ctx)->host_query = fixture_query;
	f.lie = true;
	input = read_only(&f, "input");
	CHECK(publish(&f, "gpt", &input, 1, false, NULL, "lied", NULL) ==
	    FSIMG_OK);
	CHECK(holds(&f, "lied", MIB, MIB, 0) && holds(&f, "lied", 8 * MIB, MIB,
	    0x5a));
	fsimg_io_free(input);
	remove_files(&f, (const char *const[]){ "input", "lied", NULL });
	fixture_free(&f);
}

/*
 * An object built from an application's descriptor shares that
 * descriptor's file offset, which the host's hole queries seek: a wrap
 * that looks up the file's holes leaves the application's position where
 * it was.
 */
static void
test_borrowed_position(void)
{
	const uint64_t data[] = { 0 };
	fsimg_io_t *input;
	int fd;
	fixture_t f;

	fixture_init(&f, 0);
	CHECK(close(sparse_file(&f, "input", 64 * MIB, data, 1)) == 0);
	fd = openat(f.directory_fd, "input", O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1 && lseek(fd, 12345, SEEK_SET) == 12345);
	CHECK(fsimg_io_from_fd(f.ctx, fd, FSIMG_IO_READ, &input, NULL) ==
	    FSIMG_OK);
	CHECK(publish(&f, "gpt", &input, 1, false, NULL, "out", NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_test_holes(f.ctx)->discovery_syncs == 1);
	CHECK(lseek(fd, 0, SEEK_CUR) == 12345);
	fsimg_io_free(input);
	CHECK(close(fd) == 0);
	remove_files(&f, (const char *const[]){ "input", "out", NULL });
	fixture_free(&f);
}

/*
 * fsimg never asks the host about the files it writes, so no discovery
 * sync is made while a writer emits a staged image and transcodes its raw
 * intermediate. Suppressing flushes cannot pass that check: a flush
 * failure injected at the writer's required output flush fails the
 * emission and publishes nothing.
 */
static void
test_own_files_never_ask(void)
{
	fsimg_write_options_t options;
	fsimg_layer_options_t layers;
	fsimg_writer_t *writer;
	fsimg_sink_t sink;
	fixture_t f;

	fixture_init(&f, 0);
	write_disk(&f, "fat12/gpt/dmg", "", 8 * MIB, "image.dmg");
	write_disk(&f, "fat12/gpt", "", 8 * MIB, "image.raw");
	CHECK(fsimg_test_holes(f.ctx)->discovery_syncs == 0);

	fsimg_layer_options_init(&layers);
	fsimg_write_options_init(&options);
	options.flags = FSIMG_WRITE_REPRODUCIBLE | FSIMG_WRITE_DURABLE;
	options.epoch = EPOCH;
	options.layers = &layers;
	options.volume_bytes = 8 * MIB;
	CHECK(fsimg_writer_new(f.ctx, "fat12/gpt", &options, &writer, NULL) ==
	    FSIMG_OK);
	fsimg_sink_init(&sink, FSIMG_SINK_FILE);
	sink.parent_fd = f.directory_fd;
	sink.name = "unflushed";
	fsimg_test_holes(f.ctx)->flush_errno = EIO;
	CHECK(fsimg_writer_output(writer, &sink, NULL) == FSIMG_EIO);
	fsimg_test_holes(f.ctx)->flush_errno = 0;
	fsimg_writer_free(writer);
	CHECK(!exists(&f, "unflushed"));
	remove_files(&f, (const char *const[]){ "image.dmg", "image.raw",
	    NULL });
	fixture_free(&f);
}

/*
 * Whether the filesystem under TMPDIR reports a synced file's holes, which
 * the tests of the host's report need: a filesystem without holes, or
 * without the interface, answers none.
 */
static bool
host_reports_holes(void)
{
	const uint64_t data[] = { 0 };
	uint64_t hole, next, observed;
	bool reports;
	int fd;
	fixture_t f;

	fixture_init(&f, 0);
	fd = sparse_file(&f, "probe", 64 * MIB, data, 1);
	CHECK(fsync(fd) == 0);
	reports = fsimg_host_next_hole(fd, 0, NULL, NULL, &hole, &next,
	    &observed) == 0 && hole < observed;
	CHECK(close(fd) == 0);
	remove_files(&f, (const char *const[]){ "probe", NULL });
	fixture_free(&f);
	return (reports);
}

int
main(void)
{
	/* The file size limit fails writes and growth with EFBIG instead. */
	signal(SIGXFSZ, SIG_IGN);
	test_record_marking();
	test_failed_writes_stay_marked();
	test_zeros_after_writes();
	test_zeroing_never_consults_answers();
	test_callback_answers();
	test_fold_and_shrink();
	test_record_growth();
	test_resize_failures();
	test_record_bounds();
	test_transcode_of_fresh_files();
	test_determinism();
	if (host_reports_holes()) {
		test_external_host_report();
		test_shrink_seen_at_lookup();
		test_truncated_input();
		test_undetected_modification();
		test_tail_cost();
	} else
		puts("SKIP: TMPDIR reports no holes; host report untested");
	test_cancellation_in_skipped_zeros();
	test_sync_failure_fallback();
	test_false_host_answer();
	test_own_files_never_ask();
	test_borrowed_position();
	return (0);
}
