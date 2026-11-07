/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "write-erofs.h"

/*
 * Emission places the plan's bytes. The writer has no emitter, so a plan
 * cannot become an image; planning alone is complete.
 */
fsimg_status_t
erofs_write_emit(fsimg_writer_t *writer FSIMG_UNUSED,
    fsimg_emit_t *emit FSIMG_UNUSED, fsimg_error_t *error)
{
	return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_WRITER,
	    "the EROFS writer cannot emit an image"));
}
