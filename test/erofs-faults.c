/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "internal.h"
#include "meta.h"
#include "erofs.h"
#include "erofs-gen.h"
#include "test.h"

/*
 * Fault injection across opening, listing, reading and verifying one
 * generated EROFS volume. Each sweep fails the Nth allocation, scratch
 * operation, source read or cancellation poll, for N = 1, 2, ... until a run
 * no longer reaches the fault, and checks that every injected failure is
 * reported with its own status, releases everything it held and leaves the
 * context reusable, and that a run the fault did not stop gives exactly the
 * clean result.
 *
 * The volume has 512-byte blocks and holds one file of each storage kind
 * the reader decodes, each with its own byte formula (expected_byte()):
 *
 *   /flat     flat-inline: three blocks and a 100-byte inline tail
 *   /chunk    chunk-based: two chunks of two blocks, the last one partial
 *   /lz4      compact index: an LZ4 cluster with matches, then a literal one
 *   /deflate  full index: one DEFLATE cluster
 *   /frag     full index: an LZ4 cluster, then a fragment in the packed inode
 *   /d        a directory stored as one LZ4 cluster, holding /d/x
 *   /d/x      flat-inline, all inline
 */

#define BLKSZBITS 9u
#define BLOCK_BYTES (1u << BLKSZBITS)
#define DATA_BLOCKS 24u
#define ATTEMPT_LIMIT 10000u
/* Metadata cache pages: few enough that the small volume's streams spill. */
#define META_CACHE_PAGES 2u

/* Superblock fields this file sets beyond what the generator writes. */
#define SB_ALGORITHM_MASK 0x54u
#define INODE_NLINK 0x06u	/* a compact inode's link count */

#define MODE_FILE 0100644u
#define MODE_DIR 040755u
#define LAYOUT_FLAT 0u
#define LAYOUT_FLAT_INLINE 2u
#define LAYOUT_COMPRESSED_FULL 1u
#define LAYOUT_COMPRESSED_COMPACT 3u
#define LAYOUT_CHUNK_BASED 4u
#define DIRENT_FILE 1u
#define DIRENT_DIR 2u

#define FLAT_SIZE (3 * BLOCK_BYTES + 100)
#define CHUNK_BITS 1u		/* two blocks to a chunk */
#define CHUNK_BYTES (BLOCK_BYTES << CHUNK_BITS)
#define CHUNK_COUNT 2u
#define CHUNK_SIZE (CHUNK_BYTES + 562)
#define LZ4_SIZE 1500u
#define LZ4_SPLIT 1000u		/* where the literal cluster starts */
#define DEFLATE_SIZE 2000u
#define FRAG_SIZE 1300u
#define FRAG_SPLIT 600u		/* where the fragment starts */
#define FRAG_OFFSET 16u		/* the fragment's offset in the packed inode */
#define PACKED_SIZE (FRAG_OFFSET + (FRAG_SIZE - FRAG_SPLIT) + 64)
#define NESTED_SIZE 40u

typedef enum entry_id {
	ENTRY_FLAT, ENTRY_CHUNK, ENTRY_LZ4, ENTRY_DEFLATE, ENTRY_FRAG,
	ENTRY_DIR, ENTRY_NESTED, ENTRY_COUNT
} entry_id_t;

typedef struct entry_spec {
	const char		*path;
	fsimg_entry_kind_t	 kind;
	uint64_t		 size;	/* checked for files only */
} entry_spec_t;

static const entry_spec_t entry_specs[ENTRY_COUNT] = {
	[ENTRY_FLAT] = { "/flat", FSIMG_ENTRY_FILE, FLAT_SIZE },
	[ENTRY_CHUNK] = { "/chunk", FSIMG_ENTRY_FILE, CHUNK_SIZE },
	[ENTRY_LZ4] = { "/lz4", FSIMG_ENTRY_FILE, LZ4_SIZE },
	[ENTRY_DEFLATE] = { "/deflate", FSIMG_ENTRY_FILE, DEFLATE_SIZE },
	[ENTRY_FRAG] = { "/frag", FSIMG_ENTRY_FILE, FRAG_SIZE },
	[ENTRY_DIR] = { "/d", FSIMG_ENTRY_DIRECTORY, 0 },
	[ENTRY_NESTED] = { "/d/x", FSIMG_ENTRY_FILE, NESTED_SIZE },
};

/* The volume's own counts, which verification reports: the root and /d. */
#define VERIFY_FILES 6u
#define VERIFY_DIRECTORIES 2u

typedef enum stage {
	STAGE_OPEN, STAGE_WALK, STAGE_CONTENT, STAGE_VERIFY, STAGE_COUNT
} stage_t;

typedef enum fault {
	FAULT_ALLOC, FAULT_SCRATCH_OPEN, FAULT_SCRATCH_RESIZE,
	FAULT_SCRATCH_READ, FAULT_SCRATCH_WRITE, FAULT_SHORT, FAULT_EOF,
	FAULT_READ, FAULT_CANCEL, FAULT_COUNT
} fault_t;

typedef struct result {
	uint64_t entries;
	uint64_t bytes;
	uint64_t digest;
	bool valid;
} result_t;

typedef struct fixture {
	const gen_image_t *image;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_stats_t baseline;
	uint64_t reads;
	uint64_t polls;
	uint64_t point;
	fault_t fault;
	bool armed;
	bool fired;
	int directory;
} fixture_t;

static const char *stages[] = { "open", "walk", "content", "verify" };
static const char *faults[] = { "allocation", "scratch-open", "scratch-resize",
    "scratch-read", "scratch-write", "short", "EOF", "read-error",
    "cancellation" };
static const fsimg_meta_fault_t scratch_faults[] = { FSIMG_META_OPEN,
    FSIMG_META_RESIZE, FSIMG_META_READ, FSIMG_META_WRITE };
static unsigned failures;

static double
seconds(void)
{
	struct timespec stamp;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &stamp) == 0);
	return ((double)stamp.tv_sec + (double)stamp.tv_nsec / 1000000000);
}

static unsigned char
pattern_byte(unsigned seed, uint64_t i)
{
	return ((unsigned char)(seed + i * 31 + (i >> 8) * 7));
}

/*
 * Each entry's content as a formula of its offset. A cluster compressed
 * with matches repeats with the period its encoder was given (9 for the LZ4
 * ones, 37 for DEFLATE); every other extent has bytes of its own, so a read
 * from a wrong place or offset shows as a difference.
 */
static unsigned char
expected_byte(entry_id_t id, uint64_t i)
{
	switch (id) {
	case ENTRY_FLAT:
		return (pattern_byte(1, i));
	case ENTRY_CHUNK:
		return (pattern_byte(2, i));
	case ENTRY_LZ4:
		return (i < LZ4_SPLIT ? pattern_byte(3, i % 9) :
		    pattern_byte(4, i - LZ4_SPLIT));
	case ENTRY_DEFLATE:
		return (pattern_byte(5, i % 37));
	case ENTRY_FRAG:
		return (i < FRAG_SPLIT ? pattern_byte(6, i % 9) :
		    pattern_byte(7, i - FRAG_SPLIT));
	case ENTRY_NESTED:
		return (pattern_byte(8, i));
	case ENTRY_DIR:
	case ENTRY_COUNT:
		break;
	}
	CHECK(false);
	return (0);
}

/* The entry's whole content, which the caller frees. */
static unsigned char *
entry_content(entry_id_t id)
{
	size_t size = (size_t)entry_specs[id].size;
	unsigned char *bytes = malloc(size);

	CHECK(bytes != NULL);
	for (size_t i = 0; i < size; i++)
		bytes[i] = expected_byte(id, i);
	return (bytes);
}

static gen_inode_t
inode_proto(uint16_t mode, unsigned layout)
{
	gen_inode_t inode;

	bzero(&inode, sizeof(inode));
	inode.mode = mode;
	inode.nlink = mode == MODE_DIR ? 2 : 1;
	inode.layout = (uint8_t)layout;
	inode.uid = 1000;
	inode.gid = 100;
	return (inode);
}

static uint64_t
add_flat(gen_tree_t *tree, entry_id_t id)
{
	gen_inode_t proto = inode_proto(MODE_FILE, LAYOUT_FLAT_INLINE);
	unsigned char *bytes = entry_content(id);
	uint64_t nid;

	nid = gen_tree_add(tree, &proto, bytes, (size_t)entry_specs[id].size);
	free(bytes);
	return (nid);
}

/* Each chunk's blocks hold its part of the content, zero after its end. */
static uint64_t
add_chunked(gen_tree_t *tree)
{
	gen_inode_t proto = inode_proto(MODE_FILE, LAYOUT_CHUNK_BASED);
	unsigned char *bytes = entry_content(ENTRY_CHUNK);
	gen_chunk_t chunks[CHUNK_COUNT];
	unsigned char data[CHUNK_BYTES];
	uint64_t nid;

	for (size_t i = 0; i < CHUNK_COUNT; i++) {
		size_t start = i * CHUNK_BYTES;
		size_t length = CHUNK_SIZE - start < CHUNK_BYTES ?
		    CHUNK_SIZE - start : CHUNK_BYTES;

		bzero(data, sizeof(data));
		bcopy(bytes + start, data, length);
		chunks[i] = gen_chunk_block(gen_tree_data(tree, data,
		    sizeof(data)));
	}
	proto.size = CHUNK_SIZE;
	/* The layout word's low half is the chunk format: 4-byte entries. */
	nid = gen_tree_add_chunked(tree, &proto, CHUNK_BITS, chunks,
	    CHUNK_COUNT);
	free(bytes);
	return (nid);
}

static uint64_t
add_zfile(gen_tree_t *tree, uint16_t mode, const gen_zfile_t *file)
{
	gen_inode_t proto = inode_proto(mode, LAYOUT_FLAT);

	return (gen_tree_add_zfile(tree, &proto, file));
}

static uint64_t
add_lz4(gen_tree_t *tree)
{
	static const gen_zcluster_t clusters[] = {
		{ .start = 0, .blocks = 1, .kind = GEN_ZK_LZ4_PERIODIC,
		    .period = 9 },
		{ .start = LZ4_SPLIT, .blocks = 1, .kind = GEN_ZK_LZ4_LITERAL },
	};
	unsigned char *bytes = entry_content(ENTRY_LZ4);
	gen_zfile_t file = { .content = bytes, .size = LZ4_SIZE,
	    .index_layout = LAYOUT_COMPRESSED_COMPACT,
	    .advice = GEN_ZA_COMPACT_2B, .algorithms = EROFS_ALGORITHM_LZ4,
	    .clusters = clusters, .cluster_count = 2, .last = GEN_ZL_BLOCKS };
	uint64_t nid;

	nid = add_zfile(tree, MODE_FILE, &file);
	free(bytes);
	return (nid);
}

/* A raw DEFLATE stream, as EROFS stores it, in one block. */
static uint64_t
add_deflate(gen_tree_t *tree)
{
	unsigned char *bytes = entry_content(ENTRY_DEFLATE);
	unsigned char stream[BLOCK_BYTES];
	gen_zcluster_t cluster = { .start = 0, .blocks = 1,
	    .kind = GEN_ZK_RAW, .raw = stream };
	gen_zfile_t file = { .content = bytes, .size = DEFLATE_SIZE,
	    .index_layout = LAYOUT_COMPRESSED_FULL,
	    .algorithms = EROFS_ALGORITHM_DEFLATE, .clusters = &cluster,
	    .cluster_count = 1, .last = GEN_ZL_BLOCKS };
	z_stream z;
	uint64_t nid;

	bzero(&z, sizeof(z));
	CHECK(deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8,
	    Z_DEFAULT_STRATEGY) == Z_OK);
	z.next_in = bytes;
	z.avail_in = DEFLATE_SIZE;
	z.next_out = stream;
	z.avail_out = sizeof(stream);
	CHECK(deflate(&z, Z_FINISH) == Z_STREAM_END);
	cluster.raw_length = z.total_out;
	CHECK(deflateEnd(&z) == Z_OK);
	nid = add_zfile(tree, MODE_FILE, &file);
	free(bytes);
	return (nid);
}

/*
 * The packed inode is flat and holds /frag's last FRAG_SIZE - FRAG_SPLIT
 * bytes at FRAG_OFFSET, between bytes of its own; /frag's first cluster is
 * LZ4 in a block and its second the fragment.
 */
static uint64_t
add_fragment_file(gen_tree_t *tree)
{
	static const gen_zcluster_t clusters[] = {
		{ .start = 0, .blocks = 1, .kind = GEN_ZK_LZ4_PERIODIC,
		    .period = 9 },
		{ .start = FRAG_SPLIT, .blocks = 0, .kind = GEN_ZK_PLAIN },
	};
	gen_inode_t proto = inode_proto(MODE_FILE, LAYOUT_FLAT);
	unsigned char *bytes = entry_content(ENTRY_FRAG);
	unsigned char packed[PACKED_SIZE];
	gen_zfile_t file = { .content = bytes, .size = FRAG_SIZE,
	    .index_layout = LAYOUT_COMPRESSED_FULL,
	    .algorithms = EROFS_ALGORITHM_LZ4, .clusters = clusters,
	    .cluster_count = 2, .last = GEN_ZL_FRAGMENT,
	    .fragment_offset = FRAG_OFFSET };
	uint64_t nid;

	for (size_t i = 0; i < sizeof(packed); i++)
		packed[i] = pattern_byte(9, i);
	bcopy(bytes + FRAG_SPLIT, packed + FRAG_OFFSET, FRAG_SIZE - FRAG_SPLIT);
	gen_tree_set_packed(tree, gen_tree_add(tree, &proto, packed,
	    sizeof(packed)));
	nid = add_zfile(tree, MODE_FILE, &file);
	free(bytes);
	return (nid);
}

/* Directory /d, its records in one LZ4 literal cluster, naming /d/x. */
static uint64_t
add_compressed_dir(gen_tree_t *tree, uint64_t nested)
{
	uint64_t nid = gen_nid_at(&tree->image, tree->meta_next);
	const gen_dirent_t records[] = { { ".", nid, DIRENT_DIR },
	    { "..", 0, DIRENT_DIR }, { "x", nested, DIRENT_FILE } };
	unsigned char bytes[BLOCK_BYTES];
	gen_zcluster_t cluster = { .start = 0, .blocks = 1,
	    .kind = GEN_ZK_LZ4_LITERAL };
	gen_zfile_t file = { .content = bytes,
	    .index_layout = LAYOUT_COMPRESSED_FULL,
	    .algorithms = EROFS_ALGORITHM_LZ4, .clusters = &cluster,
	    .cluster_count = 1, .last = GEN_ZL_BLOCKS };

	bzero(bytes, sizeof(bytes));
	file.size = gen_dir_pack(bytes, sizeof(bytes), BLOCK_BYTES, records,
	    sizeof(records) / sizeof(records[0]));
	CHECK(add_zfile(tree, MODE_DIR, &file) == nid);
	return (nid);
}

/*
 * The volume's features: LZ4_0PADDING, COMPR_CFGS with LZ4 and DEFLATE in
 * the algorithm mask, FRAGMENTS and CHUNKED_FILE. Under COMPR_CFGS one
 * configuration record per algorithm follows the superblock: LZ4's (14
 * bytes) and DEFLATE's (6 bytes, window bits 15).
 */
static void
build_image(gen_tree_t *tree)
{
	uint64_t deflate_record = GEN_SUPER_OFFSET + 128 + 16;
	gen_dirent_t root[6];
	uint64_t nested;

	gen_tree_init(tree, BLKSZBITS, gen_tree_first_block(BLKSZBITS) +
	    DATA_BLOCKS);
	gen_tree_set_incompat(tree, EROFS_INCOMPAT_LZ4_0PADDING |
	    EROFS_INCOMPAT_COMPR_CFGS | EROFS_INCOMPAT_FRAGMENTS |
	    EROFS_INCOMPAT_CHUNKED_FILE);
	gen_super_put(&tree->image, SB_ALGORITHM_MASK,
	    1u << EROFS_ALGORITHM_LZ4 | 1u << EROFS_ALGORITHM_DEFLATE, 2);
	gen_lz4_config_put(&tree->image, 0, 65535, 1);
	gen_put(&tree->image, deflate_record, 6, 2);
	gen_put(&tree->image, deflate_record + 2, 15, 1);

	root[0] = (gen_dirent_t){ "chunk", add_chunked(tree), DIRENT_FILE };
	root[1] = (gen_dirent_t){ "deflate", add_deflate(tree), DIRENT_FILE };
	root[2] = (gen_dirent_t){ "flat", add_flat(tree, ENTRY_FLAT),
	    DIRENT_FILE };
	root[3] = (gen_dirent_t){ "frag", add_fragment_file(tree),
	    DIRENT_FILE };
	root[4] = (gen_dirent_t){ "lz4", add_lz4(tree), DIRENT_FILE };
	nested = add_flat(tree, ENTRY_NESTED);
	root[5] = (gen_dirent_t){ "d", add_compressed_dir(tree, nested),
	    DIRENT_DIR };
	gen_tree_dir(tree, 0, 0, root, sizeof(root) / sizeof(root[0]));
	/* The root's link count counts its subdirectory /d. */
	gen_put(&tree->image, gen_inode_offset(&tree->image, 0) + INODE_NLINK,
	    3, 2);
	CHECK(tree->data_next <= gen_tree_first_block(BLKSZBITS) + DATA_BLOCKS);
	gen_tree_seal(tree);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *size, fsimg_error_t *error)
{
	(void)error;
	*size = ((fixture_t *)argument)->image->length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error)
{
	fixture_t *f = argument;
	bool inject;

	f->reads++;
	*count = 0;
	inject = f->armed && f->reads == f->point;
	if (inject && (f->fault == FAULT_EOF || f->fault == FAULT_READ)) {
		f->fired = true;
		if (f->fault == FAULT_EOF)
			return (FSIMG_OK);
		return (fsimg_error_set(error, FSIMG_EIO, EIO, FSIMG_OP_IO,
		    "injected source read failure"));
	}
	CHECK(offset <= f->image->length);
	if (length > f->image->length - offset)
		length = f->image->length - offset;
	if (inject && f->fault == FAULT_SHORT && length > 1) {
		length /= 2;
		f->fired = true;
	}
	bcopy(f->image->bytes + offset, buffer, length);
	*count = length;
	return (FSIMG_OK);
}

static bool
cancelled(void *argument)
{
	fixture_t *f = argument;

	f->polls++;
	if (f->armed && f->fault == FAULT_CANCEL && f->polls >= f->point) {
		f->fired = true;
		return (true);
	}
	return (false);
}

static fsimg_status_t
open_image(fixture_t *f, fsimg_error_t *error)
{
	return (fsimg_reader_open(f->ctx, f->io, NULL, &f->reader, error));
}

static void
make_io(fixture_t *f)
{
	fsimg_io_ops_t ops;

	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.get_size = memory_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, f, &f->io, NULL) ==
	    FSIMG_OK);
}

/* The digest includes paths and every decoded content byte. */
static void
digest_bytes(result_t *result, const unsigned char *bytes, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		result->digest ^= bytes[i];
		result->digest *= UINT64_C(1099511628211);
	}
}

static bool
find_entry(const char *path, entry_id_t *id)
{
	for (entry_id_t i = 0; i < ENTRY_COUNT; i++) {
		if (strcmp(entry_specs[i].path, path) == 0) {
			*id = i;
			return (true);
		}
	}
	return (false);
}

/*
 * List the volume, and with content read every file in pieces that cross
 * block and cluster boundaries. Each entry must be one the volume holds,
 * with its kind and size, and each byte its formula's: comparing the digest
 * alone could bless a consistently truncated result.
 */
static fsimg_status_t
walk(fixture_t *f, bool content, result_t *result, fsimg_error_t *error)
{
	const fsimg_entry_t *entry;
	unsigned char bytes[700];
	fsimg_status_t status;
	size_t count, length;
	uint64_t offset;
	entry_id_t id;

	while ((status = fsimg_reader_next(f->reader, &entry, error)) ==
	    FSIMG_OK) {
		result->entries++;
		digest_bytes(result, (const unsigned char *)entry->path,
		    strlen(entry->path));
		if (!find_entry(entry->path, &id) ||
		    entry->kind != entry_specs[id].kind ||
		    (entry->kind == FSIMG_ENTRY_FILE &&
		    entry->size != entry_specs[id].size)) {
			result->valid = false;
			return (FSIMG_OK);
		}
		if (!content || entry->kind != FSIMG_ENTRY_FILE)
			continue;
		for (offset = 0; offset < entry->size; offset += count) {
			length = entry->size - offset < sizeof(bytes) ?
			    (size_t)(entry->size - offset) : sizeof(bytes);
			status = fsimg_reader_read_at(f->reader, offset, bytes,
			    length, &count, error);
			if (status != FSIMG_OK)
				return (status);
			if (count != length) {
				result->valid = false;
				return (FSIMG_OK);
			}
			for (size_t i = 0; i < count; i++) {
				if (bytes[i] != expected_byte(id, offset + i))
					result->valid = false;
			}
			digest_bytes(result, bytes, count);
			result->bytes += count;
		}
	}
	return (status == FSIMG_DONE ? FSIMG_OK : status);
}

/*
 * Diagnostic records are optional, and failures must report the same status
 * with either form.
 */
static fsimg_status_t
perform(fixture_t *f, stage_t stage, result_t *result, fsimg_error_t *error)
{
	fsimg_verify_report_t report = {0};
	fsimg_status_t status;

	*result = (result_t){ .digest = UINT64_C(14695981039346656037),
	    .valid = true };
	switch (stage) {
	case STAGE_OPEN:
		return (open_image(f, error));
	case STAGE_WALK:
		return (walk(f, false, result, error));
	case STAGE_CONTENT:
		return (walk(f, true, result, error));
	case STAGE_VERIFY:
		status = fsimg_verify(f->reader, FSIMG_VERIFY_PAYLOAD, &report,
		    error);
		result->valid = report.complete == (status == FSIMG_OK);
		if (status == FSIMG_OK) {
			result->valid = result->valid &&
			    report.files == VERIFY_FILES &&
			    report.directories == VERIFY_DIRECTORIES;
			result->entries = report.files;
			result->bytes = report.logical_bytes;
		}
		return (status);
	case STAGE_COUNT:
		break;
	}
	CHECK(false);
	return (FSIMG_EINVAL);
}

static void
prepare(fixture_t *f, const gen_image_t *image, int directory,
    stage_t stage)
{
	fsimg_options_t options;

	*f = (fixture_t){ .image = image, .directory = directory };
	fsimg_options_init(&options);
	/* The context requires at least 16 MiB. The small metadata cache
	 * makes the listing and verification streams spill to scratch with
	 * this small volume. */
	options.memory_bytes = UINT64_C(16) << 20;
	options.scratch_directory_fd = directory;
	options.cancelled = cancelled;
	options.cancel_argument = f;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_test_meta_cache(f->ctx, META_CACHE_PAGES, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &f->baseline, NULL) == FSIMG_OK);
	make_io(f);
	if (stage != STAGE_OPEN)
		CHECK(open_image(f, NULL) == FSIMG_OK);
	f->reads = f->polls = 0;
}

static void
directory_empty(int directory)
{
	struct dirent *entry;
	struct stat st;
	DIR *stream;
	int fd;

	/* A separate directory description, so each scan starts at zero. */
	fd = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
	    O_CLOEXEC);
	CHECK(fd != -1);
	stream = fdopendir(fd);
	CHECK(stream != NULL);
	errno = 0;
	while ((entry = readdir(stream)) != NULL) {
		CHECK(fstatat(directory, entry->d_name, &st,
		    AT_SYMLINK_NOFOLLOW) == 0);
		CHECK(strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0);
		errno = 0;
	}
	CHECK(errno == 0);
	CHECK(closedir(stream) == 0);
}

static void
release_handles(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_reader_free(f->reader);
	f->reader = NULL;
	fsimg_io_free(f->io);
	f->io = NULL;
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == f->baseline.memory_bytes);
	CHECK(stats.scratch_bytes == 0);
	CHECK(stats.descriptors == f->baseline.descriptors);
	directory_empty(f->directory);
}

/*
 * Every attempt has this single exit: turn off injection, release all handles,
 * check accounting and names, then prove the same context remains reusable.
 * The recovery walk goes through the same teardown checks as the attempt.
 */
static void
cleanup(fixture_t *f)
{
	result_t result = { .valid = true };
	fsimg_status_t status;

	f->armed = false;
	fsimg_test_fail_alloc_after(f->ctx, 0);
	fsimg_test_meta_fail_after(f->ctx, FSIMG_META_OPEN, 0, 0);
	release_handles(f);
	make_io(f);
	status = open_image(f, NULL);
	if (status == FSIMG_OK)
		status = walk(f, false, &result, NULL);
	release_handles(f);
	CHECK(status == FSIMG_OK);
	CHECK(result.valid && result.entries == ENTRY_COUNT);
	fsimg_ctx_free(f->ctx);
}

static bool
same_result(const result_t *a, const result_t *b)
{
	return (a->entries == b->entries && a->bytes == b->bytes &&
	    a->digest == b->digest && a->valid);
}

static bool
scratch_fault(fault_t fault)
{
	return (fault >= FAULT_SCRATCH_OPEN && fault <= FAULT_SCRATCH_WRITE);
}

/*
 * Opening reads the superblock and the root inode and keeps no metadata
 * stream, so it never touches scratch and its scratch sweeps are skipped;
 * the baseline run checks that this still holds. Listing spools directory
 * records and verification its reference and claim records, both of which
 * spill under the small metadata cache.
 */
static bool
stage_reaches_scratch(stage_t stage)
{
	return (stage != STAGE_OPEN);
}

static bool
fault_reached(const fixture_t *f, fault_t fault, unsigned point)
{
	switch (fault) {
	case FAULT_ALLOC:
		return (f->ctx->allocation_attempts >= point);
	case FAULT_SCRATCH_OPEN:
	case FAULT_SCRATCH_RESIZE:
	case FAULT_SCRATCH_READ:
	case FAULT_SCRATCH_WRITE:
		return (f->ctx->meta_fault_after == 0);
	case FAULT_SHORT:
		return (f->reads >= point);
	default:
		return (f->fired);
	}
}

static fsimg_status_t
expected_status(fault_t fault, bool hit)
{
	if (!hit || fault == FAULT_SHORT)
		return (FSIMG_OK);
	switch (fault) {
	case FAULT_ALLOC:
		return (FSIMG_ENOMEM);
	case FAULT_CANCEL:
		return (FSIMG_ECANCELLED);
	default:
		return (FSIMG_EIO);
	}
}

static fsimg_status_t
attempt(fixture_t *f, stage_t stage, fault_t fault, unsigned point,
    const result_t *expected, fsimg_error_t *error, bool *hit)
{
	result_t result;
	fsimg_status_t status, wanted;

	f->armed = true;
	f->fault = fault;
	f->point = point;
	if (fault == FAULT_ALLOC)
		fsimg_test_fail_alloc_after(f->ctx, point);
	else if (scratch_fault(fault))
		fsimg_test_meta_fail_after(f->ctx,
		    scratch_faults[fault - FAULT_SCRATCH_OPEN], point, EIO);
	if (error != NULL)
		fsimg_error_clear(error);
	status = perform(f, stage, &result, error);
	*hit = fault_reached(f, fault, point);
	wanted = expected_status(fault, *hit);
	/* A failure passed a record fills it: its status and a message. */
	if (error != NULL && status != FSIMG_OK &&
	    (error->status != status || error->message[0] == '\0')) {
		fprintf(stderr, "stage=%s fault=%s attempt=%u status=%d "
		    "error.status=%d message=\"%s\"\n", stages[stage],
		    faults[fault], point, status, error->status,
		    error->message);
		failures++;
	}
	if (status != wanted || !result.valid || (status == FSIMG_OK &&
	    !same_result(&result, expected))) {
		fprintf(stderr, "stage=%s fault=%s attempt=%u record=%u "
		    "status=%d expected=%d result_matches=%u message=\"%s\"\n",
		    stages[stage], faults[fault], point,
		    (unsigned)(error != NULL), status, wanted,
		    (unsigned)same_result(&result, expected),
		    error != NULL ? error->message : "");
		failures++;
	}
	return (status);
}

static void
sweep(const gen_image_t *image, int directory, stage_t stage, fault_t fault,
    const result_t *expected)
{
	fixture_t f;
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t hits = 0;
	bool hit, any_hit, complete = false;
	double start = seconds();
	unsigned point, forms = fault == FAULT_ALLOC ? 2 : 1;

	/* Flushed so that a crash or hang names the sweep it happened in. */
	printf("EROFS faults stage=%s fault=%s\n", stages[stage],
	    faults[fault]);
	CHECK(fflush(stdout) == 0);
	for (point = 1; point < ATTEMPT_LIMIT; point++) {
		any_hit = false;
		complete = true;
		for (unsigned i = 0; i < forms; i++) {
			fsimg_error_t *detail;

			/* Allocation faults use both record forms at
			 * every point; other faults alternate NULL on odd
			 * points and a cleared record on even points. */
			detail = (point + i) % 2 == 0 ? &error : NULL;
			prepare(&f, image, directory, stage);
			status = attempt(&f, stage, fault, point, expected,
			    detail, &hit);
			any_hit |= hit;
			complete = complete && status == FSIMG_OK;
			if (hit && (fault != FAULT_SHORT || f.fired))
				hits++;
			/* Cleanup runs for an unexpected outcome too. */
			cleanup(&f);
		}
		if (!any_hit)
			break;
		complete = false;
	}
	printf("EROFS faults stage=%s fault=%s attempts=%u hits=%" PRIu64
	    " seconds=%.3f complete=%u\n", stages[stage], faults[fault], point,
	    hits, seconds() - start, (unsigned)complete);
	CHECK(fflush(stdout) == 0);
	CHECK(complete);
	CHECK(hits != 0);
}

int
main(int argc, char **argv)
{
	gen_tree_t tree;
	fixture_t f;
	result_t expected[STAGE_COUNT];
	fsimg_stats_t stats, before;
	char path[] = "erofs-faults-XXXXXX";
	int directory, open_descriptors;
	double start = seconds();
	bool selected = false;

	CHECK(argc >= 1 && argc <= 3);
	CHECK(mkdtemp(path) != NULL);
	directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(directory != -1);
	open_descriptors = fsimg_test_open_descriptors();
	build_image(&tree);
	for (stage_t stage = STAGE_OPEN; stage < STAGE_COUNT; stage++) {
		prepare(&f, &tree.image, directory, stage);
		CHECK(fsimg_ctx_get_stats(f.ctx, &before, NULL) == FSIMG_OK);
		CHECK(perform(&f, stage, &expected[stage], NULL) == FSIMG_OK);
		CHECK(expected[stage].valid);
		CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
		printf("baseline stage=%s reads=%" PRIu64 " polls=%" PRIu64
		    " scratch_peak=%" PRIu64 "\n", stages[stage], f.reads,
		    f.polls, stats.scratch_peak);
		CHECK(stats.io_calls - before.io_calls >= f.reads);
		/* The stages that keep metadata streams must spill them, rather
		 * than silently reducing their scratch sweeps to in-memory
		 * tests; opening keeps none. */
		CHECK((stats.scratch_peak > before.scratch_peak) ==
		    stage_reaches_scratch(stage));
		if (stage == STAGE_WALK || stage == STAGE_CONTENT)
			CHECK(expected[stage].entries == ENTRY_COUNT);
		cleanup(&f);
	}
	for (stage_t stage = STAGE_OPEN; stage < STAGE_COUNT; stage++) {
		if (argc > 1 && strcmp(argv[1], stages[stage]) != 0)
			continue;
		for (fault_t fault = FAULT_ALLOC; fault < FAULT_COUNT; fault++) {
			if (argc > 2 && strcmp(argv[2], faults[fault]) != 0)
				continue;
			selected = true;
			if (scratch_fault(fault) &&
			    !stage_reaches_scratch(stage))
				continue;
			sweep(&tree.image, directory, stage, fault,
			    &expected[stage]);
		}
	}
	CHECK(selected);
	gen_tree_free(&tree);
	directory_empty(directory);
	CHECK(fsimg_test_open_descriptors() == open_descriptors);
	CHECK(close(directory) == 0);
	CHECK(rmdir(path) == 0);
	printf("EROFS faults seconds=%.3f failures=%u\n", seconds() - start,
	    failures);
	return (failures == 0 ? 0 : 1);
}
