/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <strings.h>
#include <unistd.h>

#include "host.h"
#include "internal.h"
#include "io.h"

/*
 * What a root is, which decides what it knows about which of its bytes read as
 * zero. A slice is always what its root is.
 */
typedef enum root_kind {
	ROOT_EXTERNAL,	/* A file fsimg did not write: the host's report */
	ROOT_FRESH,	/* Adopted empty, fsimg its only writer: its record */
	ROOT_SCRATCH,	/* Unsized private scratch, never asked */
	ROOT_CALLBACK,	/* A caller's object: its next_zero_range callback */
	ROOT_RESOLVER	/* Library-computed bytes: its own operations */
} root_kind_t;

/*
 * The written-range record of a fresh root: one bit per granule of 2^shift
 * bytes, set when some byte of the granule may be nonzero. bits covers at
 * least every granule of the root's length and at most RECORD_MAX_GRANULES of
 * them, and every bit past the length is clear.
 */
typedef struct written_record {
	unsigned char	*bits;		/* bytes long, charged; NULL when 0 */
	size_t		 bytes;		/* Capacity, grown geometrically */
	unsigned	 shift;
} written_record_t;

/* The two states a granule's bit records. */
typedef enum granule_state {
	GRANULE_CLEAR,		/* Never written: reads as zero */
	GRANULE_MARKED		/* Possibly nonzero */
} granule_state_t;

#define RECORD_MIN_SHIFT	12			/* 4 KiB */
#define RECORD_MAX_GRANULES	(UINT64_C(1) << 23)	/* 1 MiB of bits */

/*
 * Whether a read-only external root asks the host for its holes. The state
 * only moves forward: the first lookup syncs the file, and a failed sync or a
 * host that cannot answer turns host answers off for good.
 */
typedef enum host_answers {
	HOST_NOT_SYNCED,
	HOST_SYNCED,
	HOST_OFF
} host_answers_t;

struct fsimg_io {
	fsimg_ctx_t	*ctx;
	fsimg_session_t	*session;
	fsimg_io_t	*root;
	uint64_t	 base;
	uint64_t	 length;
	unsigned	 access;
	int		 fd;
	root_kind_t	 kind;		/* A root's; slices read their root's */
	fsimg_io_ops_t	 ops;
	void		*argument;
	/* A resolver's operations, and backing, the file whose identity it
	 * carries for alias and stability checks.
	 */
	const fsimg_io_resolver_ops_t *resolver;
	fsimg_io_t	*backing;
	written_record_t record;	/* ROOT_FRESH only */
	host_answers_t	 host;		/* ROOT_EXTERNAL only */
};

static fsimg_status_t io_range(fsimg_io_t *, uint64_t, uint64_t,
    fsimg_error_t *);
static fsimg_status_t io_zero(fsimg_io_t *, fsimg_session_t *, uint64_t,
    uint64_t, fsimg_error_t *);
static fsimg_status_t io_transfer(fsimg_io_t *, uint64_t, void *, size_t,
    size_t *, bool, bool, fsimg_session_t *, fsimg_error_t *);
static fsimg_status_t adapter_transfer(fsimg_io_t *, uint64_t, void *,
    size_t, size_t *, bool, fsimg_error_t *);
static fsimg_status_t callback_result(fsimg_status_t, fsimg_error_t *);
static fsimg_status_t io_from_fd(fsimg_ctx_t *, int, unsigned, bool,
    fsimg_io_t **, fsimg_error_t *);
static fsimg_status_t adopt_fresh(fsimg_ctx_t *, int, unsigned,
    fsimg_io_t **, fsimg_error_t *);
static fsimg_status_t answer_check(uint64_t, uint64_t, uint64_t, uint64_t,
    fsimg_status_t, const char *, fsimg_error_t *);
static fsimg_status_t discovery_sync(fsimg_io_t *, fsimg_error_t *);
static fsimg_status_t host_zero_range(fsimg_io_t *, uint64_t, uint64_t *,
    uint64_t *, fsimg_error_t *);
static fsimg_status_t external_answer(fsimg_io_t *, uint64_t, uint64_t,
    uint64_t *, uint64_t *, fsimg_error_t *);
static uint64_t record_granules(uint64_t, unsigned);
static bool record_bit(const written_record_t *, uint64_t);
static uint64_t record_find(const written_record_t *, uint64_t, uint64_t,
    granule_state_t);
static void record_mark(fsimg_io_t *, uint64_t, uint64_t);
static void record_next(const fsimg_io_t *, uint64_t, uint64_t,
    granule_state_t, uint64_t *, uint64_t *);
static void record_clear(written_record_t *, uint64_t, uint64_t);
static fsimg_status_t record_prepare(const fsimg_io_t *, uint64_t,
    written_record_t *, fsimg_error_t *);
static void record_discard(const fsimg_io_t *, written_record_t *);
static void record_commit(fsimg_io_t *, written_record_t *, uint64_t);
static void root_release(fsimg_io_t *);
static fsimg_status_t zero_piece(fsimg_io_t *, fsimg_session_t *, uint64_t,
    uint64_t, unsigned char **, bool *, fsimg_error_t *);

void
fsimg_io_bind_session(fsimg_io_t *io, fsimg_session_t *session)
{
	io->root->session = session;
}

fsimg_ctx_t *
fsimg_io_context(const fsimg_io_t *io)
{
	return (io == NULL ? NULL : io->ctx);
}

fsimg_status_t
fsimg_io_check_output(fsimg_io_t *io, uint64_t length, bool durable,
    bool *resize, fsimg_error_t *error)
{
	uint64_t current;
	fsimg_status_t status;

	*resize = false;
	status = fsimg_io_get_size(io, &current, error);
	if (status != FSIMG_OK)
		return (status);
	if ((io->access & FSIMG_IO_WRITE) == 0 ||
	    (durable && (io->root->ops.flags & FSIMG_IO_DURABLE) == 0))
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "sink lacks required write or durability capability"));
	*resize = io->root == io && (io->fd != -1 || io->ops.resize != NULL);
	if (!*resize && current != length)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "fixed sink length differs from planned volume"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_identity(fsimg_io_t *io, fsimg_host_file_t *identity, bool *known,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	*known = false;
	bzero(identity, sizeof(*identity));
	status = io_range(io, 0, 0, error);
	if (status != FSIMG_OK)
		return (status);
	if (io->root->backing != NULL)
		return (fsimg_io_identity(io->root->backing, identity, known, error));
	if (io->root->fd == -1)
		return (FSIMG_OK);
	if (fsimg_host_file(io->root->fd, identity) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_IO, "image identity query failed"));
	*known = true;
	return (FSIMG_OK);
}

/*
 * Adopt a fresh file: the descriptor must be an empty file, and its
 * creator's contract makes fsimg its only writer, so every range not
 * written through the object reads as zero. The record starts empty at
 * the finest granule; a file of length zero needs no bits yet.
 */
static fsimg_status_t
adopt_fresh(fsimg_ctx_t *ctx, int fd, unsigned access, fsimg_io_t **out,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_io_adopt_fd(ctx, fd, access, out, error);
	if (status != FSIMG_OK)
		return (status);
	if ((*out)->length != 0) {
		fsimg_io_free(*out);
		*out = NULL;
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "fresh output requires an empty private file"));
	}
	(*out)->kind = ROOT_FRESH;
	(*out)->record = (written_record_t){ .shift = RECORD_MIN_SHIFT };
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_adopt_empty_output(fsimg_ctx_t *ctx, int fd, fsimg_io_t **out,
    fsimg_error_t *error)
{
	return (adopt_fresh(ctx, fd, FSIMG_IO_WRITE, out, error));
}

/*
 * A handed-over file: the descriptor is adopted on every outcome, and
 * accepted only as what its creator promised, checked on the descriptor
 * itself before any byte moves: a regular file, empty, with no name left
 * to reach it (link count zero), open for reading and writing without
 * O_APPEND, which would move every write to the end. The creator keeps no
 * other descriptor or mapping of it, so the object is a fresh file.
 */
fsimg_status_t
fsimg_io_adopt_handed_over(fsimg_ctx_t *ctx, int fd, const char *role,
    fsimg_io_t **out, fsimg_error_t *error)
{
	struct stat st;
	fsimg_status_t status;
	int flags;

	*out = NULL;
	flags = fcntl(fd, F_GETFL);
	if (flags == -1 || fstat(fd, &st) == -1 || !S_ISREG(st.st_mode) ||
	    st.st_size != 0 || st.st_nlink != 0 ||
	    (flags & O_ACCMODE) != O_RDWR || (flags & O_APPEND) != 0) {
		close(fd);
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IO,
		    "a handed-over file must be an empty, unlinked, read/write "
		    "regular file");
		fsimg_error_path(error, role);
		return (FSIMG_EINVAL);
	}
	status = fsimg_fd_reserve(ctx, error);
	if (status != FSIMG_OK) {
		close(fd);
		return (status);
	}
	return (adopt_fresh(ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE, out,
	    error));
}

/*
 * An answer's shape, in root coordinates: within [offset, length] and making
 * progress. A callback's or a resolver's answer that breaks it fails as
 * status, with message.
 */
static fsimg_status_t
answer_check(uint64_t offset, uint64_t length, uint64_t start, uint64_t end,
    fsimg_status_t status, const char *message, fsimg_error_t *error)
{
	if (start < offset || end < start || end > length ||
	    (start == end && start == offset && start != length))
		return (fsimg_error_set(error, status, 0, FSIMG_OP_IO, message));
	return (FSIMG_OK);
}

/*
 * Discovery's own sync of an external file, separate from
 * fsimg_io_flush: it is counted for tests, an interruption is retried as
 * the flush loop retries it, and any other failure only turns host
 * answers off, since every byte can still be read. Cancellation polled
 * between retries is the one error.
 */
static fsimg_status_t
discovery_sync(fsimg_io_t *root, fsimg_error_t *error)
{
	fsimg_hole_test_t *test = &root->ctx->hole_test;
	fsimg_status_t status;
	int result;

	fsimg_stat_add(&test->discovery_syncs, 1);
	for (unsigned interruptions = 0;; interruptions++) {
		status = fsimg_poll(root->ctx, NULL, error);
		if (status != FSIMG_OK)
			return (status);
		result = fsync(root->fd);
		if (result == 0 && test->sync_errno != 0) {
			result = -1;
			errno = test->sync_errno;
		}
		if (result == 0) {
			root->host = HOST_SYNCED;
			return (FSIMG_OK);
		}
		if (errno != EINTR || interruptions == 1024) {
			root->host = HOST_OFF;
			return (FSIMG_OK);
		}
	}
}

/*
 * A read-only external root's next hole at or after offset, from the
 * host's report once the file is synced, in root coordinates. The length
 * the query observes bounds the answer, and a file observed shorter than
 * declared has changed: that fails at once, since the file may be
 * restored before a later read reaches the range the answer covered. A
 * host that cannot answer turns host answers off; off answers nothing.
 */
static fsimg_status_t
host_zero_range(fsimg_io_t *root, uint64_t offset, uint64_t *start,
    uint64_t *end, fsimg_error_t *error)
{
	fsimg_hole_test_t *test = &root->ctx->hole_test;
	uint64_t hole, data, observed;
	fsimg_status_t status;
	int result;

	*start = *end = root->length;
	if (root->host == HOST_NOT_SYNCED) {
		status = discovery_sync(root, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (root->host == HOST_OFF)
		return (FSIMG_OK);
	if (test->host_query != NULL)
		result = test->host_query(test->argument, root->fd, offset, &hole,
		    &data, &observed);
	else
		result = fsimg_host_next_hole(root->fd, offset,
		    test->between_seeks, test->argument, &hole, &data, &observed);
	if (result == -1) {
		root->host = HOST_OFF;
		return (FSIMG_OK);
	}
	if (!test->no_length_check) {
		if (observed < root->length)
			return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
			    FSIMG_OP_IO, "input file shrank while it was read"));
		hole = hole < observed ? hole : observed;
		data = data < observed ? data : observed;
	}
	*start = hole < root->length ? hole : root->length;
	*end = data < root->length ? data : root->length;
	if (*start < offset || *end < *start)
		*start = *end = root->length;
	return (FSIMG_OK);
}

/*
 * The answer of a file fsimg did not write, for an object ending at limit
 * in root coordinates. A writable one is a sink and answers nothing. A
 * read-only one answers from the host's report, ending at most at the
 * object's own last byte (the tail rule), so the chunk holding that byte
 * is always read with an exact read, which fails on a file cut short
 * after its lookup.
 */
static fsimg_status_t
external_answer(fsimg_io_t *root, uint64_t from, uint64_t limit,
    uint64_t *low, uint64_t *high, fsimg_error_t *error)
{
	fsimg_status_t status;

	*low = *high = limit;
	if ((root->access & FSIMG_IO_WRITE) != 0)
		return (FSIMG_OK);
	status = host_zero_range(root, from, low, high, error);
	if (status != FSIMG_OK)
		return (status);
	if (!root->ctx->hole_test.no_tail_clip && *high > limit - 1)
		*high = limit - 1;
	if (*low >= *high)
		*low = *high = limit;
	return (FSIMG_OK);
}

/*
 * Each kind of root answers from what it is; the answer, in root
 * coordinates, is then clipped to the object and translated, so a slice
 * answers through its root. An external file's answer never covers the
 * object's own last byte, so the chunk holding it is always read with an
 * exact read, which fails on a file still short there.
 */
fsimg_status_t
fsimg_io_next_zero_range(fsimg_io_t *io, uint64_t offset, uint64_t *start,
    uint64_t *end, fsimg_error_t *error)
{
	fsimg_io_t *root;
	uint64_t from, limit, low, high;
	fsimg_status_t status;

	*start = *end = 0;
	status = io_range(io, offset, 0, error);
	if (status != FSIMG_OK)
		return (status);
	*start = *end = io->length;
	if (offset == io->length)
		return (FSIMG_OK);
	root = io->root;
	from = io->base + offset;
	limit = io->base + io->length;
	low = high = root->length;
	switch (root->kind) {
	case ROOT_CALLBACK:
		if (root->ops.next_zero_range == NULL)
			break;
		root->ctx->in_callback = true;
		status = root->ops.next_zero_range(root->argument, from,
		    root->length, &low, &high, error);
		root->ctx->in_callback = false;
		status = callback_result(status, error);
		if (status == FSIMG_OK)
			status = answer_check(from, root->length, low, high,
			    FSIMG_EIO, "inconsistent callback answer", error);
		break;
	case ROOT_EXTERNAL:
		status = external_answer(root, from, limit, &low, &high, error);
		break;
	case ROOT_SCRATCH:
		break;
	case ROOT_FRESH:
		record_next(root, from, limit, GRANULE_CLEAR, &low, &high);
		break;
	case ROOT_RESOLVER:
		status = root->resolver->next_zero(root->argument, from, &low,
		    &high, error);
		if (status == FSIMG_OK)
			status = answer_check(from, root->length, low, high,
			    FSIMG_ESTATE, "inconsistent zero range", error);
		break;
	}
	if (status != FSIMG_OK)
		return (status);
	/* Clip to the object; an answer past its end is the empty one there. */
	if (low >= limit)
		low = high = limit;
	else if (high > limit)
		high = limit;
	*start = low - io->base;
	*end = high - io->base;
	return (FSIMG_OK);
}

/* The number of granules of 2^shift bytes that length bytes cover. */
static uint64_t
record_granules(uint64_t length, unsigned shift)
{
	return ((length >> shift) +
	    ((length & ((UINT64_C(1) << shift) - 1)) != 0));
}

static bool
record_bit(const written_record_t *record, uint64_t granule)
{
	return ((record->bits[granule / 8] >> (granule % 8) & 1) != 0);
}

/*
 * The first granule in [first, limit) in state, or limit when there is
 * none. Whole bytes of the other state are skipped at once, so a scan
 * costs at most one step per byte of bits.
 */
static uint64_t
record_find(const written_record_t *record, uint64_t first, uint64_t limit,
    granule_state_t state)
{
	const unsigned char other = state == GRANULE_MARKED ? 0x00 : 0xff;

	while (first < limit) {
		if (first % 8 == 0 && limit - first >= 8 &&
		    record->bits[first / 8] == other) {
			first += 8;
			continue;
		}
		if (record_bit(record, first) == (state == GRANULE_MARKED))
			return (first);
		first++;
	}
	return (limit);
}

/*
 * Mark the granules covering [offset, offset + length) of a fresh root
 * possibly nonzero. The range lies within the root, whose every granule
 * the record covers.
 */
static void
record_mark(fsimg_io_t *root, uint64_t offset, uint64_t length)
{
	written_record_t *record = &root->record;
	uint64_t last;

	if (length == 0)
		return;
	last = (offset + length - 1) >> record->shift;
	for (uint64_t granule = offset >> record->shift; granule <= last;
	    granule++)
		record->bits[granule / 8] |= (unsigned char)(1u << granule % 8);
}

/*
 * The first run of granules in state meeting [from, to) of a fresh root,
 * clipped to it, as [*start, *end); *start == *end == to when there is
 * none.
 */
static void
record_next(const fsimg_io_t *root, uint64_t from, uint64_t to,
    granule_state_t state, uint64_t *start, uint64_t *end)
{
	const written_record_t *record = &root->record;
	unsigned shift = record->shift;
	uint64_t first, last, limit;

	*start = *end = to;
	if (from >= to)
		return;
	limit = record_granules(to, shift);
	first = record_find(record, from >> shift, limit, state);
	if (first == limit)
		return;
	last = record_find(record, first, limit, state == GRANULE_MARKED ?
	    GRANULE_CLEAR : GRANULE_MARKED);
	*start = first << shift > from ? first << shift : from;
	*end = last == limit ? to : last << shift;
}

/* Clear the bits of granules [first, limit), which the record holds. */
static void
record_clear(written_record_t *record, uint64_t first, uint64_t limit)
{
	for (; first < limit && first % 8 != 0; first++)
		record->bits[first / 8] &= (unsigned char)~(1u << first % 8);
	if (limit - first >= 8) {
		bzero(record->bits + first / 8, (size_t)((limit - first) / 8));
		first += (limit - first) / 8 * 8;
	}
	for (; first < limit; first++)
		record->bits[first / 8] &= (unsigned char)~(1u << first % 8);
}

/*
 * Prepare the record for a resize to length without changing the object.
 * The granule grows, never shrinks, to the smallest at which length fits
 * RECORD_MAX_GRANULES. A record whose capacity already covers length at
 * its granule is kept, so *prepared shares its bits. Otherwise new bits
 * are allocated and charged: at the same granule the capacity at least
 * doubles, up to the maximum, and the old bits are copied, so a file
 * grown by small appends reallocates a logarithmic number of times; at a
 * coarser granule the capacity is what length needs, and each new bit is
 * the OR of the old bits it replaces.
 */
static fsimg_status_t
record_prepare(const fsimg_io_t *root, uint64_t length,
    written_record_t *prepared, fsimg_error_t *error)
{
	const written_record_t *record = &root->record;
	const size_t most = RECORD_MAX_GRANULES / 8;
	fsimg_error_t detail;
	uint64_t granule, granules;
	size_t needed;
	unsigned shift;

	*prepared = *record;
	shift = record->shift;
	while (record_granules(length, shift) > RECORD_MAX_GRANULES)
		shift++;
	needed = (size_t)((record_granules(length, shift) + 7) / 8);
	if (shift == record->shift && needed <= record->bytes)
		return (FSIMG_OK);
	prepared->shift = shift;
	prepared->bytes = needed;
	if (shift == record->shift && record->bytes * 2 > needed)
		prepared->bytes = record->bytes * 2 < most ?
		    record->bytes * 2 : most;
	prepared->bits = fsimg_alloc(root->ctx, prepared->bytes, &detail);
	if (prepared->bits == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	if (shift == record->shift) {
		if (record->bytes != 0)
			bcopy(record->bits, prepared->bits, record->bytes);
		return (FSIMG_OK);
	}
	granules = (uint64_t)record->bytes * 8;
	for (granule = record_find(record, 0, granules, GRANULE_MARKED);
	    granule < granules;
	    granule = record_find(record, granule + 1, granules,
	    GRANULE_MARKED)) {
		uint64_t folded = granule >> (shift - record->shift);

		prepared->bits[folded / 8] |= (unsigned char)(1u << folded % 8);
	}
	return (FSIMG_OK);
}

/* Free a prepared record's bits unless it shares the committed ones. */
static void
record_discard(const fsimg_io_t *root, written_record_t *prepared)
{
	if (prepared->bits != root->record.bits)
		fsimg_dealloc(prepared->bits);
}

/*
 * Commit a prepared record once the host resize to length succeeded. On a
 * shrink the granules wholly beyond length are cleared, since a later
 * growth reads as zero there; a granule straddling length keeps its bit.
 * Bits beyond the old length are already clear, so only the removed
 * granules are visited.
 */
static void
record_commit(fsimg_io_t *root, written_record_t *prepared, uint64_t length)
{
	written_record_t *record = &root->record;
	uint64_t first, old;

	if (prepared->bits != record->bits)
		fsimg_dealloc(record->bits);
	*record = *prepared;
	first = record_granules(length, record->shift);
	old = record_granules(root->length, record->shift);
	if (old > first)
		record_clear(record, first, old);
}

/* Release what a root owns beyond its descriptor: a resolver's argument
 * and a fresh file's record.
 */
static void
root_release(fsimg_io_t *root)
{
	if (root->kind == ROOT_RESOLVER)
		root->resolver->release(root->argument);
	fsimg_dealloc(root->record.bits);
}

void
fsimg_test_io_record(const fsimg_io_t *io, unsigned *shift, size_t *bytes)
{
	*shift = io->root->record.shift;
	*bytes = io->root->record.bytes;
}

void
fsimg_io_ops_init(fsimg_io_ops_t *ops)
{
	bzero(ops, sizeof(*ops));
	ops->size = sizeof(*ops);
	ops->version = FSIMG_OPTIONS_VERSION;
	ops->max_transfer = 64 * 1024;
}

static fsimg_status_t
callback_result(fsimg_status_t status, fsimg_error_t *error)
{
	if (status == FSIMG_OK) {
		fsimg_error_clear(error);
		return (FSIMG_OK);
	}
	if (status > FSIMG_OK || status < FSIMG_EDURABILITY)
		return (fsimg_error_set(error, FSIMG_EIO, 0,
		    FSIMG_OP_IO, "invalid callback status"));
	/* A callback may return only a status, even when details were requested. */
	if (error != NULL && error->status != status)
		fsimg_error_set(error, status, 0, FSIMG_OP_IO,
		    "I/O callback failed");
	return (status);
}

fsimg_status_t
fsimg_io_from_callbacks(fsimg_ctx_t *ctx, const fsimg_io_ops_t *ops,
    void *argument, fsimg_io_t **out, fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_io_t *io;
	uint64_t length;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL || ops == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "missing callback table or output"));
	if (ops->size != sizeof(*ops))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "unsupported callback table size"));
	if (ops->version != FSIMG_OPTIONS_VERSION ||
	    (ops->flags & ~FSIMG_IO_DURABLE) != 0 || ops->get_size == NULL ||
	    (ops->read_at == NULL && ops->write_at == NULL) ||
	    (ops->write_at == NULL && (ops->resize != NULL ||
	    ops->zero_range != NULL)) ||
	    (ops->read_at == NULL && ops->next_zero_range != NULL) ||
	    ((ops->flags & FSIMG_IO_DURABLE) != 0 && ops->flush == NULL))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "inconsistent I/O capabilities"));
	for (size_t i = 0; i < 4; i++) {
		if (ops->reserved[i] != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IO, "nonzero reserved I/O field"));
	}
	io = fsimg_alloc(ctx, sizeof(*io), &detail);
	if (io == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	length = 0;
	fsimg_error_clear(&detail);
	ctx->in_callback = true;
	status = ops->get_size(argument, &length, &detail);
	ctx->in_callback = false;
	status = callback_result(status, &detail);
	if (status != FSIMG_OK) {
		fsimg_dealloc(io);
		if (error != NULL)
			*error = detail;
		return (status);
	}
	io->ctx = ctx;
	io->root = io;
	io->fd = -1;
	io->kind = ROOT_CALLBACK;
	io->length = length;
	io->ops = *ops;
	if (io->ops.max_transfer == 0 || io->ops.max_transfer > SSIZE_MAX)
		io->ops.max_transfer = SSIZE_MAX;
	io->argument = argument;
	if (ops->read_at != NULL)
		io->access |= FSIMG_IO_READ;
	if (ops->write_at != NULL)
		io->access |= FSIMG_IO_WRITE;
	*out = io;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_resolver(fsimg_ctx_t *ctx, fsimg_io_t *backing, uint64_t length,
    const fsimg_io_resolver_ops_t *resolver, void *argument, fsimg_io_t **out,
    fsimg_error_t *error)
{
	fsimg_io_t *io;

	*out = NULL;
	io = fsimg_alloc(ctx, sizeof(*io), error);
	if (io == NULL) {
		resolver->release(argument);
		return (error->status);
	}
	io->ctx = ctx;
	io->root = io;
	io->fd = -1;
	io->kind = ROOT_RESOLVER;
	io->length = length;
	io->access = FSIMG_IO_READ;
	io->ops.max_transfer = 64 * 1024;
	io->resolver = resolver;
	io->argument = argument;
	io->backing = backing;
	*out = io;
	return (FSIMG_OK);
}

static fsimg_status_t
io_from_fd(fsimg_ctx_t *ctx, int fd, unsigned access, bool reserved,
    fsimg_io_t **out, fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_io_t *io;
	struct stat st;
	fsimg_status_t status;
	int duplicate, flags, mode, saved;

	if (out != NULL)
		*out = NULL;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL || access == 0 ||
	    (access & ~(FSIMG_IO_READ | FSIMG_IO_WRITE)) != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "invalid descriptor access or output"));
	flags = fcntl(fd, F_GETFL);
	if (flags == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_IO, "descriptor flags unavailable"));
	mode = flags & O_ACCMODE;
	if (((access & FSIMG_IO_READ) != 0 && mode == O_WRONLY) ||
	    ((access & FSIMG_IO_WRITE) != 0 && mode == O_RDONLY) ||
	    (access == FSIMG_IO_READ && mode != O_RDONLY) ||
	    ((access & FSIMG_IO_WRITE) != 0 && (flags & O_APPEND) != 0) ||
	    fsimg_host_unsupported_io(flags))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "descriptor flags incompatible with requested access"));
	if (fstat(fd, &st) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_IO, "descriptor stat failed"));
	if (!S_ISREG(st.st_mode) || st.st_size < 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "descriptor is not a regular file"));
	io = fsimg_alloc(ctx, sizeof(*io), &detail);
	if (io == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	duplicate = fd;
	if (!reserved) {
		status = fsimg_fd_reserve(ctx, error);
		if (status != FSIMG_OK) {
			fsimg_dealloc(io);
			return (status);
		}
		duplicate = fsimg_host_dup(fd);
		if (duplicate == -1) {
			saved = errno;
			fsimg_fd_release(ctx, -1);
			fsimg_dealloc(io);
			return (fsimg_error_set(error, FSIMG_EIO, saved,
			    FSIMG_OP_IO, "descriptor duplication failed"));
		}
	}
	io->ctx = ctx;
	io->root = io;
	io->fd = duplicate;
	/* Every file root is external unless its adoption makes it fresh or
	 * scratch.
	 */
	io->kind = ROOT_EXTERNAL;
	io->access = access;
	io->length = (uint64_t)st.st_size;
	io->ops.max_transfer = 64 * 1024;
	io->ops.flags = FSIMG_IO_DURABLE;
	*out = io;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_from_fd(fsimg_ctx_t *ctx, int fd, unsigned access, fsimg_io_t **out,
    fsimg_error_t *error)
{
	return (io_from_fd(ctx, fd, access, false, out, error));
}

fsimg_status_t
fsimg_io_adopt_fd(fsimg_ctx_t *ctx, int fd, unsigned access, fsimg_io_t **out,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = io_from_fd(ctx, fd, access, true, out, error);
	if (status != FSIMG_OK)
		fsimg_fd_release(ctx, fd);
	return (status);
}

fsimg_status_t
fsimg_io_adopt_scratch(fsimg_ctx_t *ctx, int fd, fsimg_io_t **out,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_io_adopt_fd(ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE, out,
	    error);
	if (status != FSIMG_OK)
		return (status);
	(*out)->length = INT64_MAX;
	(*out)->kind = ROOT_SCRATCH;
	return (FSIMG_OK);
}

static fsimg_status_t
io_range(fsimg_io_t *io, uint64_t offset, uint64_t length,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_ctx_ready(io == NULL ? NULL : io->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (!fsimg_range_fits(offset, length, io->length) ||
	    !fsimg_range_fits(io->base, io->length, io->root->length))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_IO, "request outside I/O object"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_slice(fsimg_io_t *parent, uint64_t offset, uint64_t length,
    fsimg_io_t **out, fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_io_t *slice;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	status = io_range(parent, offset, length, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "missing slice output"));
	if (parent->root->kind == ROOT_SCRATCH)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "scratch object cannot be sliced"));
	slice = fsimg_alloc(parent->ctx, sizeof(*slice), &detail);
	if (slice == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	/* Flatten translation, so nested slices never recurse during I/O. */
	slice->ctx = parent->ctx;
	slice->root = parent->root;
	slice->base = parent->base + offset;
	slice->length = length;
	slice->access = parent->access;
	slice->fd = -1;
	*out = slice;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_read_slice(fsimg_io_t *parent, uint64_t offset, uint64_t length,
    fsimg_io_t **out, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_io_slice(parent, offset, length, out, error);
	if (status == FSIMG_OK)
		(*out)->access &= ~(unsigned)(FSIMG_IO_WRITE | FSIMG_IO_DURABLE);
	return (status);
}

fsimg_status_t
fsimg_io_close_output(fsimg_io_t *io, fsimg_error_t *error)
{
	int result, saved;

	result = close(io->fd);
	saved = errno;
	fsimg_fd_release(io->ctx, -1);
	root_release(io);
	fsimg_dealloc(io);
	if (result == -1)
		return (fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_IO, "output file close failed"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_set_times(fsimg_io_t *io, const struct timespec *times,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = io_range(io, 0, 0, error);
	if (status != FSIMG_OK)
		return (status);
	if (io->root != io || io->fd == -1 || (io->access & FSIMG_IO_WRITE) == 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_EXTRACT, "timestamp restoration requires an owned output file"));
	if (futimens(io->fd, times) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_EXTRACT, "output file timestamp restoration failed"));
	return (FSIMG_OK);
}

void
fsimg_io_free(fsimg_io_t *io)
{
	if (io == NULL || io->ctx->in_callback)
		return;
	if (io->fd != -1)
		fsimg_fd_release(io->ctx, io->fd);
	if (io->root == io)
		root_release(io);
	fsimg_dealloc(io);
}

fsimg_status_t
fsimg_io_get_size(fsimg_io_t *io, uint64_t *out, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (out != NULL)
		*out = 0;
	status = io_range(io, 0, 0, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "missing size output"));
	if (io->root->kind == ROOT_SCRATCH)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "scratch object has no size"));
	*out = io->length;
	return (FSIMG_OK);
}

static fsimg_status_t
adapter_transfer(fsimg_io_t *root, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, bool writing, fsimg_error_t *error)
{
	off_t host_offset;
	ssize_t count;
	fsimg_status_t status;

	*transferred = 0;
	fsimg_error_clear(error);
	if (root->kind == ROOT_RESOLVER)
		return (root->resolver->resolve(root->argument, offset, buffer,
		    length, transferred, error));
	if (root->fd != -1) {
		if (!fsimg_host_offset(offset, &host_offset))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_IO, "offset exceeds host range"));
		if (writing)
			count = pwrite(root->fd, buffer, length, host_offset);
		else
			count = pread(root->fd, buffer, length, host_offset);
		if (count == -1)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_IO, "positional I/O failed"));
		*transferred = (size_t)count;
		return (FSIMG_OK);
	}
	root->ctx->in_callback = true;
	if (writing)
		status = root->ops.write_at(root->argument, offset, buffer,
		    length, transferred, error);
	else
		status = root->ops.read_at(root->argument, offset, buffer,
		    length, transferred, error);
	root->ctx->in_callback = false;
	if (*transferred > length) {
		*transferred = 0;
		return (fsimg_error_set(error, FSIMG_EIO, 0,
		    FSIMG_OP_IO, "callback over-reported transfer"));
	}
	return (callback_result(status, error));
}

static fsimg_status_t
io_transfer(fsimg_io_t *io, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, bool writing, bool exact, fsimg_session_t *session,
    fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_progress_t progress;
	fsimg_io_t *root;
	size_t batch, count, done;
	fsimg_status_t status;
	unsigned interruptions;

	if (transferred != NULL)
		*transferred = 0;
	status = io_range(io, offset, length, error);
	if (status != FSIMG_OK)
		return (status);
	if (transferred == NULL || (buffer == NULL && length != 0))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "missing transfer output or buffer"));
	if ((io->access & (writing ? FSIMG_IO_WRITE : FSIMG_IO_READ)) == 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "I/O access not supported"));
	if (length == 0)
		return (FSIMG_OK);

	root = io->root;
	if (session == NULL)
		session = root->session;
	/* Before the host or a callback sees the write, so a failed or short
	 * write leaves its whole requested range possibly nonzero.
	 */
	if (writing && root->kind == ROOT_FRESH)
		record_mark(root, io->base + offset, length);
	done = 0;
	interruptions = 0;
	bzero(&progress, sizeof(progress));
	progress.phase = writing ? "write" : "read";
	while (done < length) {
		progress.bytes = done;
		status = fsimg_poll(io->ctx, &progress, &detail);
		if (status != FSIMG_OK)
			break;
		batch = length - done;
		if (batch > root->ops.max_transfer)
			batch = root->ops.max_transfer;
		if (session != NULL) {
			status = fsimg_session_charge(session, 0, 0, 1, &detail);
			if (status != FSIMG_OK)
				break;
		}
		status = adapter_transfer(root, io->base + offset + done,
		    (unsigned char *)buffer + done, batch, &count, writing,
		    &detail);
		fsimg_stat_add(&io->ctx->stats.io_calls, 1);
		done += count;
		if (writing)
			fsimg_stat_add(&io->ctx->stats.written_bytes, count);
		else
			fsimg_stat_add(&io->ctx->stats.read_bytes, count);
		if (status == FSIMG_EIO && detail.os_error == EINTR) {
			/* Confirmed progress survives interruption; never replay it. */
			if (count != 0)
				interruptions = 0;
			else if (++interruptions > 1024)
				break;
			status = FSIMG_OK;
			fsimg_error_clear(&detail);
			continue;
		}
		if (status != FSIMG_OK)
			break;
		interruptions = 0;
		if (count == 0) {
			if (writing || exact)
				status = fsimg_error_set(&detail, FSIMG_EIO, 0,
				    FSIMG_OP_IO, "unexpected EOF or zero progress");
			break;
		}
		if (!exact)
			break;
	}
	*transferred = done;
	if (status != FSIMG_OK) {
		if (error != NULL) {
			*error = detail;
			error->offset = offset + done;
			error->offset_valid = true;
			if (writing && done != 0)
				error->output_state = FSIMG_OUTPUT_INCOMPLETE;
		}
		return (status);
	}
	fsimg_error_clear(error);
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_read_at(fsimg_io_t *io, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	return (io_transfer(io, offset, buffer, length, transferred, false,
	    false, NULL, error));
}

fsimg_status_t
fsimg_io_write_at(fsimg_io_t *io, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	return (io_transfer(io, offset, (void *)buffer, length, transferred,
	    true, false, NULL, error));
}

fsimg_status_t
fsimg_io_read_exact(fsimg_io_t *io, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	return (io_transfer(io, offset, buffer, length, transferred, false,
	    true, NULL, error));
}

fsimg_status_t
fsimg_io_write_exact(fsimg_io_t *io, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	return (io_transfer(io, offset, (void *)buffer, length, transferred,
	    true, true, NULL, error));
}

fsimg_status_t
fsimg_io_read_session(fsimg_io_t *io, fsimg_session_t *session,
    uint64_t offset, void *buffer, size_t length, size_t *transferred,
    fsimg_error_t *error)
{
	return (io_transfer(io, offset, buffer, length, transferred, false,
	    true, session, error));
}

fsimg_status_t
fsimg_io_write_session(fsimg_io_t *io, fsimg_session_t *session,
    uint64_t offset, const void *buffer, size_t length, size_t *transferred,
    fsimg_error_t *error)
{
	return (io_transfer(io, offset, (void *)buffer, length, transferred, true,
	    true, session, error));
}

fsimg_status_t
fsimg_io_allocated_bytes(fsimg_io_t *io, uint64_t *bytes,
    fsimg_error_t *error)
{
	struct stat st;

	*bytes = 0;
	if (io->root->fd == -1)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "I/O object has no file"));
	if (fstat(io->root->fd, &st) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno, FSIMG_OP_IO,
		    "allocation stat failed"));
	if (st.st_blocks < 0 ||
	    !fsimg_mul_u64((uint64_t)st.st_blocks, 512, bytes))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_IO,
		    "allocation size overflow"));
	return (FSIMG_OK);
}

/*
 * A fresh root's record is prepared for the new length before the host
 * resize and committed only after it succeeds, so a failure at either
 * step leaves the record describing the file as it is.
 */
fsimg_status_t
fsimg_io_resize(fsimg_io_t *io, uint64_t length, fsimg_error_t *error)
{
	written_record_t prepared;
	off_t host_length;
	fsimg_status_t status;
	int saved;

	status = io_range(io, 0, 0, error);
	if (status != FSIMG_OK)
		return (status);
	if (io->root != io || (io->access & FSIMG_IO_WRITE) == 0 ||
	    io->kind == ROOT_SCRATCH || (io->fd == -1 && io->ops.resize == NULL))
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "I/O object cannot resize"));
	status = fsimg_poll(io->ctx, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	if (io->fd != -1) {
		if (!fsimg_host_offset(length, &host_length))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_IO, "length exceeds host range"));
		prepared = io->record;
		if (io->kind == ROOT_FRESH) {
			status = record_prepare(io, length, &prepared, error);
			if (status != FSIMG_OK)
				return (status);
		}
		fsimg_stat_add(&io->ctx->stats.io_calls, 1);
		if (ftruncate(io->fd, host_length) == -1) {
			saved = errno;
			record_discard(io, &prepared);
			return (fsimg_error_set(error, FSIMG_EIO, saved,
			    FSIMG_OP_IO, "resize failed"));
		}
		if (io->kind == ROOT_FRESH)
			record_commit(io, &prepared, length);
	} else {
		io->ctx->in_callback = true;
		status = io->ops.resize(io->argument, length, error);
		io->ctx->in_callback = false;
		status = callback_result(status, error);
		fsimg_stat_add(&io->ctx->stats.io_calls, 1);
		if (status != FSIMG_OK)
			return (status);
	}
	io->length = length;
	return (FSIMG_OK);
}

/*
 * Write zeros over [offset, offset + length) of io in bounded batches,
 * through the zero-range callback when the root has one, else from a
 * charged zero buffer allocated into *zeros at its first use. *attempted
 * records that a mutation was attempted.
 */
static fsimg_status_t
zero_piece(fsimg_io_t *io, fsimg_session_t *session, uint64_t offset,
    uint64_t length, unsigned char **zeros, bool *attempted,
    fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_io_t *root = io->root;
	uint64_t done;
	size_t batch, transferred;
	fsimg_status_t status;

	if (root->ops.zero_range == NULL && *zeros == NULL) {
		*zeros = fsimg_alloc(io->ctx, 64 * 1024, &detail);
		if (*zeros == NULL) {
			if (error != NULL)
				*error = detail;
			return (detail.status);
		}
	}
	status = FSIMG_OK;
	for (done = 0; done < length; done += batch) {
		batch = length - done > 64 * 1024 ? 64 * 1024 :
		    (size_t)(length - done);
		status = fsimg_poll(io->ctx, NULL, error);
		if (status != FSIMG_OK)
			break;
		*attempted = true;
		if (root->ops.zero_range == NULL) {
			/* On a fresh root these bytes lie in marked granules,
			 * so the write marks nothing new.
			 */
			status = io_transfer(io, offset + done, *zeros, batch,
			    &transferred, true, true, session, error);
		} else {
			if (session != NULL) {
				status = fsimg_session_charge(session, 0, 0, 1, error);
				if (status != FSIMG_OK)
					break;
			}
			io->ctx->in_callback = true;
			status = root->ops.zero_range(root->argument,
			    io->base + offset + done, batch, error);
			io->ctx->in_callback = false;
			status = callback_result(status, error);
			fsimg_stat_add(&io->ctx->stats.io_calls, 1);
			if (status == FSIMG_OK)
				fsimg_stat_add(&io->ctx->stats.written_bytes, batch);
		}
		if (status != FSIMG_OK)
			break;
	}
	return (status);
}

/*
 * Make a range read as zero. A fresh root's record is the one proof that lets
 * a write be omitted: zeros are written only over the possibly-nonzero
 * granules the range meets, clipped to the range, and the range costs one unit
 * of work however little is written, so zeroing a vast clear range still polls
 * cancellation. Every other object is zeroed in full, whatever it reports
 * about its zero ranges.
 */
static fsimg_status_t
io_zero(fsimg_io_t *io, fsimg_session_t *session, uint64_t offset,
    uint64_t length, fsimg_error_t *error)
{
	fsimg_io_t *root;
	unsigned char *zeros;
	uint64_t root_offset, root_end, start, end;
	fsimg_status_t status;
	bool attempted;

	status = io_range(io, offset, length, error);
	if (status != FSIMG_OK)
		return (status);
	if ((io->access & FSIMG_IO_WRITE) == 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "I/O object is not writable"));
	if (length == 0)
		return (FSIMG_OK);
	root = io->root;
	if (session == NULL)
		session = root->session;
	zeros = NULL;
	attempted = false;
	if (root->kind != ROOT_FRESH) {
		status = zero_piece(io, session, offset, length, &zeros,
		    &attempted, error);
	} else {
		/* One unit of work for the range, however little is written. */
		if (session != NULL)
			status = fsimg_session_charge(session, 0, 0, 1, error);
		else
			status = fsimg_poll(io->ctx, NULL, error);
		/* Zero each marked run the range meets, in root coordinates. */
		root_offset = io->base + offset;
		root_end = root_offset + length;
		while (status == FSIMG_OK && root_offset < root_end) {
			record_next(root, root_offset, root_end, GRANULE_MARKED,
			    &start, &end);
			if (start == end)
				break;
			status = zero_piece(io, session, start - io->base,
			    end - start, &zeros, &attempted, error);
			root_offset = end;
		}
	}
	fsimg_dealloc(zeros);
	if (status != FSIMG_OK && attempted && error != NULL)
		error->output_state = FSIMG_OUTPUT_INCOMPLETE;
	return (status);
}

fsimg_status_t
fsimg_io_zero_range(fsimg_io_t *io, uint64_t offset, uint64_t length,
    fsimg_error_t *error)
{
	return (io_zero(io, NULL, offset, length, error));
}

fsimg_status_t
fsimg_io_zero_session(fsimg_io_t *io, fsimg_session_t *session,
    uint64_t offset, uint64_t length, fsimg_error_t *error)
{
	return (io_zero(io, session, offset, length, error));
}

fsimg_status_t
fsimg_io_flush(fsimg_io_t *io, bool durable, fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_io_t *root;
	fsimg_status_t status;

	status = io_range(io, 0, 0, error);
	if (status != FSIMG_OK)
		return (status);
	root = io->root;
	if (durable && (root->ops.flags & FSIMG_IO_DURABLE) == 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "durable flush not supported"));
	for (unsigned interruptions = 0;; interruptions++) {
		status = fsimg_poll(io->ctx, NULL, &detail);
		if (status != FSIMG_OK)
			break;
		fsimg_error_clear(&detail);
		if (root->fd != -1) {
			if (fsync(root->fd) == 0 &&
			    root->ctx->hole_test.flush_errno == 0)
				return (FSIMG_OK);
			if (root->ctx->hole_test.flush_errno != 0)
				errno = root->ctx->hole_test.flush_errno;
			status = fsimg_error_set(&detail, FSIMG_EIO, errno,
			    FSIMG_OP_IO, "flush failed");
		} else {
			if (root->ops.flush == NULL)
				return (FSIMG_OK);
			io->ctx->in_callback = true;
			status = root->ops.flush(root->argument, &detail);
			io->ctx->in_callback = false;
			status = callback_result(status, &detail);
		}
		if (status != FSIMG_EIO || detail.os_error != EINTR ||
		    interruptions == 1024)
			break;
	}
	if (error != NULL)
		*error = detail;
	return (status);
}
