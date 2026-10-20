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
static void prose(fsimg_cli_display_t *, const char *);
static void byte_size(fsimg_cli_display_t *, uint64_t);
static void usage(fsimg_cli_display_t *, bool);
static void help(fsimg_cli_display_t *);
static void loss_notice(fsimg_cli_display_t *);
static void metadata_fields(fsimg_cli_display_t *, uint64_t);
static void entry_kinds(fsimg_cli_display_t *, uint64_t);
static const char *kind_name(uint64_t);
static void uuid_value(fsimg_cli_display_t *, const unsigned char *);
static void owner_fields(fsimg_cli_display_t *, const unsigned char *, uint64_t);
static void format_details(fsimg_cli_display_t *, const fsimg_format_info_t *);
static void string_value(fsimg_cli_display_t *, const char *);
static void path_excerpt(fsimg_cli_display_t *, const char *);
static void uint_value(fsimg_cli_display_t *, uint64_t);
static const char *confinement_name(fsimg_confinement_t);
static const char *basis_name(uint64_t);
static const char *verified_scope_name(fsimg_validation_scope_t);
static const char *scope_name(const fsimg_cli_display_t *);
static void timestamp(fsimg_cli_display_t *, const unsigned char *, bool);
static void volume(fsimg_cli_display_t *, const unsigned char *);
static void plan(fsimg_cli_display_t *, const unsigned char *);
static void civil_time(int64_t, char *, size_t);
static void long_listing_columns(fsimg_cli_display_t *, const unsigned char *);
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

static void
path_excerpt(fsimg_cli_display_t *display, const char *path)
{
	size_t length;
	bool truncated;

	/* Bound the escaped operand here so later outcome and cleanup lines
	 * always fit, independently of upstream host-path validation.
	 */
	length = strnlen(path, 257);
	truncated = length > 256;
	if (truncated)
		length = 256;
	append(display, "\"");
	escape_span(display, path, length, false, false, true, NULL);
	append(display, "\"%s", truncated ? " [path truncated]" : "");
}

/* Prose remains terminal-safe without the delimiters used for names/JSON. */
static void
prose(fsimg_cli_display_t *display, const char *value)
{
	escape_span(display, value, strlen(value), false, false, true, NULL);
}

static void
byte_size(fsimg_cli_display_t *display, uint64_t value)
{
	static const char *const units[] = { "bytes", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
	uint64_t unit, remainder;
	unsigned index;

	unit = 1;
	index = 0;
	while (index < sizeof(units) / sizeof(units[0]) - 1 && value / unit >= 1024) {
		unit *= 1024;
		index++;
	}
	remainder = value % unit;
	append(display, "%llu", (unsigned long long)(value / unit));
	if (remainder != 0)
		append(display, ".%llu", (unsigned long long)(remainder * 10 / unit));
	append(display, " %s", units[index]);
	if (remainder != 0)
		append(display, " (%llu bytes)", (unsigned long long)value);
}

/* The synopsis lines of each mode, and the overview listing them all. */
static const char synopsis_create[] =
    "usage: fsimg c[W] -f image -F stack [options] [-C dir] operand ...\n";
static const char synopsis_extract[] =
    "usage: fsimg x -f image [options] [-C dir] [member ...]\n"
    "       fsimg x -O -f image [options] member\n";
static const char synopsis_list[] = "usage: fsimg t -f image [options] [member ...]\n";
static const char synopsis_verify[] = "usage: fsimg W -f image [options]\n";
static const char synopsis_info[] = "usage: fsimg i -f image [options]\n";
static const char synopsis_formats[] = "usage: fsimg --formats [--json] [-v]\n";

static void
usage(fsimg_cli_display_t *display, bool overview)
{
	const char *mode = display->options->mode;

	if (overview || mode[0] == '\0') {
		append(display,
		    "usage: fsimg c[W] -f image -F stack [options] [-C dir] operand ...\n"
		    "       fsimg x -f image [options] [-C dir] [member ...]\n"
		    "       fsimg x -O -f image [options] member\n"
		    "       fsimg t -f image [options] [member ...]\n"
		    "       fsimg W -f image [options]\n"
		    "       fsimg i -f image [options]\n"
		    "       fsimg --formats [--json] [-v]\n"
		    "       fsimg -h | -V\n");
		if (!overview && mode[0] == '\0')
			return;
		append(display, "\nmodes:\n"
		    "    c          Create an image from host operands\n"
		    "    x          Extract into a new directory, or one member to stdout with -O\n"
		    "    t          List members\n"
		    "    W          Verify an image; cW verifies a new image before publication\n"
		    "    i, --info  Show the stack and volume information\n"
		    "    --formats  List supported formats\n\n"
		    "The first argument may bundle the mode and option letters: fsimg xvf boot.img.\n"
		    "Use fsimg MODE -h, such as fsimg x -h, for mode help.\n"
		    "Use fsimg -V for the version; see fsimg(1) for full documentation.\n");
		return;
	}
	if (mode[0] == 'c')
		append(display, synopsis_create);
	else if (strcmp(mode, "x") == 0)
		append(display, synopsis_extract);
	else if (strcmp(mode, "t") == 0)
		append(display, synopsis_list);
	else if (strcmp(mode, "W") == 0)
		append(display, synopsis_verify);
	else if (strcmp(mode, "i") == 0)
		append(display, synopsis_info);
	else
		append(display, synopsis_formats);
}

/* Option descriptions shared by the image modes, in option order. */
static const char help_budgets[] =
    "    -l key=value    Limit scratch, spool, entries, depth, path, fds, output,\n"
    "                    work or seconds\n";
static const char help_memory[] =
    "    -m bytes        Memory budget (default 256M, minimum 16M)\n";
static const char help_read_common[] =
    "    -F stack        Force the filesystem decoder, retaining validation\n"
    "    -f image        Read a seekable regular image file\n"
    "    -h              Show mode help\n"
    "    --json          Write newline-delimited JSON\n";
static const char help_slice[] =
    "    --length=bytes  Volume slice length (default the rest of the image)\n";
static const char help_settings_read[] =
    "    -o oem=cp437    Set FAT short-name encoding\n"
    "    --offset=bytes  Volume slice offset (default 0)\n";
static const char help_policies[] =
    "    -S policy       Sandbox: auto (default), required or off\n"
    "    -T directory    Scratch parent; - disables scratch\n";
static const char help_units[] =
    "\nOptions precede operands. K, M, G, T and P are powers of 1024.\n";

static void
help(fsimg_cli_display_t *display)
{
	const char *mode = display->options->mode;

	usage(display, mode[0] == '\0');
	if (mode[0] == '\0')
		return;
	append(display, "\noptions:\n");
	if (strcmp(mode, "formats") == 0) {
		append(display, "    -h              Show mode help\n"
		    "    --json          Write newline-delimited JSON\n"
		    "    -v              Show profiles, geometry and sandbox capabilities on stderr\n\n"
		    "example:\n    fsimg --formats -v\n");
		return;
	}
	if (mode[0] == 'c') {
		append(display, "    -C dir          Open the operands after it relative to dir\n"
		    "    -d              With W, also read initialized file data\n"
		    "    -e epoch        Reproducible Unix timestamp; requires -R\n"
		    "    -F stack        Required filesystem: fat12, fat16, fat32, exfat, qnx6\n"
		    "                    or qnx4\n"
		    "    -f image        New image pathname; optional with -n\n"
		    "    --free-space=bytes\n"
		    "                    Minimum free space after allocation\n"
		    "    -h              Show mode help\n"
		    "    --json          Write newline-delimited JSON\n"
		    "    -L label        Volume label (default FSIMG where the format has one)\n");
		append(display, "%s%s", help_budgets, help_memory);
		append(display, "    -n              Report a plan without creating an image\n"
		    "    -o key=value    Filesystem setting: sector, cluster, endian, uuid,\n"
		    "                    inodes or reserve\n"
		    "    -p              Import owner, group and permission bits\n"
		    "    -R              Create reproducible image bytes\n");
		append(display, "%s", help_policies);
		append(display, "    -s bytes        Exact image size (default automatic)\n"
		    "    -v              Repeat as -vv for progress, details and sampled\n"
		    "                    metadata losses on stderr\n"
		    "    -W              Verify the staged image before publication\n");
		append(display, "%s", help_units);
		append(display, "Only -C dir may appear among the operands, for the operands after it.\n"
		    "Each operand is stored under its own path; -C dir . stores the contents\n"
		    "of dir at the image root. The output must be new. Keep the sources stable\n"
		    "during creation.\n\n"
		    "examples:\n"
		    "    fsimg cf boot.img -F fat32 -L BOOT -s 64M -C esp .\n"
		    "    fsimg cRf data.img -F exfat rootfs extra\n"
		    "    fsimg cWf data.img -F exfat -C esp . -C extra EFI\n"
		    "    fsimg cn -F exfat -R rootfs\n");
		return;
	}
	if (strcmp(mode, "x") == 0) {
		append(display, "    -C dir          Existing parent of the new extraction directory\n"
		    "                    (default the working directory)\n"
		    "    -E              Extract into the empty private -C directory itself\n");
		append(display, "%s", help_read_common);
		append(display, "    -k              Skip and count symbolic links and other unsupported kinds\n");
		append(display, "%s%s%s", help_budgets, help_slice, help_memory);
		append(display, "    -O              Write one member's bytes to stdout\n");
		append(display, "%s%s", help_settings_read, help_policies);
		append(display, "    -v              Repeat as -vv for progress and details on stderr\n");
		append(display, "%s", help_units);
		append(display, "Extracts into a new directory named after the image, such as ./boot for\n"
		    "boot.img, with private modes and without host timestamps. Symbolic links\n"
		    "and other unsupported kinds fail unless -k skips them.\n\n"
		    "examples:\n"
		    "    fsimg xf boot.img\n"
		    "    fsimg xf boot.img -C /tmp EFI/BOOT\n"
		    "    fsimg xEf boot.img -C ./empty-dir\n"
		    "    fsimg xOf boot.img /EFI/BOOT/BOOTX64.EFI > BOOTX64.EFI\n");
		return;
	}
	if (strcmp(mode, "W") == 0)
		append(display, "    -d              Also read initialized file data\n");
	append(display, "%s%s%s%s", help_read_common, help_budgets, help_slice, help_memory);
	if (strcmp(mode, "t") == 0)
		append(display, "    --no-recursion  List one level\n");
	append(display, "%s%s", help_settings_read, help_policies);
	if (strcmp(mode, "t") == 0)
		append(display, "    -v              Long listing; -vv adds details on stderr\n");
	else
		append(display, "    -v              Show capability, geometry and sandbox details on stderr\n");
	append(display, "%s", help_units);
	if (strcmp(mode, "t") == 0)
		append(display, "\nexamples:\n    fsimg tvf disk.img\n"
		    "    fsimg tf disk.img --no-recursion /EFI\n");
	else if (strcmp(mode, "W") == 0)
		append(display, "Checks the filesystem without repairing it.\n\n"
		    "example:\n    fsimg Wdf disk.img\n");
	else
		append(display, "\nexample:\n    fsimg if disk.img\n");
}

static void
metadata_fields(fsimg_cli_display_t *display, uint64_t fields)
{
	static const char *const names[] = {
		"creation time", "modification time", "access time", "attributes",
		"change time", "owner", "mode"
	};
	bool first = true;

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if ((fields & (UINT64_C(1) << i)) == 0)
			continue;
		append(display, "%s%s", first ? "" : ", ", names[i]);
		first = false;
	}
}

static const char *
kind_name(uint64_t kind)
{
	switch (kind) {
	case FSIMG_ENTRY_FILE: return ("file");
	case FSIMG_ENTRY_DIRECTORY: return ("directory");
	case FSIMG_ENTRY_SYMLINK: return ("symlink");
	case FSIMG_ENTRY_FIFO: return ("fifo");
	}
	return ("unknown");
}

/* Name each known kind bit in ascending order, the FSIMG_ENTRY_* order. */
static void
entry_kinds(fsimg_cli_display_t *display, uint64_t kinds)
{
	uint64_t rest, kind;
	bool first = true;

	for (rest = kinds & FSIMG_ENTRY_KNOWN; rest != 0; rest &= rest - 1) {
		kind = rest & (~rest + 1);	/* the lowest remaining bit */
		append(display, "%s%s", first ? "" : ", ", kind_name(kind));
		first = false;
	}
}

static void
loss_notice(fsimg_cli_display_t *display)
{
	uint64_t losses = display->result->metadata_losses;
	uint64_t skipped = display->result->skipped_entries;
	const char *mode = display->options->mode;

	/* Examples are a stderr detail: -vv in c and x, whose -v is the entry
	 * stream.
	 */
	if (losses != 0)
		append(display, "fsimg: warning: %llu metadata field%s could not be preserved exactly%s\n",
		    (unsigned long long)losses, losses == 1 ? "" : "s",
		    (display->options->request.flags & FSIMG_JOB_VERBOSE) != 0 ? "" :
		    mode != NULL && (mode[0] == 'c' || mode[0] == 'x') ?
		    "; use -vv for examples" : "; use -v for examples");
	if (skipped != 0)
		append(display, "fsimg: warning: %llu %s skipped\n",
		    (unsigned long long)skipped, skipped == 1 ?
		    "entry of an unsupported kind was" :
		    "entries of unsupported kinds were");
}

/* A UUID's sixteen bytes in textual order, as RFC 4122 groups them. */
static void
uuid_value(fsimg_cli_display_t *display, const unsigned char *uuid)
{
	for (unsigned i = 0; i < 16; i++)
		append(display, "%s%02x", i == 4 || i == 6 || i == 8 || i == 10 ?
		    "-" : "", uuid[i]);
}

/* JSON owner and mode, each null when its field is absent. */
static void
owner_fields(fsimg_cli_display_t *display, const unsigned char *raw,
    uint64_t valid)
{
	if ((valid & FSIMG_META_OWNER) != 0)
		append(display, ",\"uid\":%llu,\"gid\":%llu",
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_UID),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_GID));
	else
		append(display, ",\"uid\":null,\"gid\":null");
	if ((valid & FSIMG_META_MODE) != 0)
		append(display, ",\"mode\":%llu",
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_MODE));
	else
		append(display, ",\"mode\":null");
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

/* The scope a verification reached, from the VERIFY record itself, so cW's
 * verification of its staged image reports structural or payload reads.
 */
static const char *
verified_scope_name(fsimg_validation_scope_t scope)
{
	if (scope == FSIMG_VALIDATION_PAYLOAD)
		return ("initialized-payload");
	if (scope == FSIMG_VALIDATION_STRUCTURAL)
		return ("structural");
	return ("header");
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
	return (verified_scope_name(display->scope));
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
	char format_name[16];
	bool json = display->options->json;

	if (json) {
		/* A bare volume's stack is its filesystem alone, and it is not
		 * compressed.
		 */
		append(display, "{\"schema\":1,\"type\":\"info\",\"stack\":");
		string_value(display, (const char *)raw + FSIMG_WIRE_INFO_FORMAT);
		append(display, ",\"compression\":null,\"format\":");
		string_value(display, (const char *)raw + FSIMG_WIRE_INFO_FORMAT);
		append(display, ",\"profile\":");
		string_value(display, (const char *)raw + FSIMG_WIRE_INFO_PROFILE);
		append(display, ",\"label\":");
		if ((valid & FSIMG_VOLUME_LABEL) != 0)
			string_value(display, (const char *)raw + FSIMG_WIRE_INFO_LABEL);
		else
			append(display, "null");
		append(display, ",\"uuid\":");
		if ((valid & FSIMG_VOLUME_UUID) != 0) {
			append(display, "\"");
			uuid_value(display, raw + FSIMG_WIRE_INFO_UUID);
			append(display, "\"");
		} else
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
	append(display, "Stack:        ");
	prose(display, (const char *)raw + FSIMG_WIRE_INFO_FORMAT);
	append(display, "\nCompression:  none\nFormat:       ");
	bcopy(raw + FSIMG_WIRE_INFO_FORMAT, format_name, sizeof(format_name));
	format_name[sizeof(format_name) - 1] = '\0';
	for (size_t i = 0; format_name[i] != '\0'; i++) {
		if (format_name[i] >= 'a' && format_name[i] <= 'z')
			format_name[i] = (char)(format_name[i] - 'a' + 'A');
	}
	prose(display, format_name);
	append(display, "\nLabel:        ");
	if ((valid & FSIMG_VOLUME_LABEL) != 0)
		string_value(display, (const char *)raw + FSIMG_WIRE_INFO_LABEL);
	else
		append(display, "unknown");
	if ((valid & FSIMG_VOLUME_UUID) != 0) {
		append(display, "\nUUID:         ");
		uuid_value(display, raw + FSIMG_WIRE_INFO_UUID);
	}
	append(display, "\nVolume size:  ");
	byte_size(display, fsimg_wire_get64(raw + FSIMG_WIRE_INFO_VOLUME));
	append(display, "\nSector size:  ");
	byte_size(display, fsimg_wire_get64(raw + FSIMG_WIRE_INFO_SECTOR));
	append(display, "\nCluster size: ");
	byte_size(display, fsimg_wire_get64(raw + FSIMG_WIRE_INFO_CLUSTER));
	append(display, "\nFree space:   ");
	if ((valid & FSIMG_VOLUME_FREE) != 0)
		byte_size(display, fsimg_wire_get64(raw + FSIMG_WIRE_INFO_FREE));
	else
		append(display, "unknown (not scanned)");
	append(display, "\nValidation:   header only\nTime basis:   %s\nProfile:      ",
	    basis_name(fsimg_wire_get64(raw + FSIMG_WIRE_INFO_TIME_BASIS)));
	prose(display, (const char *)raw + FSIMG_WIRE_INFO_PROFILE);
	append(display, "\nCapabilities: read%s%s\n",
	    (fsimg_wire_get64(raw + FSIMG_WIRE_INFO_CAPABILITIES) & FSIMG_CAP_WRITE) != 0 ? ", write" : "",
	    (fsimg_wire_get64(raw + FSIMG_WIRE_INFO_CAPABILITIES) & FSIMG_CAP_VERIFY) != 0 ? ", verify" : "");
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
		append(display, "Format:       ");
		prose(display, (const char *)raw + FSIMG_WIRE_PLAN_FORMAT);
		append(display, "\nVolume size:  ");
		byte_size(display, fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_VOLUME));
		append(display, "\nFile data:    ");
		byte_size(display, fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_PAYLOAD));
		append(display, "\nMetadata:     ");
		byte_size(display, fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_METADATA));
		append(display, "\nFree space:   ");
		byte_size(display, fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_FREE));
		append(display, "\nSector size:  ");
		byte_size(display, fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_SECTOR));
		append(display, "\nCluster size: ");
		byte_size(display, fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_CLUSTER));
		append(display, "\nEntries:      %llu\nScratch used: ",
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_ENTRIES));
		byte_size(display, fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_SCRATCH));
		append(display, "\n");
	}
}

/*
 * "YYYY-MM-DD HH:MM:SS" of seconds since 1970-01-01 in the proleptic
 * Gregorian calendar, with no locale or host timezone. Days are counted from
 * 0000-03-01 in 400-year eras of 146097 days, so leap days fall at the end
 * of each counted year and the month lengths follow a fixed pattern.
 */
static void
civil_time(int64_t seconds, char *text, size_t capacity)
{
	int64_t days, era, year;
	int64_t day_of_era, year_of_era, day_of_year, month_index;
	int64_t second_of_day;
	unsigned month, day;

	days = seconds / 86400;
	second_of_day = seconds % 86400;
	if (second_of_day < 0) {
		second_of_day += 86400;
		days--;
	}
	days += 719468;
	era = (days >= 0 ? days : days - 146096) / 146097;
	day_of_era = days - era * 146097;
	year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
	    day_of_era / 146096) / 365;
	day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 -
	    year_of_era / 100);
	month_index = (5 * day_of_year + 2) / 153;
	day = (unsigned)(day_of_year - (153 * month_index + 2) / 5 + 1);
	month = (unsigned)(month_index < 10 ? month_index + 3 : month_index - 9);
	year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
	snprintf(text, capacity, "%04lld-%02u-%02u %02u:%02u:%02u", (long long)year,
	    month, day, (unsigned)(second_of_day / 3600),
	    (unsigned)(second_of_day % 3600 / 60), (unsigned)(second_of_day % 60));
}

/* The -v listing's columns before the name: the kind and nine permission
 * characters, or nine '?' when the format stores no mode; the logical
 * length of a file or link, or '-'; and the modification time, or '-'.
 * Unknown-offset times encode their calendar fields as UTC seconds, so one
 * conversion serves every basis.
 */
static void
long_listing_columns(fsimg_cli_display_t *display, const unsigned char *raw)
{
	static const char kinds[] = "-dlp";
	static const char permissions[] = "rwxrwxrwx";
	uint64_t valid = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_VALID);
	uint64_t kind = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_KIND);
	uint64_t mode = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_MODE);
	char column[40];	/* the widest time: a 12-digit signed year */

	switch (kind) {
	case FSIMG_ENTRY_FILE: column[0] = kinds[0]; break;
	case FSIMG_ENTRY_DIRECTORY: column[0] = kinds[1]; break;
	case FSIMG_ENTRY_SYMLINK: column[0] = kinds[2]; break;
	case FSIMG_ENTRY_FIFO: column[0] = kinds[3]; break;
	default: column[0] = '?'; break;
	}
	for (unsigned i = 0; i < 9; i++) {
		if ((valid & FSIMG_META_MODE) == 0)
			column[1 + i] = '?';
		else
			column[1 + i] = (mode & (0400u >> i)) != 0 ? permissions[i] : '-';
	}
	column[10] = '\0';
	append(display, "%s ", column);
	if (kind == FSIMG_ENTRY_FILE || kind == FSIMG_ENTRY_SYMLINK)
		append(display, "%12llu ",
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_LENGTH));
	else
		append(display, "%12s ", "-");
	if ((valid & FSIMG_META_MODIFIED) != 0) {
		civil_time(fsimg_wire_get_signed64(raw + FSIMG_WIRE_ENTRY_MODIFIED +
		    FSIMG_WIRE_TIME_SECONDS), column, sizeof(column));
		append(display, "%s ", column);
	} else
		append(display, "%-19s ", "-");
}

static void
entry_start(fsimg_cli_display_t *display, const unsigned char *raw)
{
	uint64_t valid = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_VALID);
	uint64_t kind = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_KIND);

	/* Human listings mark a link with @ and a FIFO with |, as ls -F does. */
	display->entry_marker = kind == FSIMG_ENTRY_SYMLINK ? "@" :
	    kind == FSIMG_ENTRY_FIFO ? "|" : "";
	if (display->options->json) {
		append(display, "{\"schema\":1,\"type\":\"entry\",\"kind\":\"%s\",\"size_bytes\":",
		    kind_name(kind));
		uint_value(display, fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_LENGTH));
		append(display, ",\"metadata_valid\":%llu,\"attributes\":%llu,\"created\":",
		    (unsigned long long)valid,
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_ATTRIBUTES));
		timestamp(display, raw + FSIMG_WIRE_ENTRY_CREATED, (valid & FSIMG_META_CREATED) != 0);
		append(display, ",\"modified\":");
		timestamp(display, raw + FSIMG_WIRE_ENTRY_MODIFIED, (valid & FSIMG_META_MODIFIED) != 0);
		append(display, ",\"accessed\":");
		timestamp(display, raw + FSIMG_WIRE_ENTRY_ACCESSED, (valid & FSIMG_META_ACCESSED) != 0);
		append(display, ",\"changed\":");
		timestamp(display, raw + FSIMG_WIRE_ENTRY_CHANGED, (valid & FSIMG_META_CHANGED) != 0);
		owner_fields(display, raw, valid);
		append(display, ",\"path\":\"");
	} else {
		if (display->options->verbosity != 0)
			long_listing_columns(display, raw);
		append(display, "\"");
	}
	display->entry_open = true;
}

static void
verification(fsimg_cli_display_t *display, const unsigned char *raw)
{
	static const char *const keys[] = { "files", "directories", "logical_bytes",
	    "initialized_bytes_read", "allocated_clusters", "free_clusters", "bad_clusters" };

	if (display->options->json) {
		append(display, "{\"schema\":1,\"type\":\"verify\",\"complete\":true,\"validation_scope\":\"%s\"",
		    verified_scope_name(display->scope));
		for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			append(display, ",\"%s\":", keys[i]);
			uint_value(display, fsimg_wire_get64(raw + FSIMG_WIRE_VERIFY_FILES + i * 8));
		}
		append(display, "}\n");
	} else {
		path_excerpt(display, display->options->image);
		append(display, ": filesystem structure%s verified (%llu files, %llu directories)\n",
		    display->scope == FSIMG_VALIDATION_PAYLOAD ? " and initialized file data" : "",
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_VERIFY_FILES),
		    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_VERIFY_DIRECTORIES));
	}
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
		    "\"max_label_bytes\":%u,\"write_settings\":%u,\"max_file_bytes\":",
		    info->capabilities, info->entry_kinds, info->metadata_fields,
		    info->max_label_bytes, info->write_settings);
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
		append(display, "%-6s  %-4s  %-5s  %s\n", info->name,
		    (info->capabilities & FSIMG_CAP_READ) != 0 ? "yes" : "no",
		    (info->capabilities & FSIMG_CAP_WRITE) != 0 ? "yes" : "no",
		    (info->capabilities & FSIMG_CAP_VERIFY) != 0 ? "yes" : "no");
	}
}

static void
format_details(fsimg_cli_display_t *display, const fsimg_format_info_t *info)
{
	append(display, "%s:\n  Profile: ", info->name);
	prose(display, info->profile);
	append(display, "\n  Maximum file size: ");
	byte_size(display, info->max_file_bytes);
	append(display, "\n  Maximum volume size: ");
	byte_size(display, info->max_volume_bytes);
	append(display, "\n  Maximum name: %u UTF-16 units\n  Sector sizes (bytes): ", info->max_name_utf16_units);
	sizes(display, info->sector_size_mask);
	append(display, "\n  Read cluster sizes (bytes): ");
	sizes(display, info->allocation_unit_size_mask);
	append(display, "\n  Write cluster sizes (bytes): ");
	sizes(display, info->write_allocation_unit_size_mask);
	append(display, "\n  Entry kinds: ");
	entry_kinds(display, info->entry_kinds);
	append(display, "\n  Metadata: ");
	metadata_fields(display, info->metadata_fields);
	if (info->max_label_bytes != 0)
		append(display, "\n  Maximum label: %u bytes\n", info->max_label_bytes);
	else
		append(display, "\n  Maximum label: none\n");
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
		string_value(display, display->options->mode);
		append(display, ",\"validation_scope\":\"%s\",\"entries_processed\":\"%llu\","
		    "\"logical_bytes\":\"%llu\",\"raw_bytes\":\"%llu\",\"metadata_losses\":\"%llu\","
		    "\"skipped_entries\":\"%llu\",\"sandbox\":\"%s\",\"published\":%s,", scope_name(display),
		    (unsigned long long)r->entries, (unsigned long long)r->logical_bytes,
		    (unsigned long long)r->raw_bytes, (unsigned long long)r->metadata_losses,
		    (unsigned long long)r->skipped_entries,
		    confinement_name(r->confinement), display->paths->published ? "true" : "false");
		limits(display);
		append(display, ",\"progress_records\":\"%llu\",", (unsigned long long)display->progress_records);
		output_counts(display);
		append(display, "}\n");
	} else {
		append(display, "fsimg: %s: %llu entries, ", scope_name(display),
		    (unsigned long long)r->entries);
		byte_size(display, r->logical_bytes);
		append(display, "%s\n", display->paths->published ? "; image published" : "");
	}
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
	if (display->syntax && display->options->command == FSIMG_CLI_USAGE) {
		usage(display, true);
		return;
	}
	append(display, "fsimg: ");
	prose(display, error->message[0] != '\0' ? error->message : fsimg_status_string(error->status));
	if (error->message_truncated)
		append(display, " [message truncated]");
	if (error->path[0] != '\0') {
		append(display, " ");
		string_value(display, error->path);
		if (error->path_truncated)
			append(display, " [path truncated]");
	}
	if (error->os_error != 0) {
		append(display, ": ");
		prose(display, strerror(error->os_error));
	}
	if (error->resource != FSIMG_RESOURCE_NONE)
		append(display, " (%s limit %llu, observed %llu)", resource,
		    (unsigned long long)error->limit, (unsigned long long)error->observed);
	if (error->offset_valid)
		append(display, " at byte %llu", (unsigned long long)error->offset);
	append(display, "\n");
	if (error->output_state == FSIMG_OUTPUT_INCOMPLETE)
		append(display, "fsimg: output is incomplete\n");
	else if (error->output_state == FSIMG_OUTPUT_PUBLISHED) {
		append(display, "fsimg: output was published");
		if (display->options->image != NULL) {
			append(display, " as ");
			path_excerpt(display, display->options->image);
		}
		append(display, "; finalization failed; do not retry blindly\n");
	} else if (error->output_state == FSIMG_OUTPUT_UNCERTAIN)
		append(display, "fsimg: output publication or durability is uncertain; inspect the destination before retrying\n");
	if (display->paths->verification_failed)
		append(display, "fsimg: the staged %s volume failed verification; nothing was published\n",
		    display->options->request.format);
	if (error->cleanup_error != 0) {
		append(display, "fsimg: cleanup failed: ");
		prose(display, strerror(error->cleanup_error));
		append(display, "\n");
	}
	if (display->paths->retained_output[0] != '\0') {
		append(display, "fsimg: retained output: ");
		string_value(display, display->paths->retained_output);
		append(display, "\n");
	}
	if (display->paths->retained_scratch[0] != '\0') {
		append(display, "fsimg: retained scratch: ");
		string_value(display, display->paths->retained_scratch);
		append(display, "\n");
	}
	if (display->syntax)
		usage(display, display->options->overview);
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
	display->entry_marker = "";
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
		/* cW's verification worker reuses the creation command's result,
		 * so only the creation worker reports its capabilities.
		 */
		if ((options->request.flags & FSIMG_JOB_VERBOSE) != 0 &&
		    !display->verifying_staged) {
			diagnostic = true;
			if (options->json)
				append(display, "{\"schema\":1,\"type\":\"capabilities\",\"sandbox\":\"%s\"}\n",
				    confinement_name(display->result->confinement));
			else {
				append(display, "fsimg: sandbox %s\n", confinement_name(display->result->confinement));
				if (options->request.kind >= FSIMG_JOB_CREATE &&
				    !display->verifying_staged)
					append(display, "fsimg: source profile requires isolated storage to exclude special-node opens\n");
			}
		}
		break;
	case FSIMG_FRAME_INFO:
		/* cW reports the verification of its staged image, not its header. */
		if (!display->verifying_staged &&
		    (options->json || options->request.kind == FSIMG_JOB_INFO))
			volume(display, raw);
		break;
	case FSIMG_FRAME_PLAN:
		if (options->json || options->request.kind == FSIMG_JOB_PLAN ||
		    (options->request.flags & FSIMG_JOB_VERBOSE) != 0) {
			plan(display, raw);
			diagnostic = !options->json && options->request.kind == FSIMG_JOB_CREATE;
		}
		break;
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
			if (options->json)
				append(display, "\",\"complete\":true}\n");
			else
				append(display, "\"%s\n", display->entry_marker);
			display->entry_open = false;
		}
		break;
	case FSIMG_FRAME_VERIFY:
		display->scope = (fsimg_validation_scope_t)fsimg_wire_get64(raw + FSIMG_WIRE_VERIFY_SCOPE);
		if (options->json)
			verification(display, raw);
		else
			bcopy(raw, display->verification, sizeof(display->verification));
		break;
	case FSIMG_FRAME_PROGRESS:
		if (!progress_milestone(display, raw))
			break;
		diagnostic = true;
		append(display, options->json ?
		    "{\"schema\":1,\"type\":\"progress\",\"entries\":\"%llu\",\"bytes\":\"%llu\",\"work\":\"%llu\"}\n" :
		    "fsimg: processed %llu entries, %llu bytes (%llu work units)\n",
		    (unsigned long long)fsimg_wire_get64(raw),
		    (unsigned long long)fsimg_wire_get64(raw + 8),
		    (unsigned long long)fsimg_wire_get64(raw + 16));
		break;
	case FSIMG_FRAME_LOSS:
		if (!options->json && (options->request.flags & FSIMG_JOB_VERBOSE) == 0)
			break;
		diagnostic = true;
		if (options->json)
			append(display, "{\"schema\":1,\"type\":\"loss\",\"fields\":%llu,\"path_truncated\":%s,\"path_excerpt\":\"",
			    (unsigned long long)fsimg_wire_get64(raw + FSIMG_WIRE_LOSS_FIELDS),
			    fsimg_wire_get64(raw + FSIMG_WIRE_LOSS_TRUNCATED) ? "true" : "false");
		else {
			append(display, "fsimg: metadata example: ");
			metadata_fields(display, fsimg_wire_get64(raw + FSIMG_WIRE_LOSS_FIELDS));
			append(display, " could not be preserved exactly for \"");
		}
		escape_span(display, raw + FSIMG_WIRE_LOSS_PATH,
		    (size_t)fsimg_wire_get64(raw + FSIMG_WIRE_LOSS_LENGTH), options->json, false, true, NULL);
		if (options->json)
			append(display, "\"}\n");
		else
			append(display, "\"%s\n", fsimg_wire_get64(raw + FSIMG_WIRE_LOSS_TRUNCATED) != 0 ?
			    " [path truncated]" : "");
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
	bool diagnostic, first;

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
			first = display->format_cursor == 0;
			status = fsimg_format_next(&display->format_cursor, &info, error);
			if (status == FSIMG_OK) {
				if (first && !options->json)
					append(display, "Format  Read  Write  Verify\n");
				format(display, &info);
				return (finish_span(display, false, text, error));
			}
			if (status != FSIMG_DONE)
				return (status);
			display->format_cursor = 0;
			display->phase = FSIMG_CLI_OUTPUT_CAPABILITIES;
		}
		if (display->phase == FSIMG_CLI_OUTPUT_CAPABILITIES) {
			if (!options->json) {
				if ((options->request.flags & FSIMG_JOB_VERBOSE) == 0)
					return (FSIMG_DONE);
				status = fsimg_format_next(&display->format_cursor, &info, error);
				if (status == FSIMG_OK) {
					format_details(display, &info);
					return (finish_span(display, true, text, error));
				}
				if (status != FSIMG_DONE)
					return (status);
				diagnostic = true;
			}
			if (options->json)
				append(display, "{\"schema\":1,\"type\":\"capabilities\",\"sandbox_backend\":\"%s\","
				    "\"sandbox_runtime\":\"%s\",\"source_device_open_exclusion\":false,"
				    "\"posix_modes\":false,\"ownership\":false,\"links\":false,\"boot_loader\":false}\n",
				    fsimg_sandbox_backend(), strcmp(fsimg_sandbox_backend(), "none") == 0 ?
				    "unavailable" : "not-probed");
			else append(display, "Sandbox backend: %s (runtime %s)\n"
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
		if (options->command == FSIMG_CLI_PROCESS && !options->json &&
		    display->phase == FSIMG_CLI_OUTPUT_RECORDS) {
			display->phase = FSIMG_CLI_OUTPUT_SUMMARY;
			loss_notice(display);
			if (display->used != 0)
				return (finish_span(display, true, text, error));
		}
		display->phase = FSIMG_CLI_OUTPUT_DONE;
		if (options->command == FSIMG_CLI_HELP)
			help(display);
		else if (options->command == FSIMG_CLI_VERSION)
			append(display, "fsimg %s\n", PACKAGE_VERSION);
		else if (options->json)
			summary(display);
		else if (options->request.kind == FSIMG_JOB_VERIFY)
			verification(display, display->verification);
		else if ((options->request.flags & FSIMG_JOB_VERBOSE) != 0) {
			summary(display);
			diagnostic = true;
		} else
			return (FSIMG_DONE);
	}
	return (finish_span(display, diagnostic, text, error));
}
