/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>

#include "worker.h"

/* This process owns potentially blocking stdout/stderr writes. Its fixed input
 * header selects stream 1 or 2 and a bounded length, followed by exactly those
 * bytes. Status records report cumulative confirmed bytes, errno, and marker:
 * zero means progress, one acknowledges a record, two ends the complete stream.
 * It has no parser, pathname, source, image, or scratch authority.
 */
static int report(const uint64_t [2], int, unsigned);
static int read_exact(void *, size_t, bool);

static int
report(const uint64_t confirmed[2], int failure, unsigned marker)
{
	unsigned char raw[FSIMG_RELAY_STATUS_BYTES];
	size_t offset;
	ssize_t written;

	fsimg_wire_put64(raw + FSIMG_RELAY_STDOUT, confirmed[0]);
	fsimg_wire_put64(raw + FSIMG_RELAY_STDERR, confirmed[1]);
	fsimg_wire_put64(raw + FSIMG_RELAY_ERRNO, (uint64_t)failure);
	fsimg_wire_put64(raw + FSIMG_RELAY_MARKER, marker);
	offset = 0;
	while (offset < sizeof(raw)) {
		written = write(4, raw + offset, sizeof(raw) - offset);
		if (written == -1 && errno == EINTR)
			continue;
		if (written <= 0)
			return (-1);
		offset += (size_t)written;
	}
	return (0);
}

/* Return 1 for a complete read, zero for clean EOF between records, -1 for a
 * partial record or I/O failure. Payload EOF is never a complete operation.
 */
static int
read_exact(void *buffer, size_t length, bool between_records)
{
	unsigned char *bytes = buffer;
	size_t offset;
	ssize_t count;

	offset = 0;
	while (offset < length) {
		count = read(3, bytes + offset, length - offset);
		if (count == -1 && errno == EINTR)
			continue;
		if (count == 0 && offset == 0 && between_records)
			return (0);
		if (count <= 0) {
			if (count == 0)
				errno = EIO;
			return (-1);
		}
		offset += (size_t)count;
	}
	return (1);
}

int
fsimg_output_main(void)
{
	struct rlimit core = { 0, 0 };
	unsigned char buffer[65536], header[FSIMG_RELAY_HEADER_BYTES];
	uint64_t confirmed[2] = { 0, 0 };
	uint64_t remaining, stream;
	size_t batch, offset;
	ssize_t written;
	int code, failure;

	if (setrlimit(RLIMIT_CORE, &core) == -1)
		return (1);
	if (chdir("/") == -1)
		return (1);
	/* Report EPIPE through the status pipe instead of dying before reporting. */
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		return (1);
	for (;;) {
		code = read_exact(header, sizeof(header), true);
		if (code <= 0) {
			failure = code == -1 ? errno : 0;
			code = report(confirmed, failure, 2);
			return (code == 0 && failure == 0 ? 0 : 1);
		}
		stream = fsimg_wire_get64(header + FSIMG_RELAY_STREAM);
		remaining = fsimg_wire_get64(header + FSIMG_RELAY_LENGTH);
		if (stream < 1 || stream > 2 || remaining == 0 ||
		    remaining > FSIMG_WORKER_TEXT_BYTES) {
			report(confirmed, EINVAL, 2);
			return (1);
		}
		while (remaining != 0) {
			batch = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
			if (read_exact(buffer, batch, false) == -1) {
				report(confirmed, errno, 2);
				return (1);
			}
			offset = 0;
			while (offset < batch) {
				written = write((int)stream, buffer + offset, batch - offset);
				if (written == -1 && errno == EINTR)
					continue;
				if (written <= 0) {
					report(confirmed, written == -1 ? errno : EIO, 2);
					return (1);
				}
				offset += (size_t)written;
				if ((uint64_t)written > UINT64_MAX - confirmed[stream - 1]) {
					report(confirmed, EOVERFLOW, 2);
					return (1);
				}
				confirmed[stream - 1] += (uint64_t)written;
				if (report(confirmed, 0, 0) == -1)
					return (1);
			}
			remaining -= batch;
		}
		if (report(confirmed, 0, 1) == -1)
			return (1);
	}
}
