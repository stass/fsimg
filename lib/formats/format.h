/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_FORMAT_H
#define FSIMG_FORMAT_H

#include "reader.h"
#include "writer.h"

struct fsimg_format {
	const char	*name;
	const fsimg_reader_ops_t *reader;
	const fsimg_writer_ops_t *writer;
	unsigned	 variant;
};

const fsimg_format_t *fsimg_format_find(const char *);
fsimg_status_t fsimg_format_open(fsimg_reader_t *, const char *, fsimg_error_t *);

#endif
