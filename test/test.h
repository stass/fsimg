/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_TEST_H
#define FSIMG_TEST_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
		exit(1); \
	} \
} while (0)

#endif
