/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdint.h>
#include <strings.h>

#include "worker.h"

int LLVMFuzzerTestOneInput(const uint8_t *, size_t);

int
LLVMFuzzerTestOneInput(const uint8_t *bytes, size_t length)
{
	fsimg_frame_decoder_t decoder;
	fsimg_worker_request_t request;
	fsimg_error_t error;
	uint64_t selection_length;
	size_t offset, consumed;
	unsigned capabilities;
	fsimg_status_t status;

	bzero(&decoder, sizeof(decoder));
	offset = 0;
	/* Each input is a finite stream, including concatenated/truncated frames.
	 * Semantic validators share the same implementation as the supervisor.
	 */
	for (unsigned frames = 0; offset < length && frames < 1024; frames++) {
		status = fsimg_frame_feed(&decoder, bytes + offset, length - offset,
		    &consumed, &error);
		if (status != FSIMG_OK || consumed == 0)
			return (0);
		offset += consumed;
		if (!decoder.complete)
			continue;
		switch (decoder.frame.type) {
		case FSIMG_FRAME_JOB:
			fsimg_job_decode(decoder.frame.payload, decoder.frame.length,
			    &request, &selection_length, &capabilities, &error);
			break;
		case FSIMG_FRAME_INFO:
			fsimg_wire_info_frame_valid(&decoder.frame, UINT64_MAX);
			break;
		case FSIMG_FRAME_IMAGE:
			fsimg_wire_image_frame_valid(&decoder.frame);
			break;
		case FSIMG_FRAME_PARTITION:
			fsimg_wire_partition_frame_valid(&decoder.frame, 1024, length);
			break;
		case FSIMG_FRAME_PLAN:
			fsimg_wire_plan_frame_valid(&decoder.frame);
			break;
		case FSIMG_FRAME_ENTRY:
			fsimg_wire_entry_frame_valid(&decoder.frame, (length & 1u) != 0, 65536);
			break;
		case FSIMG_FRAME_VERIFY:
			fsimg_wire_verify_frame_valid(&decoder.frame, (length & 1u) != 0);
			break;
		case FSIMG_FRAME_ERROR:
			fsimg_wire_error_decode(decoder.frame.payload, decoder.frame.length, &error);
			break;
		default:
			break;
		}
		fsimg_frame_advance(&decoder);
	}
	fsimg_frame_finish(&decoder, &error);
	return (0);
}
