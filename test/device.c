/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fsimg-endian.h"
#include "host.h"
#include "io.h"
#include "sandbox.h"
#include "test.h"

typedef struct device_fixture {
	uint64_t	 capacity;
	uint64_t	 quantum;
	dev_t		 device;
	ino_t		 inode;
	unsigned	 queries;
	unsigned	 reads;
	unsigned	 interrupts;
	unsigned	 after_reads; /* Error/EOF begins after this many reads */
	unsigned	 cancel_after_reads;
	size_t		 short_bytes;
	int		 query_errno;
	int		 read_errno;
	bool		 eof;
	bool		 pattern;
	bool		 cancelled;
	bool		 active;
	int		 reader;
	int		 writer;
} device_fixture_t;

static device_fixture_t fixture;

static bool
device_cancelled(void *argument)
{
	device_fixture_t *device = argument;

	return ((device->cancelled && device->queries != 0) ||
	    (device->cancel_after_reads != 0 &&
	    device->reads >= device->cancel_after_reads));
}

int fsimg_test_device_fstat(int, struct stat *);
int fsimg_test_device_sector(int, uint64_t *);
int fsimg_test_device_size(int, uint64_t, uint64_t *);
ssize_t fsimg_test_device_pread(int, void *, size_t, off_t);

static bool
fixture_descriptor(int fd)
{
	struct stat st;

	return (fixture.active && fsimg_host_fstat(fd, &st) == 0 &&
	    st.st_dev == fixture.device && st.st_ino == fixture.inode);
}

int
fsimg_test_device_fstat(int fd, struct stat *st)
{
	int result;

	result = fsimg_host_fstat(fd, st);
	if (result == 0 && fixture_descriptor(fd)) {
#if defined(__FreeBSD__)
		st->st_mode = S_IFCHR | 0400;
#else
		st->st_mode = S_IFBLK | 0400;
#endif
		st->st_size = 0;
	}
	return (result);
}

int
fsimg_test_device_sector(int fd, uint64_t *out)
{
	if (!fixture_descriptor(fd))
		return (fsimg_host_disk_sector(fd, out));
	fixture.queries++;
	if (fixture.query_errno != 0) {
		errno = fixture.query_errno;
		return (-1);
	}
	*out = fixture.quantum;
	return (0);
}

int
fsimg_test_device_size(int fd, uint64_t quantum, uint64_t *out)
{
	if (!fixture_descriptor(fd))
		return (fsimg_host_disk_size(fd, quantum, out));
	CHECK(quantum == fixture.quantum);
	fixture.queries++;
	*out = fixture.capacity;
	return (0);
}

static unsigned char
pattern_byte(uint64_t offset)
{
	return ((unsigned char)(offset % 251));
}

ssize_t
fsimg_test_device_pread(int fd, void *buffer, size_t length, off_t offset)
{
	size_t count;

	if (!fixture_descriptor(fd))
		return (pread(fd, buffer, length, offset));
	CHECK(offset >= 0 && fixture.quantum != 0);
	CHECK((uint64_t)offset % fixture.quantum == 0);
	CHECK(length != 0 && length % fixture.quantum == 0);
	CHECK((uint64_t)offset <= fixture.capacity);
	CHECK(length <= fixture.capacity - (uint64_t)offset);
	fixture.reads++;
	/* Poison bytes the host will not confirm, including after short reads. */
	memset(buffer, 0xa5, length);
	if (fixture.interrupts != 0) {
		fixture.interrupts--;
		errno = EINTR;
		return (-1);
	}
	if (fixture.read_errno != 0 && fixture.reads > fixture.after_reads) {
		errno = fixture.read_errno;
		return (-1);
	}
	if (fixture.eof && fixture.reads > fixture.after_reads)
		return (0);
	count = fixture.short_bytes != 0 && fixture.short_bytes < length ?
	    fixture.short_bytes : length;
	if (!fixture.pattern)
		return (pread(fd, buffer, count, offset));
	for (size_t i = 0; i < count; i++)
		((unsigned char *)buffer)[i] = pattern_byte((uint64_t)offset + i);
	return ((ssize_t)count);
}

static void
fixture_open(uint64_t quantum, uint64_t capacity)
{
	char path[] = "device-fixture-XXXXXX";
	struct stat st;

	bzero(&fixture, sizeof(fixture));
	fixture.writer = mkstemp(path);
	CHECK(fixture.writer != -1);
	fixture.reader = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(fixture.reader != -1);
	CHECK(unlink(path) == 0);
	CHECK(fsimg_host_fstat(fixture.reader, &st) == 0);
	fixture.device = st.st_dev;
	fixture.inode = st.st_ino;
	fixture.quantum = quantum;
	fixture.capacity = capacity;
	fixture.active = fixture.pattern = true;
}

static void
fixture_close(void)
{
	CHECK(close(fixture.reader) == 0);
	CHECK(close(fixture.writer) == 0);
	fixture.active = false;
}

static void
test_sector_reads(uint64_t quantum)
{
	unsigned char buffer[70003];
	fsimg_host_file_t identity;
	fsimg_options_t options;
	fsimg_error_t error;
	fsimg_stats_t baseline, after;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io, *slice, *nested, *decoded, *child;
	uint64_t start, end;
	size_t count;
	unsigned reads;
	bool known;

	fixture_open(quantum, quantum * 1000);
	fsimg_options_init(&options);
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(ctx, &baseline, NULL) == FSIMG_OK);
	CHECK(fsimg_io_from_fd(ctx, fixture.reader, FSIMG_IO_READ, &io, NULL) == FSIMG_OK);
	CHECK(fixture.queries == 2 && fsimg_io_logical_sector(io) == quantum);
	CHECK(fsimg_io_slice(io, quantum - 2, 80000, &slice, NULL) == FSIMG_OK);
	CHECK(fsimg_io_slice(slice, 1, 75000, &nested, NULL) == FSIMG_OK);
	CHECK(fsimg_io_logical_sector(nested) == quantum);
	CHECK(fsimg_io_read_exact(nested, 0, buffer + 1, 70001, &count, NULL) == FSIMG_OK);
	CHECK(count == 70001);
	for (size_t i = 0; i < count; i++)
		CHECK(buffer[i + 1] == pattern_byte(quantum - 1 + i));
	CHECK(fsimg_io_read_exact(io, fixture.capacity - 1, buffer, 1,
	    &count, NULL) == FSIMG_OK && buffer[0] == pattern_byte(fixture.capacity - 1));
	reads = fixture.reads;
	CHECK(fsimg_io_read_at(io, fixture.capacity, NULL, 0, &count, NULL) == FSIMG_OK);
	CHECK(fsimg_io_read_at(io, fixture.capacity, buffer, 1, &count, NULL) == FSIMG_ERANGE);
	CHECK(fsimg_io_write_at(io, 0, buffer, 1, &count, NULL) == FSIMG_EUNSUPPORTED);
	CHECK(fsimg_io_resize(io, 0, NULL) == FSIMG_EUNSUPPORTED);
	CHECK(fsimg_io_zero_range(io, 0, 1, NULL) == FSIMG_EUNSUPPORTED);
	CHECK(fsimg_io_flush(io, true, NULL) == FSIMG_EUNSUPPORTED);
	CHECK(fsimg_io_flush(io, false, NULL) == FSIMG_EUNSUPPORTED);
	CHECK(fsimg_io_next_zero_range(io, 0, &start, &end, NULL) == FSIMG_OK);
	CHECK(start == fixture.capacity && end == fixture.capacity);
	CHECK(fixture.reads == reads && fixture.queries == 2);
	CHECK(fsimg_io_identity(io, &identity, &known, NULL) == FSIMG_OK && !known);
	CHECK(fsimg_io_decoded_view(nested, &decoded, NULL) == FSIMG_OK);
	CHECK(fsimg_io_slice(decoded, 0, 8, &child, NULL) == FSIMG_OK);
	CHECK(fsimg_io_logical_sector(child) == 0);
	fsimg_io_free(child);
	fsimg_io_free(decoded);
	fsimg_io_free(nested);
	fsimg_io_free(slice);
	fixture.short_bytes = (size_t)quantum;
	CHECK(fsimg_io_read_exact(io, quantum - 2, buffer, 10,
	    &count, &error) == FSIMG_OK && count == 10);
	fixture.short_bytes = (size_t)quantum - 1;
	memset(buffer, 0x7b, sizeof(buffer));
	CHECK(fsimg_io_read_exact(io, 0, buffer, (size_t)quantum,
	    &count, &error) == FSIMG_EIO && count == quantum - 1);
	CHECK(buffer[count] == 0x7b);
	fixture.short_bytes = 0;
	fixture.eof = true;
	CHECK(fsimg_io_read_at(io, 0, buffer, 1, &count, &error) == FSIMG_EIO && count == 0);
	fixture.eof = false;
	fixture.interrupts = 1025;
	reads = fixture.reads;
	CHECK(fsimg_io_read_exact(io, 0, buffer, 1, &count, &error) == FSIMG_EIO);
	CHECK(error.os_error == EINTR && fixture.reads - reads == 1025);
	fixture.interrupts = 0;
	fixture.read_errno = ENXIO;
	CHECK(fsimg_io_read_exact(io, 0, buffer, 1, &count, &error) == FSIMG_EIO);
	CHECK(error.os_error == ENXIO && error.offset_valid && error.offset == 0);
	fsimg_io_free(io);
	CHECK(fsimg_ctx_get_stats(ctx, &after, NULL) == FSIMG_OK);
	CHECK(after.memory_bytes == baseline.memory_bytes && after.descriptors == 0);
	fsimg_ctx_free(ctx);
	fixture_close();
}

/* A confirmed first batch survives a later error/EOF or cancellation. The
 * unwritten tail stays poisoned, and work counts actual adapter attempts.
 */
static void
test_failures_after_progress(void)
{
	static const fsimg_status_t expected[] = {
		FSIMG_EIO, FSIMG_EIO, FSIMG_ECANCELLED
	};
	unsigned char buffer[70000];
	fsimg_session_t session;
	fsimg_options_t options;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	size_t count;
	uint64_t baseline_memory, attempts;

	for (size_t scenario = 0; scenario < sizeof(expected) / sizeof(expected[0]); scenario++) {
		fixture_open(512, 1024 * 1024);
		fsimg_options_init(&options);
		options.cancelled = device_cancelled;
		options.cancel_argument = &fixture;
		CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
		baseline_memory = ctx->stats.memory_bytes;
		CHECK(fsimg_io_from_fd(ctx, fixture.reader, FSIMG_IO_READ,
		    &io, NULL) == FSIMG_OK);
		fixture.after_reads = 1;
		if (scenario == 0)
			fixture.read_errno = ENXIO;
		else if (scenario == 1)
			fixture.eof = true;
		else
			fixture.cancel_after_reads = 1;
		fsimg_session_init(&session, ctx, "read", fixture.capacity);
		fsimg_io_bind_session(io, &session);
		memset(buffer, 0x7b, sizeof(buffer));
		CHECK(fsimg_io_read_exact(io, 3, buffer, sizeof(buffer),
		    &count, &error) == expected[scenario]);
		CHECK(count == 65533 && error.offset_valid && error.offset == 65536);
		for (size_t i = 0; i < sizeof(buffer); i++)
			CHECK(buffer[i] == (i < count ? pattern_byte(i + 3) : 0x7b));
		attempts = scenario == 2 ? 1 : 2;
		CHECK(session.progress.work == attempts && fixture.reads == attempts);
		CHECK(ctx->stats.io_calls == attempts && ctx->stats.read_bytes == count);
		if (scenario == 0)
			CHECK(error.os_error == ENXIO);
		fsimg_io_free(io);
		CHECK(ctx->stats.descriptors == 0 && ctx->stats.memory_bytes == baseline_memory);
		fsimg_ctx_free(ctx);
		fixture_close();
	}
}

static void
test_geometry_and_failures(void)
{
	fsimg_input_t input;
	fsimg_options_t options;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	uint64_t baseline_memory;
	fsimg_status_t status;
	int nullfd;

	fixture_open(512, 65536);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.cancelled = device_cancelled;
	options.cancel_argument = &fixture;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	baseline_memory = ctx->stats.memory_bytes;
	CHECK(fsimg_io_from_fd(ctx, fixture.writer, FSIMG_IO_READ, &io, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_io_from_fd(ctx, fixture.reader, FSIMG_IO_WRITE, &io, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_fd_reserve(ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_io_adopt_fd(ctx, dup(fixture.reader), FSIMG_IO_READ,
	    &io, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_input_prepare(fixture.reader, NULL, NULL, &input, NULL) == FSIMG_OK);
	CHECK(input.length == fixture.capacity && input.quantum == 512);
	for (uint64_t failure = 1; failure <= 2; failure++) {
		fsimg_test_fail_alloc_after(ctx, failure);
		CHECK(fsimg_io_from_fd(ctx, fixture.reader, FSIMG_IO_READ,
		    &io, &error) == FSIMG_ENOMEM);
		CHECK(io == NULL && ctx->stats.descriptors == 0 &&
		    ctx->stats.memory_bytes == baseline_memory);
	}
	fsimg_test_fail_alloc_after(ctx, 0);
	fixture.cancelled = true;
	fixture.queries = 0;
	CHECK(fsimg_io_from_fd(ctx, fixture.reader, FSIMG_IO_READ, &io, NULL) == FSIMG_ECANCELLED);
	CHECK(fixture.queries == 1 && ctx->stats.descriptors == 0);
	fixture.cancelled = false;
	fixture.capacity = 65535;
	CHECK(fsimg_io_from_fd(ctx, fixture.reader, FSIMG_IO_READ, &io, NULL) == FSIMG_EUNSUPPORTED);
	fixture.capacity = 0;
	CHECK(fsimg_io_from_fd(ctx, fixture.reader, FSIMG_IO_READ, &io, NULL) == FSIMG_EUNSUPPORTED);
	fixture.capacity = UINT64_C(1) << 63;
	CHECK(fsimg_io_from_fd(ctx, fixture.reader, FSIMG_IO_READ, &io, NULL) == FSIMG_ERANGE);
	fixture.quantum = fixture.capacity = 32 * 1024 * 1024;
	CHECK(fsimg_io_from_fd(ctx, fixture.reader, FSIMG_IO_READ, &io, NULL) == FSIMG_ELIMIT);
	fixture.quantum = 512;
	fixture.query_errno = ENOTTY;
	CHECK(fsimg_io_from_fd(ctx, fixture.reader, FSIMG_IO_READ, &io, NULL) == FSIMG_EUNSUPPORTED);
	fixture.query_errno = ENXIO;
	CHECK(fsimg_io_from_fd(ctx, fixture.reader, FSIMG_IO_READ, &io, &error) == FSIMG_EIO);
	CHECK(error.os_error == ENXIO);
	CHECK(ctx->stats.descriptors == 0 && ctx->stats.memory_bytes == baseline_memory);
	fsimg_ctx_free(ctx);
	fixture_close();
	nullfd = open("/dev/null", O_RDONLY);
	CHECK(nullfd != -1);
	status = fsimg_input_prepare(nullfd, NULL, NULL, &input, &error);
	/* A non-disk character node can reject geometry with a host-specific
	 * I/O error. Either way it acquires no prepared disk authority.
	 */
	CHECK(status == FSIMG_EUNSUPPORTED || status == FSIMG_EIO);
	CHECK(input.kind == FSIMG_INPUT_ABSENT);
	CHECK(close(nullfd) == 0);
}

/* A small MBR disk in a fixed VHD. The encoded file is a whole 4K sector,
 * while its decoded disk uses 512-byte LBAs and ends before that sector.
 */
static void
test_decoded_vhd_reopen(void)
{
	unsigned char mbr[512] = { 0 }, footer[512] = { 0 };
	fsimg_options_t options;
	fsimg_image_options_t image_options;
	fsimg_selector_t selector;
	fsimg_image_info_t info;
	fsimg_partition_info_t original, reopened;
	fsimg_ctx_t *ctx;
	fsimg_image_t *outer, *inner, *repeated;
	fsimg_io_t *io, *disk, *slice, *again;
	uint64_t baseline_memory;
	uint32_t sum = 0;

	fixture_open(4096, 65536);
	fixture.pattern = false;
	CHECK(ftruncate(fixture.writer, 65536) == 0);
	mbr[446 + 4] = 1;
	fsimg_store_le32(mbr + 446 + 8, 1);
	fsimg_store_le32(mbr + 446 + 12, 8);
	mbr[510] = 0x55;
	mbr[511] = 0xaa;
	CHECK(pwrite(fixture.writer, mbr, sizeof(mbr), 0) == sizeof(mbr));
	bcopy("conectix", footer, 8);
	fsimg_store_be32(footer + 8, 2);
	fsimg_store_be32(footer + 12, 0x10000);
	fsimg_store_be64(footer + 16, UINT64_MAX);
	fsimg_store_be64(footer + 40, 65536 - 512);
	fsimg_store_be64(footer + 48, 65536 - 512);
	fsimg_store_be32(footer + 60, 2);
	for (size_t i = 0; i < sizeof(footer); i++)
		sum += footer[i];
	fsimg_store_be32(footer + 64, ~sum);
	CHECK(pwrite(fixture.writer, footer, sizeof(footer), 65536 - 512) == sizeof(footer));
	fsimg_options_init(&options);
	options.spool_bytes = 512;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	baseline_memory = ctx->stats.memory_bytes;
	CHECK(fsimg_io_from_fd(ctx, fixture.reader, FSIMG_IO_READ, &io, NULL) == FSIMG_OK);
	CHECK(fsimg_image_open(ctx, io, NULL, &outer, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(outer, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.container, "vhd") == 0 && info.sector_bytes == 512);
	CHECK(fsimg_image_next_partition(outer, &original, NULL) == FSIMG_OK);
	CHECK(original.offset == 512 && original.length == 4096);
	selector = (fsimg_selector_t){ .kind = FSIMG_SELECT_DISK };
	CHECK(fsimg_image_select(outer, &selector, &disk, NULL) == FSIMG_OK);
	CHECK(fsimg_io_slice(disk, 0, 65536 - 512, &slice, NULL) == FSIMG_OK);
	CHECK(fsimg_io_logical_sector(slice) == 0);
	CHECK(fsimg_image_open(ctx, slice, NULL, &inner, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(inner, &info, NULL) == FSIMG_OK && info.sector_bytes == 512);
	CHECK(fsimg_image_next_partition(inner, &reopened, NULL) == FSIMG_OK);
	CHECK(reopened.offset == original.offset && reopened.length == original.length);
	CHECK(fsimg_image_select(inner, &selector, &again, NULL) == FSIMG_OK);
	CHECK(fsimg_image_open(ctx, again, NULL, &repeated, NULL) == FSIMG_OK);
	CHECK(fsimg_image_next_partition(repeated, &reopened, NULL) == FSIMG_OK);
	CHECK(reopened.offset == original.offset && reopened.length == original.length);
	fsimg_image_free(repeated);
	fsimg_io_free(again);
	fsimg_image_free(inner);
	fsimg_io_free(slice);
	fsimg_io_free(disk);
	fsimg_image_free(outer);
	fsimg_image_options_init(&image_options);
	CHECK(fsimg_stack_parse("fat12/mbr/-", &image_options.stack, NULL) == FSIMG_OK);
	CHECK(fsimg_image_open(ctx, io, &image_options, &outer, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(outer, &info, NULL) == FSIMG_EUNSUPPORTED);
	fsimg_image_free(outer);
	CHECK(fsimg_io_slice(io, 0, fixture.capacity, &slice, NULL) == FSIMG_OK);
	CHECK(fsimg_image_open(ctx, slice, &image_options, &outer, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(outer, &info, NULL) == FSIMG_EUNSUPPORTED);
	fsimg_image_free(outer);
	fsimg_io_free(slice);
	fsimg_io_free(io);
	CHECK(ctx->stats.descriptors == 0 && ctx->stats.memory_bytes == baseline_memory);
	fsimg_ctx_free(ctx);
	fixture_close();
}

/* Called only by the disposable-device fixture script. A confined child
 * adopts geometry prepared before confinement and verifies that reading is
 * possible while geometry ioctls and ambient opens are denied.
 */
static int
native_rights(const char *path)
{
	fsimg_sandbox_caps_t caps;
	fsimg_input_t input;
	fsimg_status_t status;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	unsigned char bytes[3];
	uint64_t sector;
	size_t count;
	pid_t child;
	int fd, waited;

	if (strcmp(fsimg_sandbox_backend(), "none") == 0)
		return (77);
	fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(fsimg_input_prepare(fd, NULL, NULL, &input, NULL) == FSIMG_OK);
	CHECK(input.kind == FSIMG_INPUT_DEVICE);
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		if (dup2(fd, 3) == -1 || fsimg_host_closefrom(4) == -1)
			_exit(1);
		close(0);
		close(1);
		close(2);
		fsimg_sandbox_caps_init(&caps);
		caps.input = 3;
		caps.image_input = &input;
		status = fsimg_sandbox_install_caps(&caps, NULL);
		if (status == FSIMG_EUNSUPPORTED)
			_exit(77);
		if (status != FSIMG_OK || fsimg_host_disk_sector(3, &sector) != -1)
			_exit(2);
		if (write(3, "x", 1) != -1 || ftruncate(3, 0) != -1 ||
		    open("/etc/passwd", O_RDONLY) != -1)
			_exit(3);
		if (fsimg_ctx_new(NULL, &ctx, NULL) != FSIMG_OK ||
		    fsimg_fd_reserve(ctx, NULL) != FSIMG_OK ||
		    fsimg_io_adopt_input(ctx, 3, &input, &io, NULL) != FSIMG_OK)
			_exit(4);
		if (fsimg_io_read_exact(io, 1, bytes + 1, 1, &count, NULL) != FSIMG_OK)
			_exit(5);
		fsimg_io_free(io);
		fsimg_ctx_free(ctx);
		_exit(0);
	}
	CHECK(close(fd) == 0);
	CHECK(waitpid(child, &waited, 0) == child && WIFEXITED(waited));
	return (WEXITSTATUS(waited));
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--native") == 0)
		return (native_rights(argv[2]));
	CHECK(argc == 1);
	test_sector_reads(512);
	test_sector_reads(4096);
	test_sector_reads(1536);
	test_geometry_and_failures();
	test_failures_after_progress();
	test_decoded_vhd_reopen();
	return (0);
}
