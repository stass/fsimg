/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cli.h"
#include "sandbox.h"
#include "unicode.h"

static void append(fsimg_cli_display_t *, const char *, ...);
static void copy_bytes(fsimg_cli_display_t *, const void *, size_t);
static void escape_scalar(fsimg_cli_display_t *, uint32_t, const unsigned char *,
    unsigned, bool);
static fsimg_status_t escape_span(fsimg_cli_display_t *, const void *, size_t,
    bool, bool, bool, fsimg_error_t *);
static void string_value(fsimg_cli_display_t *, const char *);
static void uint_value(fsimg_cli_display_t *, uint64_t);
static const char *confinement_name(fsimg_confinement_t);
static const char *basis_name(uint64_t);
static const char *scope_name(const fsimg_cli_display_t *);
static void timestamp(fsimg_cli_display_t *, const unsigned char *, bool);
static void volume(fsimg_cli_display_t *, const unsigned char *);
static void plan(fsimg_cli_display_t *, const unsigned char *);
static void entry_start(fsimg_cli_display_t *, const unsigned char *);
static void verification(fsimg_cli_display_t *, const unsigned char *);
static void format(fsimg_cli_display_t *, const fsimg_format_info_t *);
static void limits(fsimg_cli_display_t *);
static void output_counts(fsimg_cli_display_t *);
static bool progress_milestone(fsimg_cli_display_t *, const unsigned char *);
static void summary(fsimg_cli_display_t *);
static void failure(fsimg_cli_display_t *);
static fsimg_status_t finish_span(fsimg_cli_display_t *, bool,
    fsimg_worker_text_t *, fsimg_error_t *);

static void
append(fsimg_cli_display_t *display, const char *format_string, ...)
{
	va_list arguments;
	int count;

	if (display->overflow)
		return;
	va_start(arguments, format_string);
	count = vsnprintf(display->buffer + display->used,
	    sizeof(display->buffer) - display->used, format_string, arguments);
	va_end(arguments);
	if (count < 0 || (size_t)count >= sizeof(display->buffer) - display->used) {
		display->overflow = true;
		return;
	}
	display->used += (size_t)count;
}

static void
copy_bytes(fsimg_cli_display_t *display, const void *data, size_t length)
{
	if (length >= sizeof(display->buffer) - display->used) {
		display->overflow = true;
		return;
	}
	bcopy(data, display->buffer + display->used, length);
	display->used += length;
	display->buffer[display->used] = '\0';
}

static void
uint_value(fsimg_cli_display_t *display, uint64_t value)
{
	append(display, "\"%llu\"", (unsigned long long)value);
}

/* Escape terminal controls and directional/invisible formatting in either
 * representation. JSON escapes preserve the scalar's value; human escapes
 * are unambiguous inside a quoted pathname.
 */
static void
escape_scalar(fsimg_cli_display_t *display, uint32_t scalar,
    const unsigned char *encoded, unsigned length, bool json)
{
	bool control;

	if (scalar == '"' || scalar == '\\') {
		append(display, "\\%c", (int)scalar);
		return;
	}
	control = scalar < 32 || (scalar >= 0x7f && scalar <= 0x9f) ||
	    scalar == 0x061c || (scalar >= 0x200b && scalar <= 0x200f) ||
	    (scalar >= 0x2028 && scalar <= 0x202e) ||
	    (scalar >= 0x2060 && scalar <= 0x206f) || scalar == 0xfeff ||
	    (scalar >= 0xe0000 && scalar <= 0xe007f);
	if (!control) {
		copy_bytes(display, encoded, length);
		return;
	}
	if (scalar <= 0xffff)
		append(display, "\\u%04x", scalar);
	else if (json) {
		scalar -= 0x10000;
		append(display, "\\u%04x\\u%04x", 0xd800 + (scalar >> 10),
		    0xdc00 + (scalar & 0x3ff));
	} else
		append(display, "\\U%08x", scalar);
}

static void
escape_invalid(fsimg_cli_display_t *display, bool json)
{
	for (unsigned i = 0; i < display->utf8_used; i++)
		append(display, json ? "\\\\x%02x" : "\\x%02x", display->utf8[i]);
	display->utf8_used = display->utf8_needed = 0;
}

/* NAME frames may divide a UTF-8 scalar. Retain at most three unfinished
 * bytes and validate completed scalars with the common Unicode codec. Invalid
 * worker names fail; arbitrary host diagnostics instead show explicit bytes.
 */
static fsimg_status_t
escape_span(fsimg_cli_display_t *display, const void *data, size_t length,
    bool json, bool strict, bool final, fsimg_error_t *error)
{
	const unsigned char *bytes = data;
	uint16_t units[2];
	uint32_t scalar;
	size_t offset, count;
	unsigned char byte;
	fsimg_status_t status;

	offset = 0;
	while (offset < length) {
		byte = bytes[offset];
		if (display->utf8_used != 0 && (byte & 0xc0) != 0x80) {
			if (strict)
				goto invalid;
			escape_invalid(display, json);
			continue;
		}
		offset++;
		if (display->utf8_used == 0) {
			if (byte < 0x80) {
				escape_scalar(display, byte, &byte, 1, json);
				continue;
			}
			display->utf8_needed = byte >= 0xc2 && byte <= 0xdf ? 2 :
			    byte >= 0xe0 && byte <= 0xef ? 3 :
			    byte >= 0xf0 && byte <= 0xf4 ? 4 : 1;
		}
		display->utf8[display->utf8_used++] = byte;
		if (display->utf8_used != display->utf8_needed)
			continue;
		status = fsimg_utf8_to_utf16((const char *)display->utf8,
		    display->utf8_used, units, 2, &count, NULL);
		if (status != FSIMG_OK) {
			if (strict)
				goto invalid;
			escape_invalid(display, json);
			continue;
		}
		scalar = count == 1 ? units[0] :
		    0x10000 + ((uint32_t)(units[0] - 0xd800) << 10) + units[1] - 0xdc00;
		escape_scalar(display, scalar, display->utf8, display->utf8_used, json);
		display->utf8_used = display->utf8_needed = 0;
	}
	if (final && display->utf8_used != 0) {
		if (strict)
			goto invalid;
		escape_invalid(display, json);
	}
	return (FSIMG_OK);
invalid:
	return (fsimg_error_set(error, FSIMG_EIO, 0, FSIMG_OP_CONTEXT,
	    "worker pathname contains invalid UTF-8"));
}

static void
string_value(fsimg_cli_display_t *display, const char *value)
{
	append(display, "\"");
	escape_span(display, value, strlen(value), display->options->json, false, true, NULL);
	append(display, "\"");
}

static const char *
confinement_name(fsimg_confinement_t confinement)
{
	switch (confinement) {
	case FSIMG_CONFINEMENT_ACTIVE: return ("active");
	case FSIMG_CONFINEMENT_UNAVAILABLE: return ("unavailable");
	case FSIMG_CONFINEMENT_DISABLED: return ("disabled");
	case FSIMG_CONFINEMENT_UNKNOWN: return ("unknown");
	}
	return ("unknown");
}

static const char *
basis_name(uint64_t basis)
{
	switch (basis) {
	case FSIMG_TIME_UTC: return ("utc");
	case FSIMG_TIME_OFFSET: return ("known-offset");
	case FSIMG_TIME_WALL: return ("wall-clock");
	case FSIMG_TIME_PER_TIMESTAMP: return ("per-timestamp");
	}
	return ("unknown");
}

static const char *
scope_name(const fsimg_cli_display_t *display)
{
	if (display->options->command == FSIMG_CLI_FORMATS)
		return ("capabilities");
	switch (display->options->request.kind) {
	case FSIMG_JOB_CREATE: return ("created");
	case FSIMG_JOB_PLAN: return ("planned");
	case FSIMG_JOB_LIST: case FSIMG_JOB_CAT: case FSIMG_JOB_EXTRACT:
		return ("selected-entry");
	default: break;
	}
	if (display->scope == FSIMG_VALIDATION_PAYLOAD)
		return ("initialized-payload");
	if (display->scope == FSIMG_VALIDATION_STRUCTURAL)
		return ("structural");
	return ("header");
}

static void
timestamp(fsimg_cli_display_t *display, const unsigned char *raw, bool valid)
{
	if (!valid) {
		append(display, "null");
		return;
	}
	append(display, "{\"seconds\":\"%lld\",\"nanoseconds\":%llu,\"precision_ns\":",
	    (long long)fsimg_wire_get_signed64(raw + FSIMG_WIRE_TIME_SECONDS),
	    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_TIME_NANOSECONDS));
	uint_value(display, fsimg_wire_get64(raw + FSIMG_WIRE_TIME_PRECISION));
	append(display, ",\"basis\":\"%s\",\"offset_minutes\":%lld}",
	    basis_name(fsimg_wire_get64(raw + FSIMG_WIRE_TIME_BASIS)),
	    (long long)fsimg_wire_get_signed64(raw + FSIMG_WIRE_TIME_OFFSET));
}

static void
volume(fsimg_cli_display_t *display, const unsigned char *raw)
{
	uint64_t valid = fsimg_wire_get64(raw + FSIMG_WIRE_INFO_VALID);
	bool json = display->options->json;

	if (json) {
		append(display, "{\"schema\":1,\"type\":\"info\",\"format\":");
		string_value(display, (const char *)raw + FSIMG_WIRE_INFO_FORMAT);
		append(display, ",\"profile\":");
		string_value(display, (const char *)raw + FSIMG_WIRE_INFO_PROFILE);
		append(display, ",\"label\":");
		if ((valid & FSIMG_VOLUME_LABEL) != 0)
			string_value(display, (const char *)raw + FSIMG_WIRE_INFO_LABEL);
		else
			append(display, "null");
		append(display, ",\"volume_bytes\":");
		uint_value(display, fsimg_wire_get64(raw + FSIMG_WIRE_INFO_VOLUME));
		append(display, ",\"allocation_units\":");
		uint_value(display, fsimg_wire_get64(raw + FSIMG_WIRE_INFO_UNITS));
		append(display, ",\"free_bytes\":");
		if ((valid & FSIMG_VOLUME_FREE) != 0)
			uint_value(display, fsimg_wire_get64(raw + FSIMG_WIRE_INFO_FREE));
		else
			append(display, "null");
		append(display, ",\"sector_bytes\":%llu,\"cluster_bytes\":%llu,"
		    "\"capabilities\":%llu,\"time_basis\":\"%s\",\"validation_scope\":\"header\","
		    "\"entry_count\":null,\"clean_flag\":%s,\"io_error_flag\":%s}\n",
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_INFO_SECTOR),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_INFO_CLUSTER),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_INFO_CAPABILITIES),
		    basis_name(fsimg_wire_get64(raw + FSIMG_WIRE_INFO_TIME_BASIS)),
		    fsimg_wire_get64(raw + FSIMG_WIRE_INFO_CLEAN) ? "true" : "false",
		    fsimg_wire_get64(raw + FSIMG_WIRE_INFO_IO_ERROR) ? "true" : "false");
		return;
	}
	string_value(display, (const char *)raw + FSIMG_WIRE_INFO_FORMAT);
	append(display, " profile=");
	string_value(display, (const char *)raw + FSIMG_WIRE_INFO_PROFILE);
	append(display, " label=");
	if ((valid & FSIMG_VOLUME_LABEL) != 0)
		string_value(display, (const char *)raw + FSIMG_WIRE_INFO_LABEL);
	else
		append(display, "unknown");
	append(display, "\nvolume=%llu bytes sector=%llu bytes cluster=%llu bytes units=%llu\n"
	    "validation=header time-basis=%s entry-count=unknown free-bytes=",
	    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_INFO_VOLUME),
	    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_INFO_SECTOR),
	    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_INFO_CLUSTER),
	    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_INFO_UNITS),
	    basis_name(fsimg_wire_get64(raw + FSIMG_WIRE_INFO_TIME_BASIS)));
	if ((valid & FSIMG_VOLUME_FREE) != 0)
		append(display, "%llu\n", (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_INFO_FREE));
	else
		append(display, "unknown\n");
}

static void
plan(fsimg_cli_display_t *display, const unsigned char *raw)
{
	static const unsigned offsets[] = { FSIMG_WIRE_PLAN_VOLUME, FSIMG_WIRE_PLAN_PAYLOAD,
	    FSIMG_WIRE_PLAN_INITIALIZED, FSIMG_WIRE_PLAN_ALLOCATED, FSIMG_WIRE_PLAN_METADATA,
	    FSIMG_WIRE_PLAN_FREE, FSIMG_WIRE_PLAN_ENTRIES, FSIMG_WIRE_PLAN_LOSSES,
	    FSIMG_WIRE_PLAN_SCRATCH };
	static const char *const keys[] = { "volume_bytes", "payload_bytes", "initialized_bytes",
	    "allocated_bytes", "metadata_bytes", "free_bytes", "entries", "metadata_losses",
	    "scratch_observed_bytes" };

	if (display->options->json) {
		append(display, "{\"schema\":1,\"type\":\"plan\",\"format\":");
		string_value(display, (const char *)raw + FSIMG_WIRE_PLAN_FORMAT);
		for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
			append(display, ",\"%s\":", keys[i]);
			uint_value(display, fsimg_wire_get64(raw + offsets[i]));
		}
		append(display, ",\"sector_bytes\":%llu,\"cluster_bytes\":%llu,"
		    "\"future_resource_estimates\":null,\"source_device_open_exclusion\":false}\n",
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_SECTOR),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_CLUSTER));
	} else {
		string_value(display, (const char *)raw + FSIMG_WIRE_PLAN_FORMAT);
		append(display, " plan: volume=%llu payload=%llu metadata=%llu free=%llu bytes\n"
		    "sector=%llu cluster=%llu bytes entries=%llu metadata-losses=%llu\n"
		    "source profile: special-node opens require isolated source storage\n",
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_VOLUME),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_PAYLOAD),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_METADATA),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_FREE),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_SECTOR),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_CLUSTER),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_ENTRIES),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_LOSSES));
	}
}

static void
entry_start(fsimg_cli_display_t *display, const unsigned char *raw)
{
	uint64_t valid = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_VALID);
	bool directory = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_KIND) == FSIMG_ENTRY_DIRECTORY;

	if (display->options->json) {
		append(display, "{\"schema\":1,\"type\":\"entry\",\"kind\":\"%s\",\"size_bytes\":",
		    directory ? "directory" : "file");
		uint_value(display, fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_LENGTH));
		append(display, ",\"metadata_valid\":%llu,\"attributes\":%llu,\"created\":",
		    (unsigned long long)valid,
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_ATTRIBUTES));
		timestamp(display, raw + FSIMG_WIRE_ENTRY_CREATED, (valid & FSIMG_META_CREATED) != 0);
		append(display, ",\"modified\":");
		timestamp(display, raw + FSIMG_WIRE_ENTRY_MODIFIED, (valid & FSIMG_META_MODIFIED) != 0);
		append(display, ",\"accessed\":");
		timestamp(display, raw + FSIMG_WIRE_ENTRY_ACCESSED, (valid & FSIMG_META_ACCESSED) != 0);
		append(display, ",\"path\":\"");
	} else
		append(display, "%c %llu \"", directory ? 'd' : 'f',
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_LENGTH));
	display->entry_open = true;
}

static void
verification(fsimg_cli_display_t *display, const unsigned char *raw)
{
	static const char *const keys[] = { "files", "directories", "logical_bytes",
	    "initialized_bytes_read", "allocated_clusters", "free_clusters", "bad_clusters" };

	display->scope = (fsimg_validation_scope_t)fsimg_wire_get64(raw + FSIMG_WIRE_VERIFY_SCOPE);
	if (display->options->json) {
		append(display, "{\"schema\":1,\"type\":\"verify\",\"complete\":true,\"validation_scope\":\"%s\"",
		    scope_name(display));
		for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			append(display, ",\"%s\":", keys[i]);
			uint_value(display, fsimg_wire_get64(raw + FSIMG_WIRE_VERIFY_FILES + i * 8));
		}
		append(display, "}\n");
	} else
		append(display, "verified %s: files=%llu directories=%llu initialized-read=%llu bytes\n",
		    scope_name(display),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_VERIFY_FILES),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_VERIFY_DIRECTORIES),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_VERIFY_PAYLOAD));
}

static void
sizes(fsimg_cli_display_t *display, uint32_t mask)
{
	bool first = true;

	append(display, "[");
	for (unsigned shift = 0; shift < 32; shift++) {
		if ((mask & (UINT32_C(1) << shift)) == 0)
			continue;
		append(display, "%s%u", first ? "" : ",", UINT32_C(1) << shift);
		first = false;
	}
	append(display, "]");
}

static void
format(fsimg_cli_display_t *display, const fsimg_format_info_t *info)
{
	if (display->options->json) {
		append(display, "{\"schema\":1,\"type\":\"format\",\"name\":");
		string_value(display, info->name);
		append(display, ",\"profile\":");
		string_value(display, info->profile);
		append(display, ",\"capabilities\":%u,\"entry_kinds\":%u,\"metadata_fields\":%u,"
		    "\"max_file_bytes\":", info->capabilities, info->entry_kinds, info->metadata_fields);
		uint_value(display, info->max_file_bytes);
		append(display, ",\"max_volume_bytes\":");
		uint_value(display, info->max_volume_bytes);
		append(display, ",\"max_name_utf16_units\":%u,\"sector_bytes\":", info->max_name_utf16_units);
		sizes(display, info->sector_size_mask);
		append(display, ",\"read_cluster_bytes\":");
		sizes(display, info->allocation_unit_size_mask);
		append(display, ",\"write_cluster_bytes\":");
		sizes(display, info->write_allocation_unit_size_mask);
		append(display, "}\n");
	} else {
		append(display, "%s read=%s write=%s verify=%s profile=", info->name,
		    (info->capabilities & FSIMG_CAP_READ) ? "yes" : "no",
		    (info->capabilities & FSIMG_CAP_WRITE) ? "yes" : "no",
		    (info->capabilities & FSIMG_CAP_VERIFY) ? "yes" : "no");
		string_value(display, info->profile);
		append(display, "\n  max-file=%llu max-volume=%llu bytes name=%u UTF-16 units\n  sectors=",
		    (unsigned long long)info->max_file_bytes,
		    (unsigned long long)info->max_volume_bytes, info->max_name_utf16_units);
		sizes(display, info->sector_size_mask);
		append(display, " read-clusters=");
		sizes(display, info->allocation_unit_size_mask);
		append(display, " write-clusters=");
		sizes(display, info->write_allocation_unit_size_mask);
		append(display, " bytes metadata-fields=0x%x\n", info->metadata_fields);
	}
}

static void
limits(fsimg_cli_display_t *display)
{
	const fsimg_worker_request_t *r = &display->options->request;

	append(display, "\"limits\":{\"memory_bytes\":\"%llu\",\"scratch_bytes\":\"%llu\","
	    "\"entries\":\"%llu\",\"depth\":\"%u\",\"path_bytes\":\"%u\",\"fds\":\"%u\","
	    "\"output_bytes\":\"%llu\",\"work\":\"%llu\",\"seconds\":\"%llu\",\"scratch_enabled\":%s}",
	    (unsigned long long)r->limits.memory_bytes, (unsigned long long)r->limits.scratch_bytes,
	    (unsigned long long)r->limits.max_entries, r->limits.max_depth, r->limits.max_path_bytes,
	    r->limits.max_open_files, (unsigned long long)r->limits.max_output_bytes,
	    (unsigned long long)r->limits.max_work, (unsigned long long)r->seconds,
	    strcmp(display->options->scratch, "-") == 0 ? "false" : "true");
}

static void
output_counts(fsimg_cli_display_t *display)
{
	const fsimg_output_budget_t *budget = display->options->request.output_budget;

	if (budget == NULL) {
		append(display, "\"text_limit_bytes\":null,\"text_bytes_queued\":null,"
		    "\"stdout_bytes_confirmed\":null,\"stderr_bytes_confirmed\":null");
		return;
	}
	append(display, "\"text_limit_bytes\":\"%llu\",\"text_bytes_queued\":\"%llu\","
	    "\"stdout_bytes_confirmed\":\"%llu\",\"stderr_bytes_confirmed\":\"%llu\"",
	    (unsigned long long)budget->limit_bytes, (unsigned long long)budget->text_bytes,
	    (unsigned long long)budget->confirmed[0], (unsigned long long)budget->confirmed[1]);
}

static void
summary(fsimg_cli_display_t *display)
{
	const fsimg_worker_result_t *r = display->result;

	if (display->options->json) {
		append(display, "{\"schema\":1,\"type\":\"summary\",\"complete\":true,\"command\":");
		string_value(display, display->options->name);
		append(display, ",\"validation_scope\":\"%s\",\"entries_processed\":\"%llu\","
		    "\"logical_bytes\":\"%llu\",\"raw_bytes\":\"%llu\",\"metadata_losses\":\"%llu\","
		    "\"sandbox\":\"%s\",\"published\":%s,", scope_name(display),
		    (unsigned long long)r->entries, (unsigned long long)r->logical_bytes,
		    (unsigned long long)r->raw_bytes, (unsigned long long)r->metadata_losses,
		    confinement_name(r->confinement), display->paths->published ? "true" : "false");
		limits(display);
		append(display, ",\"progress_records\":\"%llu\",", (unsigned long long)display->progress_records);
		output_counts(display);
		append(display, "}\n");
	} else
		append(display, "complete: %s entries=%llu logical-bytes=%llu metadata-losses=%llu sandbox=%s%s\n",
		    scope_name(display), (unsigned long long)r->entries,
		    (unsigned long long)r->logical_bytes, (unsigned long long)r->metadata_losses,
		    confinement_name(r->confinement), display->paths->published ? " published" : "");
}

static void
failure(fsimg_cli_display_t *display)
{
	static const char *const resources[] = { "none", "memory", "scratch", "fds",
	    "entries", "output", "work", "depth", "path" };
	static const char *const states[] = { "absent", "incomplete", "published", "uncertain" };
	const fsimg_error_t *error = display->failure;
	const char *resource, *state;

	resource = (unsigned)error->resource < sizeof(resources) / sizeof(resources[0]) ?
	    resources[error->resource] : "unknown";
	state = (unsigned)error->output_state < sizeof(states) / sizeof(states[0]) ?
	    states[error->output_state] : "unknown";
	if (display->options->json) {
		append(display, "{\"schema\":1,\"type\":\"error\",\"complete\":false,\"code\":%d,\"status\":",
		    error->status);
		string_value(display, fsimg_status_string(error->status));
		append(display, ",\"message\":");
		string_value(display, error->message);
		append(display, ",\"path\":");
		string_value(display, error->path);
		append(display, ",\"resource\":\"%s\",\"output_state\":\"%s\",\"os_error\":%d,"
		    "\"cleanup_error\":%d,\"limit\":\"%llu\",\"observed\":\"%llu\","
		    "\"path_truncated\":%s,\"message_truncated\":%s,\"offset_bytes\":",
		    resource, state, error->os_error, error->cleanup_error,
		    (unsigned long long)error->limit, (unsigned long long)error->observed,
		    error->path_truncated ? "true" : "false", error->message_truncated ? "true" : "false");
		if (error->offset_valid) uint_value(display, error->offset);
		else append(display, "null");
		append(display, ",\"retained_output\":");
		string_value(display, display->paths->retained_output);
		append(display, ",\"retained_scratch\":");
		string_value(display, display->paths->retained_scratch);
		append(display, ",\"sandbox\":\"%s\",", confinement_name(display->result->confinement));
		limits(display);
		append(display, ",");
		output_counts(display);
		append(display, "}\n");
		return;
	}
	append(display, "fsimg: %s: ", fsimg_status_string(error->status));
	string_value(display, error->message);
	append(display, " (resource=%s output=%s errno=%d cleanup-errno=%d)", resource,
	    state, error->os_error, error->cleanup_error);
	if (error->path[0] != '\0') {
		append(display, " path=");
		string_value(display, error->path);
	}
	if (error->resource != FSIMG_RESOURCE_NONE)
		append(display, " limit=%llu observed=%llu", (unsigned long long)error->limit,
		    (unsigned long long)error->observed);
	if (display->paths->retained_output[0] != '\0') {
		append(display, " retained-output=");
		string_value(display, display->paths->retained_output);
	}
	if (display->paths->retained_scratch[0] != '\0') {
		append(display, " retained-scratch=");
		string_value(display, display->paths->retained_scratch);
	}
	append(display, "\n");
}

void
fsimg_cli_display_init(fsimg_cli_display_t *display, const fsimg_cli_options_t *options,
    const fsimg_worker_result_t *result, const fsimg_cli_paths_t *paths)
{
	bzero(display, sizeof(*display));
	display->options = options;
	display->result = result;
	display->paths = paths;
	display->scope = FSIMG_VALIDATION_HEADER;
}

static fsimg_status_t
finish_span(fsimg_cli_display_t *display, bool diagnostic,
    fsimg_worker_text_t *text, fsimg_error_t *error)
{
	if (display->overflow)
		return (fsimg_error_set(error, FSIMG_ELIMIT, 0,
		    FSIMG_OP_CONTEXT, "formatted output exceeds its fixed buffer"));
	*text = (fsimg_worker_text_t){ .data = display->buffer,
	    .length = display->used, .diagnostic = diagnostic };
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_cli_event(void *argument, const fsimg_frame_t *frame,
    fsimg_worker_text_t *text, fsimg_error_t *error)
{
	fsimg_cli_display_t *display = argument;
	const fsimg_cli_options_t *options = display->options;
	const unsigned char *raw = frame->payload;
	bool diagnostic, listing;
	fsimg_status_t status;

	display->used = 0;
	diagnostic = false;
	listing = options->request.kind == FSIMG_JOB_LIST;
	switch (frame->type) {
	case FSIMG_FRAME_READY:
		if ((options->request.flags & FSIMG_JOB_VERBOSE) != 0) {
			diagnostic = true;
			if (options->json)
				append(display, "{\"schema\":1,\"type\":\"capabilities\",\"sandbox\":\"%s\"}\n",
				    confinement_name(display->result->confinement));
			else append(display, "fsimg: sandbox %s\n", confinement_name(display->result->confinement));
		}
		break;
	case FSIMG_FRAME_INFO:
		if (options->json || options->request.kind == FSIMG_JOB_INFO)
			volume(display, raw);
		break;
	case FSIMG_FRAME_PLAN: plan(display, raw); break;
	case FSIMG_FRAME_ENTRY:
		if (listing) entry_start(display, raw);
		break;
	case FSIMG_FRAME_NAME:
		if (listing) {
			status = escape_span(display, raw, frame->length, options->json, true, false, error);
			if (status != FSIMG_OK) return (status);
		}
		break;
	case FSIMG_FRAME_ENTRY_END:
		if (listing) {
			status = escape_span(display, NULL, 0, options->json, true, true, error);
			if (status != FSIMG_OK) return (status);
			append(display, options->json ? "\",\"complete\":true}\n" : "\"\n");
			display->entry_open = false;
		}
		break;
	case FSIMG_FRAME_VERIFY: verification(display, raw); break;
	case FSIMG_FRAME_PROGRESS:
		if (!progress_milestone(display, raw))
			break;
		diagnostic = true;
		append(display, options->json ?
		    "{\"schema\":1,\"type\":\"progress\",\"entries\":\"%llu\",\"bytes\":\"%llu\",\"work\":\"%llu\"}\n" :
		    "fsimg: entries=%llu bytes=%llu work=%llu\n",
		    (unsigned long long)fsimg_wire_get64(raw),
		    (unsigned long long)fsimg_wire_get64(raw + 8),
		    (unsigned long long)fsimg_wire_get64(raw + 16));
		break;
	case FSIMG_FRAME_LOSS:
		diagnostic = true;
		if (options->json)
			append(display, "{\"schema\":1,\"type\":\"loss\",\"fields\":%llu,\"path_truncated\":%s,\"path_excerpt\":\"",
			    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_LOSS_FIELDS),
			    fsimg_wire_get64(raw + FSIMG_WIRE_LOSS_TRUNCATED) ? "true" : "false");
		else append(display, "fsimg: optional metadata loss fields=0x%llx path=\"",
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_LOSS_FIELDS));
		escape_span(display, raw + FSIMG_WIRE_LOSS_PATH,
		    (size_t)fsimg_wire_get64(raw + FSIMG_WIRE_LOSS_LENGTH), options->json, false, true, NULL);
		append(display, options->json ? "\"}\n" : "\"\n");
		break;
	default: break;
	}
	return (finish_span(display, diagnostic, text, error));
}

/* Each unsigned counter has only 64 nonzero bit lengths. Remembering the
 * greatest level prevents repeated or decreasing reports from growing text;
 * three counters plus an initial zero report permit at most 193 records.
 */
static bool
progress_milestone(fsimg_cli_display_t *display, const unsigned char *raw)
{
	uint64_t value;
	unsigned level;
	bool changed;

	changed = !display->progress_started;
	display->progress_started = true;
	for (unsigned i = 0; i < 3; i++) {
		value = fsimg_wire_get64(raw + i * 8);
		level = 0;
		while (value != 0) {
			level++;
			value >>= 1;
		}
		if (level > display->progress_levels[i]) {
			display->progress_levels[i] = level;
			changed = true;
		}
	}
	if (changed)
		display->progress_records++;
	return (changed);
}

fsimg_status_t
fsimg_cli_final(void *argument, fsimg_worker_text_t *text, fsimg_error_t *error)
{
	fsimg_cli_display_t *display = argument;
	const fsimg_cli_options_t *options = display->options;
	fsimg_format_info_t info;
	fsimg_status_t status;
	bool diagnostic;

	display->used = 0;
	display->overflow = false;
	diagnostic = false;
	if (display->failure != NULL) {
		if (display->entry_open) {
			escape_invalid(display, options->json);
			append(display, options->json ? "\",\"complete\":false}\n" : "\" [incomplete]\n");
			display->entry_open = false;
		} else if (display->phase != FSIMG_CLI_OUTPUT_DONE) {
			failure(display);
			diagnostic = !options->json;
			display->phase = FSIMG_CLI_OUTPUT_DONE;
		} else
			return (FSIMG_DONE);
	} else if (options->command == FSIMG_CLI_FORMATS) {
		if (display->phase == FSIMG_CLI_OUTPUT_RECORDS) {
			status = fsimg_format_next(&display->format_cursor, &info, error);
			if (status == FSIMG_OK) {
				format(display, &info);
				return (finish_span(display, false, text, error));
			}
			if (status != FSIMG_DONE)
				return (status);
			display->phase = FSIMG_CLI_OUTPUT_CAPABILITIES;
		}
		if (display->phase == FSIMG_CLI_OUTPUT_CAPABILITIES) {
			if (options->json)
				append(display, "{\"schema\":1,\"type\":\"capabilities\",\"sandbox_backend\":\"%s\","
				    "\"sandbox_runtime\":\"%s\",\"source_device_open_exclusion\":false,"
				    "\"posix_modes\":false,\"ownership\":false,\"links\":false,\"boot_loader\":false}\n",
				    fsimg_sandbox_backend(), strcmp(fsimg_sandbox_backend(), "none") == 0 ?
				    "unavailable" : "not-probed");
			else append(display, "sandbox backend=%s runtime=%s\n"
			    "content profile: regular files/directories and representable timestamps; no modes, ownership, links or boot loader\n"
			    "source profile requires isolated storage to exclude special-node opens\n",
			    fsimg_sandbox_backend(), strcmp(fsimg_sandbox_backend(), "none") == 0 ?
			    "unavailable" : "not-probed");
			display->phase = FSIMG_CLI_OUTPUT_SUMMARY;
		} else if (display->phase == FSIMG_CLI_OUTPUT_SUMMARY && options->json) {
			summary(display);
			display->phase = FSIMG_CLI_OUTPUT_DONE;
		} else
			return (FSIMG_DONE);
	} else {
		if (display->phase == FSIMG_CLI_OUTPUT_DONE)
			return (FSIMG_DONE);
		display->phase = FSIMG_CLI_OUTPUT_DONE;
		if (options->command == FSIMG_CLI_HELP)
			append(display, "Usage: fsimg create [options] -t format -o image source_directory\n"
			    "       fsimg info|verify [options] -i image\n"
			    "       fsimg ls [options] -i image [image_path]\n"
			    "       fsimg cat [options] -i image image_path\n"
			    "       fsimg extract [options] -i image -C new_directory [image_path]\n"
			    "       fsimg formats [-j] | -h | -V\n\n"
			    "Common: -S auto|required|off -T scratch_directory|- -j -m bytes -l key=value -v\n"
			    "Limits: scratch,entries,depth,path,fds,output,work,seconds (zero selects defaults)\n"
			    "Read:   -b offset_bytes -z length_bytes -t format -O oem=cp437\n"
			    "Create: -L label -R [-e signed_epoch] -f free_bytes -s exact_bytes -n\n"
			    "        -O sector=bytes -O cluster=bytes\n"
			    "ls: -r recursive; verify: -d initialized payload read-through; cat rejects -j\n"
			    "Byte suffixes: K M G T P (powers of 1024). Options precede operands.\n"
			    "Outputs must be new. Inputs are seekable regular files; no stdin spooling.\n");
		else if (options->command == FSIMG_CLI_VERSION)
			append(display, "fsimg %s\n", PACKAGE_VERSION);
		else {
			diagnostic = options->request.kind == FSIMG_JOB_CAT;
			if (diagnostic && (options->request.flags & FSIMG_JOB_VERBOSE) == 0)
				return (FSIMG_DONE);
			summary(display);
		}
	}
	return (finish_span(display, diagnostic, text, error));
}
