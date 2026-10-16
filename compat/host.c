/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>

#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include "host.h"

_Static_assert(sizeof(off_t) == sizeof(int64_t), "64-bit off_t required");

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
