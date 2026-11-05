/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_TEST_H
#define FSIMG_TEST_H

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
		exit(1); \
	} \
} while (0)

/* Count the descriptor range used by these test processes. */
static inline int
fsimg_test_open_descriptors(void)
{
	int count = 0;

	for (int fd = 0; fd < 1024; fd++) {
		if (fcntl(fd, F_GETFD) != -1)
			count++;
	}
	return (count);
}

#endif

