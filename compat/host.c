/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/mount.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#if !defined(__linux__)
#include <dirent.h>
#endif
#if defined(__APPLE__)
#include <sys/attr.h>

/* Bulk records contain a u32 length, five returned attribute words, and a
 * name reference. The name offset is relative to that reference, not the record.
 */
#define MACOS_NAME_REFERENCE (sizeof(uint32_t) + sizeof(attribute_set_t))
#define MACOS_DIRECTORY_PREFIX (MACOS_NAME_REFERENCE + sizeof(attrreference_t))
#endif

#if defined(__linux__)
#include <linux/stat.h>
#include <sys/syscall.h>
#endif

#include "host.h"

_Static_assert(sizeof(off_t) == sizeof(int64_t), "64-bit off_t required");

static fsimg_timestamp_t host_time(int64_t, long);

bool
fsimg_host_offset(uint64_t offset, off_t *out)
{
	if (offset > INT64_MAX)
		return (false);
	*out = (off_t)offset;
	return (true);
}

int
fsimg_host_dup(int fd)
{
	return (fcntl(fd, F_DUPFD_CLOEXEC, 0));
}

int
fsimg_host_source_open(int parent, const char *name, bool directory)
{
	int fd, flags;

	flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC;
	if (directory)
		flags |= O_DIRECTORY;
	fd = openat(parent, name, flags);
#if defined(__FreeBSD__)
	/* FreeBSD names the O_NOFOLLOW symlink failure EMLINK. */
	if (fd == -1 && errno == EMLINK)
		errno = ELOOP;
#endif
	return (fd);
}

int
fsimg_host_closefrom(int first FSIMG_UNUSED)
{
#if defined(__linux__) && defined(__NR_close_range)
	return ((int)syscall(__NR_close_range, (unsigned)first, UINT_MAX, 0));
#elif defined(__FreeBSD__)
	closefrom(first);
	return (0);
#else
	/* Resource limits can be lowered below live descriptors. A loop bounded
	 * by the soft or hard limit cannot establish complete descriptor closure.
	 */
	errno = ENOTSUP;
	return (-1);
#endif
}

bool
fsimg_host_unsupported_io(int flags FSIMG_UNUSED)
{
#ifdef O_DIRECT
	if ((flags & O_DIRECT) != 0)
		return (true);
#endif
#ifdef O_PATH
	if ((flags & O_PATH) != 0)
		return (true);
#endif
#ifdef O_EVTONLY
	if ((flags & O_EVTONLY) != 0)
		return (true);
#endif
	return (false);
}

static fsimg_timestamp_t
host_time(int64_t seconds, long nanoseconds)
{
	fsimg_timestamp_t time = {
	    .seconds = seconds,
	    .precision_ns = 1,
	    .nanoseconds = (uint32_t)nanoseconds,
	    .basis = FSIMG_TIME_UTC
	};

	return (time);
}

int
fsimg_host_file(int fd, fsimg_host_file_t *out)
{
	struct stat st;

	bzero(out, sizeof(*out));
	if (fstat(fd, &st) == -1)
		return (-1);
	if (st.st_size < 0) {
		errno = EOVERFLOW;
		return (-1);
	}
	out->device = (uint64_t)st.st_dev;
	out->inode = (uint64_t)st.st_ino;
	out->length = (uint64_t)st.st_size;
	out->kind = S_ISREG(st.st_mode) ? FSIMG_HOST_FILE :
	    S_ISDIR(st.st_mode) ? FSIMG_HOST_DIRECTORY : FSIMG_HOST_OTHER;
#if defined(__APPLE__)
	out->modified = host_time(st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec);
	out->changed = host_time(st.st_ctimespec.tv_sec, st.st_ctimespec.tv_nsec);
	out->metadata.accessed = host_time(st.st_atimespec.tv_sec, st.st_atimespec.tv_nsec);
	out->metadata.created = host_time(st.st_birthtimespec.tv_sec, st.st_birthtimespec.tv_nsec);
#else
	out->modified = host_time(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
	out->changed = host_time(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
	out->metadata.accessed = host_time(st.st_atim.tv_sec, st.st_atim.tv_nsec);
#if defined(__FreeBSD__)
	out->metadata.created = host_time(st.st_birthtim.tv_sec, st.st_birthtim.tv_nsec);
#endif
#endif
	out->metadata.modified = out->modified;
	out->metadata.valid = FSIMG_META_MODIFIED | FSIMG_META_ACCESSED;
#if defined(__APPLE__) || defined(__FreeBSD__)
	out->metadata.valid |= FSIMG_META_CREATED;
#endif
	return (0);
}

int
fsimg_host_mount_id(int fd FSIMG_UNUSED, uint64_t *out)
{
#if defined(__linux__) && defined(__NR_statx) && defined(STATX_MNT_ID)
	struct statx st;
	int code;

	bzero(&st, sizeof(st));
	/* A NULL path permits a sandbox to prove descriptor-only authority from
	 * scalar syscall arguments. Older unconfined kernels need the empty name.
	 */
	code = (int)syscall(__NR_statx, fd, NULL, AT_EMPTY_PATH, STATX_MNT_ID, &st);
	if (code == -1 && errno == EFAULT)
		code = (int)syscall(__NR_statx, fd, "", AT_EMPTY_PATH, STATX_MNT_ID, &st);
	if (code == -1) {
		if (errno == ENOSYS || errno == EINVAL)
			errno = ENOTSUP;
		return (-1);
	}
	if ((st.stx_mask & STATX_MNT_ID) == 0) {
		errno = ENOTSUP;
		return (-1);
	}
	*out = st.stx_mnt_id;
	return (0);
#elif defined(__APPLE__) || defined(__FreeBSD__)
	struct statfs st;

	if (fstatfs(fd, &st) == -1)
		return (-1);
	*out = (uint64_t)(uint32_t)st.f_fsid.val[0] << 32 |
	    (uint32_t)st.f_fsid.val[1];
	if (*out == 0) {
		errno = ENOTSUP;
		return (-1);
	}
	return (0);
#else
	*out = 0;
	errno = ENOTSUP;
	return (-1);
#endif
}

int
fsimg_host_directory_read(int fd, void *buffer, size_t capacity, size_t *used)
{
	ssize_t count;

	*used = 0;
#if defined(__linux__)
	count = syscall(__NR_getdents64, fd, buffer, capacity);
#elif defined(__FreeBSD__)
	off_t base;

	count = getdirentries(fd, buffer, capacity, &base);
#elif defined(__APPLE__)
	struct attrlist attributes = {
	    .bitmapcount = ATTR_BIT_MAP_COUNT,
	    .commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME
	};
	unsigned char *bytes = buffer;
	uint32_t length;
	size_t offset;
	int entries;

	entries = getattrlistbulk(fd, &attributes, buffer, capacity,
	    FSOPT_PACK_INVAL_ATTRS);
	if (entries == -1)
		return (-1);
	offset = 0;
	for (int i = 0; i < entries; i++) {
		if (offset > capacity || capacity - offset < sizeof(length)) {
			errno = EIO;
			return (-1);
		}
		bcopy(bytes + offset, &length, sizeof(length));
		if (length < MACOS_DIRECTORY_PREFIX || length > capacity - offset) {
			errno = EIO;
			return (-1);
		}
		offset += length;
	}
	count = (ssize_t)offset;
#else
	errno = ENOTSUP;
	return (-1);
#endif
	if (count < 0)
		return (-1);
	*used = (size_t)count;
	return (0);
}

int
fsimg_host_directory_name(void *buffer, size_t length, size_t *cursor,
    const char **name, size_t *name_length)
{
	unsigned char *bytes = buffer;
	size_t start, record_length, name_offset, available;

#if defined(__linux__) || defined(__FreeBSD__)
next_record:
#endif
	if (*cursor > length) {
		errno = EIO;
		return (-1);
	}
	if (*cursor == length)
		return (0);
	start = *cursor;
#if defined(__linux__)
	uint16_t reclen;
	uint64_t inode;

	/* Linux getdents64 has a fixed 19-byte prefix on the 64-bit baseline. */
	if (length - start < 20)
		goto invalid;
	bcopy(bytes + start + 16, &reclen, sizeof(reclen));
	bcopy(bytes + start, &inode, sizeof(inode));
	record_length = reclen;
	name_offset = 19;
#elif defined(__FreeBSD__)
	struct dirent entry;

	if (length - start < offsetof(struct dirent, d_name) + 1)
		goto invalid;
	bzero(&entry, sizeof(entry));
	bcopy(bytes + start, &entry, offsetof(struct dirent, d_name));
	record_length = entry.d_reclen;
	name_offset = offsetof(struct dirent, d_name);
#elif defined(__APPLE__)
	attribute_set_t returned;
	attrreference_t reference;
	uint32_t reclen;

	if (length - start < MACOS_DIRECTORY_PREFIX)
		goto invalid;
	bcopy(bytes + start, &reclen, sizeof(reclen));
	bcopy(bytes + start + 4, &returned, sizeof(returned));
	bcopy(bytes + start + MACOS_NAME_REFERENCE, &reference, sizeof(reference));
	if ((returned.commonattr & ATTR_CMN_NAME) == 0 ||
	    reference.attr_dataoffset < (int32_t)sizeof(reference) || reference.attr_length == 0)
		goto invalid;
	record_length = reclen;
	name_offset = MACOS_NAME_REFERENCE + (size_t)reference.attr_dataoffset;
#else
	return (-1);
#endif
	if (record_length > length - start || name_offset >= record_length)
		goto invalid;
#if defined(__linux__) || defined(__FreeBSD__)
	/* Unused native records carry no live name. Framing still bounds the
	 * next cursor before their name bytes are ignored.
	 */
	*cursor = start + record_length;
#if defined(__linux__)
	if (inode == 0)
#else
	if (entry.d_fileno == 0)
#endif
		goto next_record;
#endif
	available = record_length - name_offset;
#if defined(__APPLE__)
	if (reference.attr_length > available)
		goto invalid;
	available = reference.attr_length;
#endif
	*name = (const char *)bytes + start + name_offset;
	*name_length = strnlen(*name, available);
	if (*name_length == available || *name_length == 0 ||
	    memchr(*name, '/', *name_length) != NULL)
		goto invalid;
	*cursor = start + record_length;
	return (1);
invalid:
	errno = EIO;
	return (-1);
}
