/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "host.h"
#include "internal.h"
#include "private.h"

/* The union preserves malloc alignment; its full size is budgeted. */
typedef union fsimg_allocation {
	max_align_t	 alignment;
	struct {
		fsimg_ctx_t	*ctx;
		size_t		 charged;
	} owner;
} fsimg_allocation_t;

/* A position in a path being rendered. separators counts the slashes
 * passed, so a byte after the first lies in component separators - 1.
 */
typedef struct render_cursor {
	const unsigned char *at;
	const bool	*omitted;
	size_t		 separators;
} render_cursor_t;

/* One rendered unit: the input bytes it consumes and the bytes they
 * render to.
 */
typedef struct render_unit {
	size_t		 input;
	size_t		 output;
} render_unit_t;

static bool render_in_omitted(const render_cursor_t *);
static render_unit_t render_measure(const render_cursor_t *);
static void render_advance(render_cursor_t *, const render_unit_t *);
static size_t render_write(const render_cursor_t *, const render_unit_t *,
    char *);

bool
fsimg_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
	if (b > UINT64_MAX - a)
		return (false);
	*out = a + b;
	return (true);
}

bool
fsimg_mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
	if (a != 0 && b > UINT64_MAX / a)
		return (false);
	*out = a * b;
	return (true);
}

bool
fsimg_to_size(uint64_t value, size_t *out)
{
	if (value > SIZE_MAX)
		return (false);
	*out = (size_t)value;
	return (true);
}

bool
fsimg_range_fits(uint64_t offset, uint64_t count, uint64_t length)
{
	return (offset <= length && count <= length - offset);
}

void
fsimg_stat_add(uint64_t *value, uint64_t amount)
{
	if (!fsimg_add_u64(*value, amount, value))
		*value = UINT64_MAX;
}

uint64_t
fsimg_report_loss(fsimg_ctx_t *ctx, const char *path, uint32_t fields)
{
	uint64_t count;

	if (fields == 0)
		return (0);
	count = 0;
	for (uint32_t rest = fields & FSIMG_META_KNOWN; rest != 0; rest &= rest - 1)
		count++;
	fsimg_stat_add(&ctx->stats.metadata_losses, count);
	if (ctx->options.loss != NULL) {
		/* The buffer holds the rendering of any path within the path
		 * limit, which bounds every path reported here.
		 */
		(void)fsimg_path_render(path, NULL, ctx->loss_path,
		    ctx->loss_path_bytes);
		ctx->in_callback = true;
		ctx->options.loss(ctx->options.loss_argument, ctx->loss_path,
		    fields);
		ctx->in_callback = false;
	}
	return (count);
}

static bool
render_in_omitted(const render_cursor_t *cursor)
{
	return (cursor->omitted != NULL && cursor->separators != 0 &&
	    cursor->omitted[cursor->separators - 1]);
}

/* The unit at the cursor: a backslash, rendered as two; in an omitted
 * component one byte, rendered as four unless printable ASCII; otherwise
 * one byte, or a UTF-8 lead byte with the continuation bytes after it, kept
 * whole so truncation never splits a sequence. A separator is one byte.
 */
static render_unit_t
render_measure(const render_cursor_t *cursor)
{
	const unsigned char *at = cursor->at;
	render_unit_t unit = { .input = 1, .output = 1 };

	if (at[0] == '\\') {
		unit.output = 2;
	} else if (at[0] != '/' && render_in_omitted(cursor)) {
		if (at[0] < 0x20 || at[0] > 0x7e)
			unit.output = 4;
	} else if (at[0] >= 0x80) {
		while (unit.input < 4 && (at[unit.input] & 0xc0) == 0x80)
			unit.input++;
		unit.output = unit.input;
	}
	return (unit);
}

static void
render_advance(render_cursor_t *cursor, const render_unit_t *unit)
{
	if (cursor->at[0] == '/')
		cursor->separators++;
	cursor->at += unit->input;
}

/* Write the unit at the cursor to out, which has room for it and a NUL. */
static size_t
render_write(const render_cursor_t *cursor, const render_unit_t *unit,
    char *out)
{
	static const char hex[] = "0123456789abcdef";
	unsigned byte = cursor->at[0];

	if (byte == '\\') {
		out[0] = '\\';
		out[1] = '\\';
	} else if (unit->output == 4) {
		out[0] = '\\';
		out[1] = 'x';
		out[2] = hex[byte >> 4];
		out[3] = hex[byte & 0xf];
	} else
		bcopy(cursor->at, out, unit->input);
	return (unit->output);
}

/* Measure the whole rendering, drop whole units from the front until the
 * rest fits beside the NUL, then write the rest. Two passes over the input
 * need no buffer beyond out, however long the path.
 */
size_t
fsimg_rendered_tail(const char *rendered, size_t length, size_t limit)
{
	const unsigned char *bytes = (const unsigned char *)rendered;
	size_t at, unit;

	for (at = 0; length - at > limit; at += unit) {
		unit = 1;
		if (bytes[at] == '\\')
			unit = bytes[at + 1] == 'x' ? 4 : 2;
		else if (bytes[at] >= 0x80) {
			while (unit < 4 && (bytes[at + unit] & 0xc0) == 0x80)
				unit++;
		}
		/* The rendering is NUL-terminated; a unit never passes its end. */
		if (unit > length - at)
			unit = length - at;
	}
	return (at);
}

bool
fsimg_path_render(const char *path, const bool *omitted, char *out,
    size_t size)
{
	render_cursor_t cursor;
	render_unit_t unit;
	uint64_t total, excess, dropped;
	size_t used;

	if (size == 0)
		return (true);
	cursor = (render_cursor_t){ .at = (const unsigned char *)path,
	    .omitted = omitted };
	total = 0;
	while (cursor.at[0] != '\0') {
		unit = render_measure(&cursor);
		total += unit.output;
		render_advance(&cursor, &unit);
	}

	cursor = (render_cursor_t){ .at = (const unsigned char *)path,
	    .omitted = omitted };
	excess = total > size - 1 ? total - (size - 1) : 0;
	for (dropped = 0; dropped < excess; dropped += unit.output) {
		unit = render_measure(&cursor);
		render_advance(&cursor, &unit);
	}

	used = 0;
	while (cursor.at[0] != '\0') {
		unit = render_measure(&cursor);
		used += render_write(&cursor, &unit, out + used);
		render_advance(&cursor, &unit);
	}
	out[used] = '\0';
	return (excess != 0);
}

void
fsimg_error_path(fsimg_error_t *error, const char *path)
{
	if (error == NULL)
		return;
	error->path_truncated = fsimg_path_render(path, NULL, error->path,
	    sizeof(error->path));
}

void
fsimg_error_clear(fsimg_error_t *error)
{
	if (error != NULL)
		bzero(error, sizeof(*error));
}

fsimg_status_t
fsimg_error_set(fsimg_error_t *error, fsimg_status_t status, int os_error,
    fsimg_operation_t operation, const char *message)
{
	int count;

	if (error != NULL) {
		fsimg_error_clear(error);
		error->status = status;
		error->os_error = os_error;
		error->operation = operation;
		count = snprintf(error->message, sizeof(error->message),
		    "%s", message);
		error->message_truncated = count < 0 ||
		    (size_t)count >= sizeof(error->message);
	}
	return (status);
}

fsimg_status_t
fsimg_limit_error(fsimg_error_t *error, fsimg_resource_t resource,
    uint64_t limit, uint64_t observed)
{
	/* The context owns every resource ceiling; resource identifies which one. */
	fsimg_error_set(error, FSIMG_ELIMIT, 0, FSIMG_OP_CONTEXT,
	    "configured resource limit exceeded");
	if (error != NULL) {
		error->resource = resource;
		error->limit = limit;
		error->observed = observed;
	}
	return (FSIMG_ELIMIT);
}

const char *
fsimg_status_string(fsimg_status_t status)
{
	switch (status) {
	case FSIMG_OK: return ("success");
	case FSIMG_DONE: return ("end of iteration");
	case FSIMG_EINVAL: return ("invalid argument");
	case FSIMG_ESTATE: return ("invalid state");
	case FSIMG_EIO: return ("I/O failure");
	case FSIMG_EFORMAT: return ("unknown format");
	case FSIMG_ECORRUPT: return ("corrupt image");
	case FSIMG_EUNSUPPORTED: return ("unsupported capability");
	case FSIMG_ERANGE: return ("out of range");
	case FSIMG_ELIMIT: return ("resource limit");
	case FSIMG_ENOMEM: return ("allocation failure");
	case FSIMG_ECANCELLED: return ("cancelled");
	case FSIMG_ESOURCE_CHANGED: return ("source changed");
	case FSIMG_EEXISTS: return ("output exists");
	case FSIMG_EDURABILITY: return ("durability uncertain");
	}
	return ("unknown status");
}

void
fsimg_options_init(fsimg_options_t *opts)
{
	bzero(opts, sizeof(*opts));
	opts->size = sizeof(*opts);
	opts->version = FSIMG_OPTIONS_VERSION;
	opts->memory_bytes = UINT64_C(256) * 1024 * 1024;
	opts->scratch_bytes = UINT64_C(64) * 1024 * 1024 * 1024;
	opts->spool_bytes = UINT64_C(64) * 1024 * 1024 * 1024;
	opts->max_entries = 10000000;
	opts->max_depth = 256;
	opts->max_path_bytes = 64 * 1024;
	opts->max_open_files = 128;
	opts->scratch_directory_fd = -1;
}

/* Hold fd as the context's scratch directory, together with its identity,
 * which stays fixed while the descriptor is held; the source scanner
 * compares objects against it.
 */
static void
take_scratch(fsimg_ctx_t *ctx, int fd, uint64_t device, uint64_t inode)
{
	ctx->options.scratch_directory_fd = fd;
	ctx->scratch_device = device;
	ctx->scratch_inode = inode;
}

fsimg_status_t
fsimg_ctx_new(const fsimg_options_t *opts, fsimg_ctx_t **out,
    fsimg_error_t *error)
{
	fsimg_options_t defaults, options;
	fsimg_error_t detail;
	fsimg_ctx_t *ctx;
	struct stat st;
	uint64_t loss_bytes;
	fsimg_status_t status;
	int fd, saved;

	fsimg_error_clear(error);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "missing context output"));
	*out = NULL;
	fsimg_options_init(&defaults);
	options = defaults;
	if (opts != NULL) {
		/* Inspect size first; never read the rest of an undersized object. */
		if (opts->size != sizeof(*opts))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "unsupported options size"));
		if (opts->version != FSIMG_OPTIONS_VERSION || opts->flags != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "unsupported options version or flags"));
		for (size_t i = 0; i < sizeof(opts->reserved) /
		    sizeof(opts->reserved[0]); i++) {
			if (opts->reserved[i] != 0)
				return (fsimg_error_set(error, FSIMG_EINVAL, 0,
				    FSIMG_OP_CONTEXT, "nonzero reserved option"));
		}
		options = *opts;
	}
#define DEFAULT_LIMIT(field) do { \
	if (options.field == 0) options.field = defaults.field; \
} while (0)
	DEFAULT_LIMIT(memory_bytes);
	DEFAULT_LIMIT(scratch_bytes);
	DEFAULT_LIMIT(spool_bytes);
	DEFAULT_LIMIT(max_entries);
	DEFAULT_LIMIT(max_depth);
	DEFAULT_LIMIT(max_path_bytes);
	DEFAULT_LIMIT(max_open_files);
#undef DEFAULT_LIMIT
	if (options.memory_bytes < UINT64_C(16) * 1024 * 1024 ||
	    options.scratch_directory_fd < -1)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "invalid memory budget or scratch descriptor"));

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return (fsimg_error_set(error, FSIMG_ENOMEM, 0,
		    FSIMG_OP_CONTEXT, "context allocation failed"));
	ctx->options = options;
	ctx->options.scratch_directory_fd = -1;
	ctx->stats.memory_bytes = sizeof(*ctx);
	ctx->stats.memory_peak = sizeof(*ctx);
	/* A rendered path is at most four bytes per input byte. */
	if (options.loss != NULL) {
		if (!fsimg_mul_u64(options.max_path_bytes, 4, &loss_bytes) ||
		    !fsimg_add_u64(loss_bytes, 1, &loss_bytes) ||
		    !fsimg_to_size(loss_bytes, &ctx->loss_path_bytes)) {
			status = fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_CONTEXT, "loss path buffer size overflow");
			goto fail;
		}
		ctx->loss_path = fsimg_alloc(ctx, ctx->loss_path_bytes, &detail);
		if (ctx->loss_path == NULL) {
			status = detail.status;
			if (error != NULL)
				*error = detail;
			goto fail;
		}
	}
	if (options.scratch_directory_fd != -1) {
		if (fsimg_host_fstat(options.scratch_directory_fd, &st) == -1) {
			status = fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_CONTEXT, "scratch descriptor stat failed");
			goto fail;
		}
		if (!S_ISDIR(st.st_mode)) {
			status = fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "scratch descriptor is not a directory");
			goto fail;
		}
		status = fsimg_fd_reserve(ctx, error);
		if (status != FSIMG_OK)
			goto fail;
		fd = fsimg_host_dup(options.scratch_directory_fd);
		if (fd == -1) {
			saved = errno;
			fsimg_fd_release(ctx, -1);
			status = fsimg_error_set(error, FSIMG_EIO, saved,
			    FSIMG_OP_CONTEXT, "scratch descriptor duplication failed");
			goto fail;
		}
		take_scratch(ctx, fd, (uint64_t)st.st_dev, (uint64_t)st.st_ino);
	}
	*out = ctx;
	return (FSIMG_OK);
fail:
	fsimg_dealloc(ctx->loss_path);
	free(ctx);
	return (status);
}

void
fsimg_ctx_free(fsimg_ctx_t *ctx)
{
	if (ctx == NULL || ctx->in_callback)
		return;
	if (ctx->options.scratch_directory_fd != -1)
		fsimg_fd_release(ctx, ctx->options.scratch_directory_fd);
	fsimg_dealloc(ctx->loss_path);
	free(ctx);
}

fsimg_status_t
fsimg_ctx_adopt_scratch(fsimg_ctx_t *ctx, int fd, fsimg_error_t *error)
{
	fsimg_host_file_t identity;
	fsimg_status_t status;

	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		goto fail;
	if (ctx->options.scratch_directory_fd != -1 || ctx->meta_cache != NULL) {
		status = fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_CONTEXT, "scratch adoption requires a fresh context");
		goto fail;
	}
	if (fsimg_host_file(fd, &identity) == -1 || identity.kind != FSIMG_HOST_DIRECTORY) {
		status = fsimg_error_set(error, FSIMG_EINVAL, errno,
		    FSIMG_OP_CONTEXT, "scratch adoption requires a directory");
		goto fail;
	}
	take_scratch(ctx, fd, identity.device, identity.inode);
	return (FSIMG_OK);
fail:
	fsimg_fd_release(ctx, fd);
	return (status);
}

fsimg_status_t
fsimg_ctx_ready(fsimg_ctx_t *ctx, fsimg_error_t *error)
{
	fsimg_error_clear(error);
	if (ctx == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "missing context"));
	if (ctx->in_callback)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_CONTEXT, "callback cannot re-enter its context"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_ctx_get_stats(fsimg_ctx_t *ctx, fsimg_stats_t *out, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "missing statistics output"));
	*out = ctx->stats;
	return (FSIMG_OK);
}

void *
fsimg_alloc(fsimg_ctx_t *ctx, size_t bytes, fsimg_error_t *error)
{
	fsimg_allocation_t *allocation;
	uint64_t observed;
	size_t charged;

	if (fsimg_ctx_ready(ctx, error) != FSIMG_OK)
		return (NULL);
	if (bytes > SIZE_MAX - sizeof(*allocation)) {
		fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_ALLOCATE,
		    "allocation size overflow");
		return (NULL);
	}
	charged = sizeof(*allocation) + bytes;
	observed = ctx->stats.memory_bytes;
	fsimg_stat_add(&observed, charged);
	if (observed > ctx->options.memory_bytes) {
		fsimg_limit_error(error, FSIMG_RESOURCE_MEMORY,
		    ctx->options.memory_bytes, observed);
		return (NULL);
	}
	fsimg_stat_add(&ctx->allocation_attempts, 1);
	if (ctx->fail_allocation != 0 &&
	    ctx->allocation_attempts == ctx->fail_allocation)
		allocation = NULL;
	else
		allocation = calloc(1, charged);
	if (allocation == NULL) {
		fsimg_error_set(error, FSIMG_ENOMEM, 0, FSIMG_OP_ALLOCATE,
		    "allocation failed");
		return (NULL);
	}
	if (ctx->poison_allocations)
		memset(allocation + 1, 0xa5, bytes);
	allocation->owner.ctx = ctx;
	allocation->owner.charged = charged;
	ctx->stats.memory_bytes = observed;
	if (observed > ctx->stats.memory_peak)
		ctx->stats.memory_peak = observed;
	return (allocation + 1);
}

void
fsimg_dealloc(void *pointer)
{
	fsimg_allocation_t *allocation;

	if (pointer == NULL)
		return;
	allocation = (fsimg_allocation_t *)pointer - 1;
	allocation->owner.ctx->stats.memory_bytes -= allocation->owner.charged;
	free(allocation);
}

void
fsimg_test_fail_alloc_after(fsimg_ctx_t *ctx, uint64_t attempt)
{
	ctx->allocation_attempts = 0;
	ctx->fail_allocation = attempt;
}

void
fsimg_test_hole_discovery(fsimg_ctx_t *ctx, bool enabled)
{
	ctx->no_hole_discovery = !enabled;
}

fsimg_hole_test_t *
fsimg_test_holes(fsimg_ctx_t *ctx)
{
	return (&ctx->hole_test);
}

void
fsimg_test_poison_allocations(fsimg_ctx_t *ctx, bool enabled)
{
	ctx->poison_allocations = enabled;
}

void
fsimg_test_transcode_flip(fsimg_ctx_t *ctx, uint64_t offset)
{
	ctx->transcode_flip = offset == UINT64_MAX ? 0 : offset + 1;
}

void
fsimg_test_unproven_writes(fsimg_ctx_t *ctx, bool enabled)
{
	ctx->unproven_writes = enabled;
}

void
fsimg_test_misplace_boot_header(fsimg_ctx_t *ctx, bool enabled)
{
	ctx->misplace_boot_header = enabled;
}

fsimg_status_t
fsimg_fd_reserve(fsimg_ctx_t *ctx, fsimg_error_t *error)
{
	if (ctx->stats.descriptors >= ctx->options.max_open_files)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_DESCRIPTORS,
		    ctx->options.max_open_files, ctx->stats.descriptors + 1));
	ctx->stats.descriptors++;
	if (ctx->stats.descriptors > ctx->stats.descriptors_peak)
		ctx->stats.descriptors_peak = ctx->stats.descriptors;
	return (FSIMG_OK);
}

void
fsimg_fd_release(fsimg_ctx_t *ctx, int fd)
{
	if (fd != -1)
		close(fd);
	ctx->stats.descriptors--;
}

fsimg_status_t
fsimg_private_mkdir(fsimg_ctx_t *ctx, int parent, const char *name,
    fsimg_operation_t operation, int *out, bool *created, fsimg_error_t *error)
{
	fsimg_host_privacy_t verdict;
	fsimg_host_fresh_t outcome;
	fsimg_status_t status;
	char message[96];
	int saved;

	*out = -1;
	*created = false;
	/* The directory, and the transient second descriptor the check reads
	 * it through.
	 */
	status = fsimg_fd_reserve(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fd_reserve(ctx, error);
	if (status != FSIMG_OK) {
		fsimg_fd_release(ctx, -1);
		return (status);
	}
	outcome = fsimg_host_private_mkdir(parent, name, out, &verdict, created);
	saved = errno;
	fsimg_fd_release(ctx, -1);
	if (outcome == FSIMG_HOST_FRESH_CREATED)
		return (FSIMG_OK);
	fsimg_fd_release(ctx, -1);
	switch (outcome) {
	case FSIMG_HOST_FRESH_TAKEN:
		return (fsimg_error_set(error, FSIMG_EEXISTS, saved, operation,
		    "directory name already exists"));
	case FSIMG_HOST_FRESH_REFUSED:
		snprintf(message, sizeof(message), "parent directory %s",
		    fsimg_host_fresh_refusal(verdict));
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, operation, message));
	default:
		return (fsimg_error_set(error, FSIMG_EIO, saved, operation,
		    "private directory creation failed"));
	}
}

fsimg_status_t
fsimg_poll(fsimg_ctx_t *ctx, const fsimg_progress_t *progress,
    fsimg_error_t *error)
{
	bool cancelled;

	cancelled = false;
	ctx->in_callback = true;
	if (ctx->options.cancelled != NULL)
		cancelled = ctx->options.cancelled(ctx->options.cancel_argument);
	if (!cancelled && ctx->options.progress != NULL && progress != NULL)
		ctx->options.progress(ctx->options.progress_argument, progress);
	ctx->in_callback = false;
	if (cancelled)
		return (fsimg_error_set(error, FSIMG_ECANCELLED, 0,
		    FSIMG_OP_CONTEXT, "operation cancelled"));
	return (FSIMG_OK);
}

void
fsimg_session_init(fsimg_session_t *session, fsimg_ctx_t *ctx,
    const char *phase, uint64_t volume_bytes)
{
	bzero(session, sizeof(*session));
	session->ctx = ctx;
	session->progress.phase = phase;
	session->output_limit = ctx->options.max_output_bytes == 0 ?
	    volume_bytes : ctx->options.max_output_bytes;
}

fsimg_status_t
fsimg_session_charge(fsimg_session_t *session, uint64_t entries,
    uint64_t output_bytes, uint64_t work, fsimg_error_t *error)
{
	fsimg_ctx_t *ctx;
	uint64_t new_entries, new_output, new_work;
	fsimg_status_t status;

	ctx = session->ctx;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	new_entries = session->progress.entries;
	new_output = session->output_bytes;
	new_work = session->progress.work;
	fsimg_stat_add(&new_entries, entries);
	fsimg_stat_add(&new_output, output_bytes);
	fsimg_stat_add(&new_work, work);
	if (entries > UINT64_MAX - session->progress.entries ||
	    new_entries > ctx->options.max_entries)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_ENTRIES,
		    ctx->options.max_entries, new_entries));
	if (output_bytes > UINT64_MAX - session->output_bytes ||
	    new_output > session->output_limit)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_OUTPUT,
		    session->output_limit, new_output));
	if (work > UINT64_MAX - session->progress.work ||
	    (ctx->options.max_work != 0 && new_work > ctx->options.max_work))
		return (fsimg_limit_error(error, FSIMG_RESOURCE_WORK,
		    ctx->options.max_work, new_work));
	status = fsimg_poll(ctx, &session->progress, error);
	if (status != FSIMG_OK)
		return (status);
	session->progress.entries = new_entries;
	session->progress.bytes = new_output;
	session->progress.work = new_work;
	session->output_bytes = new_output;
	return (FSIMG_OK);
}
