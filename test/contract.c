/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "contract.h"
#include "host.h"
#include "internal.h"
#include "meta.h"
#include "writer.h"

contract_run_t contract_run;

void
contract_init(int argc, char **argv)
{
	bzero(&contract_run, sizeof(contract_run));
	if (argc > 2) {
		fprintf(stderr, "usage: %s [profile/scenario]\n", argv[0]);
		exit(2);
	}
	contract_run.selector = argc == 2 ? argv[1] : NULL;
}

/* Whether one selector part, possibly "*", matches name. */
static bool
selector_part(const char *part, size_t length, const char *name)
{
	if (length == 1 && part[0] == '*')
		return (true);
	return (strlen(name) == length && strncmp(part, name, length) == 0);
}

/* Whether the replay selector, if any, chooses the case. */
static bool
selected(const char *profile, const char *scenario)
{
	const char *selector = contract_run.selector, *slash;

	if (selector == NULL)
		return (true);
	slash = strchr(selector, '/');
	if (slash == NULL)
		return (selector_part(selector, strlen(selector), profile));
	return (selector_part(selector, (size_t)(slash - selector), profile) &&
	    selector_part(slash + 1, strlen(slash + 1), scenario));
}

bool
contract_begin(const char *profile, const char *scenario)
{
	if (!selected(profile, scenario))
		return (false);
	CONTRACT_CHECK(contract_run.profile == NULL);
	contract_run.scheduled++;
	contract_run.profile = profile;
	contract_run.scenario = scenario;
	contract_run.phase = "setup";
	contract_run.fault = 0;
	return (true);
}

void
contract_phase(const char *phase, uint64_t fault)
{
	contract_run.phase = phase;
	contract_run.fault = fault;
}

void
contract_end(void)
{
	CONTRACT_CHECK(contract_run.profile != NULL);
	contract_run.completed++;
	contract_run.profile = NULL;
	contract_run.scenario = NULL;
	contract_run.phase = NULL;
}

void
contract_not_applicable(const char *profile, const char *scenario,
    const char *reason)
{
	if (!selected(profile, scenario))
		return;
	contract_run.not_applicable++;
	if (getenv("CONTRACT_VERBOSE") != NULL)
		printf("n/a %s/%s: %s\n", profile, scenario, reason);
}

int
contract_finish(const char *suite)
{
	CONTRACT_CHECK(contract_run.profile == NULL);
	CONTRACT_CHECK(contract_run.completed == contract_run.scheduled);
	if (contract_run.selector != NULL && contract_run.scheduled == 0) {
		fprintf(stderr, "%s: selector %s chose no case\n", suite,
		    contract_run.selector);
		return (1);
	}
	printf("%s: %llu cases passed, %llu not applicable\n", suite,
	    (unsigned long long)contract_run.completed,
	    (unsigned long long)contract_run.not_applicable);
	return (0);
}

static void
print_case(const char *file, int line)
{
	fprintf(stderr, "%s:%d: case %s/%s, phase %s", file, line,
	    contract_run.profile == NULL ? "-" : contract_run.profile,
	    contract_run.scenario == NULL ? "-" : contract_run.scenario,
	    contract_run.phase == NULL ? "-" : contract_run.phase);
	if (contract_run.fault != 0)
		fprintf(stderr, ", fault position %llu",
		    (unsigned long long)contract_run.fault);
	fprintf(stderr, "\n");
}

void
contract_fail(const char *file, int line, const char *condition)
{
	print_case(file, line);
	fprintf(stderr, "  failed: %s\n", condition);
	if (contract_run.profile != NULL)
		fprintf(stderr, "  replay: %s/%s\n", contract_run.profile,
		    contract_run.scenario);
	exit(1);
}

void
contract_fail_status(const char *file, int line, fsimg_status_t actual,
    fsimg_status_t expected, const fsimg_error_t *error)
{
	print_case(file, line);
	fprintf(stderr, "  status %d (%s), expected %d (%s)\n", actual,
	    fsimg_status_string(actual), expected,
	    fsimg_status_string(expected));
	if (error != NULL)
		fprintf(stderr, "  error: operation %d, resource %d, output %d, "
		    "path \"%s\": %s\n", error->operation, error->resource,
		    error->output_state, error->path, error->message);
	if (contract_run.profile != NULL)
		fprintf(stderr, "  replay: %s/%s\n", contract_run.profile,
		    contract_run.scenario);
	exit(1);
}

/* The canary at a backing offset outside the volume. */
static unsigned char
canary(uint64_t offset)
{
	return ((unsigned char)(0xc3 ^ (offset * 13)));
}

void
contract_volume_init(contract_volume_t *volume, uint64_t length,
    unsigned char fill)
{
	uint64_t total;

	bzero(volume, sizeof(*volume));
	CONTRACT_CHECK(length <= SIZE_MAX - 2 * CONTRACT_GUARD);
	total = length + 2 * CONTRACT_GUARD;
	volume->bytes = malloc((size_t)total);
	CONTRACT_CHECK(volume->bytes != NULL);
	volume->length = length;
	for (uint64_t i = 0; i < CONTRACT_GUARD; i++) {
		volume->bytes[i] = canary(i);
		volume->bytes[total - CONTRACT_GUARD + i] =
		    canary(total - CONTRACT_GUARD + i);
	}
	memset(volume->bytes + CONTRACT_GUARD, fill, (size_t)length);
}

void
contract_volume_free(contract_volume_t *volume)
{
	free(volume->bytes);
	volume->bytes = NULL;
}

void
contract_volume_reset(contract_volume_t *volume)
{
	unsigned char *bytes = volume->bytes;
	uint64_t length = volume->length;

	bzero(volume, sizeof(*volume));
	volume->bytes = bytes;
	volume->length = length;
}

unsigned char *
contract_volume_bytes(contract_volume_t *volume)
{
	return (volume->bytes + CONTRACT_GUARD);
}

bool
contract_volume_guards_intact(const contract_volume_t *volume)
{
	uint64_t tail = CONTRACT_GUARD + volume->length;

	for (uint64_t i = 0; i < CONTRACT_GUARD; i++) {
		if (volume->bytes[i] != canary(i) ||
		    volume->bytes[tail + i] != canary(tail + i))
			return (false);
	}
	return (true);
}

/* Every backing access lies within the volume; the driver addressed the
 * slice, so anything else escaped the slice adapter.
 */
static void
check_access(const contract_volume_t *volume, uint64_t offset,
    uint64_t length)
{
	CONTRACT_CHECK(offset >= CONTRACT_GUARD);
	CONTRACT_CHECK(offset - CONTRACT_GUARD < volume->length);
	CONTRACT_CHECK(length <= volume->length - (offset - CONTRACT_GUARD));
}

static fsimg_status_t
volume_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((contract_volume_t *)argument)->length + 2 * CONTRACT_GUARD;
	return (FSIMG_OK);
}

static fsimg_status_t
volume_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	contract_volume_t *volume = argument;

	*transferred = 0;
	check_access(volume, offset, length);
	volume->reads++;
	if (volume->reads == volume->fail_read) {
		volume->fault_fired = true;
		return (FSIMG_EIO);
	}
	if (volume->reads == volume->eof_read) {
		volume->fault_fired = true;
		return (FSIMG_OK);
	}
	if (volume->max_transfer != 0 && length > volume->max_transfer)
		length = volume->max_transfer;
	bcopy(volume->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
volume_write(void *argument, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	contract_volume_t *volume = argument;
	bool failing;

	*transferred = 0;
	check_access(volume, offset, length);
	CONTRACT_CHECK(volume->writable);
	volume->writes++;
	if (volume->writes == volume->stall_write) {
		volume->fault_fired = true;
		return (FSIMG_OK);
	}
	failing = volume->writes == volume->fail_write;
	if (volume->max_transfer != 0 && length > volume->max_transfer)
		length = volume->max_transfer;
	if (failing)
		length /= 2;
	bcopy(buffer, volume->bytes + offset, length);
	*transferred = length;
	volume->written_bytes += length;
	if (failing) {
		volume->fault_fired = true;
		volume->confirmed_end = offset - CONTRACT_GUARD + length;
		return (FSIMG_EIO);
	}
	return (FSIMG_OK);
}

static fsimg_status_t
volume_zero(void *argument, uint64_t offset, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	contract_volume_t *volume = argument;

	check_access(volume, offset, length);
	CONTRACT_CHECK(volume->writable);
	volume->zeroes++;
	if (volume->zeroes == volume->fail_zero) {
		volume->fault_fired = true;
		return (FSIMG_EIO);
	}
	bzero(volume->bytes + offset, (size_t)length);
	return (FSIMG_OK);
}

static fsimg_status_t
volume_flush(void *argument, fsimg_error_t *error FSIMG_UNUSED)
{
	contract_volume_t *volume = argument;

	volume->flushes++;
	if (volume->fail_flush) {
		volume->fault_fired = true;
		return (FSIMG_EIO);
	}
	return (FSIMG_OK);
}

fsimg_io_t *
contract_volume_open(fsimg_ctx_t *ctx, contract_volume_t *volume,
    bool writable, fsimg_io_t **backing)
{
	fsimg_io_ops_t ops;
	fsimg_io_t *slice;
	fsimg_error_t error;

	volume->writable = writable;
	fsimg_io_ops_init(&ops);
	ops.get_size = volume_size;
	ops.read_at = volume_read;
	if (writable) {
		ops.write_at = volume_write;
		ops.flush = volume_flush;
		if (!volume->no_zero_range)
			ops.zero_range = volume_zero;
	}
	CONTRACT_STATUS(fsimg_io_from_callbacks(ctx, &ops, volume, backing,
	    &error), FSIMG_OK, &error);
	CONTRACT_STATUS(fsimg_io_slice(*backing, CONTRACT_GUARD, volume->length,
	    &slice, &error), FSIMG_OK, &error);
	return (slice);
}

void
contract_volume_close(fsimg_io_t *slice, fsimg_io_t *backing)
{
	fsimg_io_free(slice);
	fsimg_io_free(backing);
}

unsigned char
contract_pattern(uint64_t identity, uint64_t offset)
{
	return ((unsigned char)(offset * 31 + identity * 7 + 1 +
	    (offset >> 11)));
}

void
contract_dir_open(contract_dir_t *dir)
{
	snprintf(dir->path, sizeof(dir->path), "contract-XXXXXX");
	CONTRACT_CHECK(mkdtemp(dir->path) != NULL);
	dir->fd = open(dir->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CONTRACT_CHECK(dir->fd != -1);
}

/* The directory must be empty: an operation left nothing behind. A case
 * that keeps output removes it first, by the names it chose.
 */
void
contract_dir_close(contract_dir_t *dir)
{
	DIR *stream;
	struct dirent *entry;
	int fd;

	fd = dup(dir->fd);
	CONTRACT_CHECK(fd != -1);
	stream = fdopendir(fd);
	CONTRACT_CHECK(stream != NULL);
	while ((entry = readdir(stream)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		fprintf(stderr, "fixture directory retains %s\n", entry->d_name);
		CONTRACT_CHECK(!"fixture directory is empty");
	}
	CONTRACT_CHECK(closedir(stream) == 0);
	CONTRACT_CHECK(close(dir->fd) == 0);
	CONTRACT_CHECK(rmdir(dir->path) == 0);
}

void
contract_baseline(fsimg_ctx_t *ctx, fsimg_stats_t *baseline)
{
	fsimg_error_t error;

	CONTRACT_STATUS(fsimg_ctx_get_stats(ctx, baseline, &error), FSIMG_OK,
	    &error);
}

void
contract_check_released(fsimg_ctx_t *ctx, const fsimg_stats_t *baseline)
{
	fsimg_stats_t stats;

	contract_baseline(ctx, &stats);
	CONTRACT_CHECK(stats.memory_bytes == baseline->memory_bytes);
	CONTRACT_CHECK(stats.scratch_bytes == baseline->scratch_bytes);
	CONTRACT_CHECK(stats.descriptors == baseline->descriptors);
	CONTRACT_CHECK(stats.spool_bytes == baseline->spool_bytes);
	CONTRACT_CHECK(stats.memory_peak >= stats.memory_bytes);
	CONTRACT_CHECK(stats.scratch_peak >= stats.scratch_bytes);
	CONTRACT_CHECK(stats.descriptors_peak >= stats.descriptors);
}

void
contract_arm_alloc(fsimg_ctx_t *ctx, uint64_t position)
{
	fsimg_test_fail_alloc_after(ctx, position);
}

/* The hook counts attempts from arming; it fired once they reached it. */
bool
contract_alloc_fired(fsimg_ctx_t *ctx)
{
	return (ctx->fail_allocation != 0 &&
	    ctx->allocation_attempts >= ctx->fail_allocation);
}

/* Position zero disarms, clearing the point so nothing reads as fired. */
void
contract_arm_meta(fsimg_ctx_t *ctx, int point, uint64_t position)
{
	fsimg_test_meta_fail_after(ctx, position == 0 ? 0 :
	    (fsimg_meta_fault_t)point, position, EIO);
}

/* The hook counts its remaining operations down; zero after arming means
 * the armed operation failed.
 */
bool
contract_meta_fired(fsimg_ctx_t *ctx)
{
	return (ctx->meta_fault_point != 0 && ctx->meta_fault_after == 0);
}

bool
contract_writer_source_active(const fsimg_writer_t *writer)
{
	return (writer->source.active);
}

bool
contract_writer_failed(const fsimg_writer_t *writer)
{
	return (writer->state == FSIMG_WRITER_FAILED);
}
