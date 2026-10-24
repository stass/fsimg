/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_SETTINGS_H
#define FSIMG_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fsimg.h"

/*
 * The settings catalogue: one immutable row per typed setting the command
 * accepts through -o and the library holds in its public option records.
 * A row states the setting's name, where its value lives (the record, the
 * member's offset and size, and its exact C kind), the API value that
 * means "default", the bool member that marks it present when it has one,
 * its command-line syntax and the operations it applies to. The library's
 * option checks, the layer capability records and the command's -o
 * conversion all read these rows, so the facts cannot disagree.
 *
 * Rows come in slices: the filesystem's, and one per layer, which the
 * layer's registry record references. A slice's order is its presentation
 * order: a layer's file settings precede its scalar settings. Rows are
 * trusted build data; the catalogue tests check them, and the operations
 * here do not revalidate them.
 *
 * The catalogue describes supplied values, not resolved ones: a default
 * means "let the owner choose", and what each value means, how values
 * relate and what a profile requires stay with the format, layer or
 * container that owns them. Nothing here reads image bytes or opens a
 * host path.
 */

/*
 * The file settings of fsimg_layer_inputs_t
 * (specs/design-layers.md#file-settings-and-sections), each the identity
 * of one catalogue row of record FSIMG_RECORD_INPUTS. The worker protocol
 * passes their descriptors in this order.
 */
typedef enum fsimg_file_setting {
	FSIMG_FILE_IFSBOOT_STARTUP = 0,
	FSIMG_FILE_IFSBOOT_PREFIX = 1
} fsimg_file_setting_t;

#define FSIMG_FILE_SETTINGS	2u

/* The public option record holding a row's value. */
typedef enum fsimg_setting_record {
	FSIMG_RECORD_NONE,	/* No storage: a command-line acknowledgement */
	FSIMG_RECORD_WRITE,	/* fsimg_write_options_t */
	FSIMG_RECORD_LAYERS,	/* fsimg_layer_options_t */
	FSIMG_RECORD_INPUTS	/* fsimg_layer_inputs_t */
} fsimg_setting_record_t;

/* The exact C type of a row's member. */
typedef enum fsimg_setting_kind {
	FSIMG_KIND_NONE,		/* No member */
	FSIMG_KIND_U32,			/* uint32_t */
	FSIMG_KIND_U64,			/* uint64_t */
	FSIMG_KIND_BOOL,		/* bool */
	FSIMG_KIND_BYTE_ORDER,		/* fsimg_byte_order_t */
	FSIMG_KIND_DMG_COMPRESSION,	/* fsimg_dmg_compression_t */
	FSIMG_KIND_IFSBOOT_COMPRESSION,	/* fsimg_ifsboot_compression_t */
	FSIMG_KIND_TEXT,		/* char[size], NUL-terminated */
	FSIMG_KIND_ID16,		/* unsigned char[16] */
	FSIMG_KIND_TEXT_POINTER,	/* const char *, borrowed; NULL absent */
	FSIMG_KIND_IO			/* fsimg_io_t *, borrowed; NULL absent */
} fsimg_setting_kind_t;

/*
 * A typed value, read through the member its row's kind names. A TEXT
 * value is a pointer to the text: a loaded one points into the record,
 * and a stored one is copied from it into the member.
 */
typedef union fsimg_setting_value {
	uint64_t	 u64;
	uint32_t	 u32;
	bool		 flag;
	fsimg_byte_order_t byte_order;
	fsimg_dmg_compression_t dmg_compression;
	fsimg_ifsboot_compression_t ifsboot_compression;
	unsigned char	 id[16];
	const char	*text;
	fsimg_io_t	*io;
} fsimg_setting_value_t;

/* The command-line lexical class of a row's value. */
typedef enum fsimg_setting_syntax {
	FSIMG_SYNTAX_BYTES,	/* An unsigned quantity with an optional binary unit */
	FSIMG_SYNTAX_COUNT,	/* An unsigned decimal count */
	FSIMG_SYNTAX_HEX32,	/* 0x or 0X and one to eight hexadecimal digits */
	FSIMG_SYNTAX_BOOL,	/* Exactly yes or no */
	FSIMG_SYNTAX_CHOICE,	/* One of the row's choice spellings */
	FSIMG_SYNTAX_UUID,	/* RFC 4122 text, stored in textual byte order */
	FSIMG_SYNTAX_TEXT,	/* Text within the row's text capacity */
	FSIMG_SYNTAX_PATH	/* A host path the command opens itself */
} fsimg_setting_syntax_t;

/* The operations a row applies to. */
#define FSIMG_SCOPE_READ	0x01u
#define FSIMG_SCOPE_WRITE	0x02u
/* It describes the one partition a writer or wrap creates; compose takes
 * such settings from each part instead.
 */
#define FSIMG_SCOPE_PARTITION	0x04u

/* A row without a presence flag. */
#define FSIMG_NO_PRESENCE	SIZE_MAX

/* One named value of a row: a CHOICE row's spelling and the value it
 * stores, or a number a numeric row restricts itself to. A list ends with
 * a NULL spelling.
 */
typedef struct fsimg_setting_choice {
	const char	*spelling;
	fsimg_setting_value_t value;
} fsimg_setting_choice_t;

/*
 * One setting.
 *
 * owner is the registry name of the layer whose slice holds the row, NULL
 * in the filesystem's slice; with key it forms the canonical key
 * ("gpt:sector", "reserve"). record, offset, size and kind locate and type
 * the member. initial is the default the public initializers store and
 * the default tests compare with. presence is the offset of the bool
 * member, in the same record, that marks the value supplied: while it is
 * false the value must hold initial, and while it is true the value
 * counts as supplied whatever it is.
 *
 * syntax is the command-line class. choices, NULL for none, are a CHOICE
 * row's spellings, or the only numbers a numeric row accepts. max bounds a
 * number below its kind's range, and nonzero refuses zero (or the nil
 * UUID) on the command line, where a zero would read as the default the
 * key cannot request. text_bytes is the command's capacity for a text,
 * its terminator included, when it is narrower than the member's or the
 * member is a pointer; zero uses size.
 *
 * scope holds FSIMG_SCOPE_* bits. support is the FSIMG_SETTING_* bit a
 * format's capability record must carry for the row to hold anything but
 * its default, zero when every format accepts it. file is the identity of
 * an FSIMG_RECORD_INPUTS row.
 */
typedef struct fsimg_setting {
	const char	*owner;
	const char	*key;
	const fsimg_setting_choice_t *choices;
	fsimg_setting_value_t initial;
	size_t		 offset;
	size_t		 size;
	size_t		 presence;
	size_t		 text_bytes;
	uint64_t	 max;
	fsimg_setting_record_t record;
	fsimg_setting_kind_t kind;
	fsimg_setting_syntax_t syntax;
	fsimg_file_setting_t file;
	unsigned	 scope;
	uint32_t	 support;
	bool		 nonzero;
} fsimg_setting_t;

/* A contiguous run of rows with one owner. */
typedef struct fsimg_setting_slice {
	const fsimg_setting_t *rows;
	size_t		 count;
} fsimg_setting_slice_t;

/* The records a check or load reads; a NULL record holds every default. */
typedef struct fsimg_setting_records {
	const fsimg_write_options_t *write;
	const fsimg_layer_options_t *layers;
	const fsimg_layer_inputs_t *inputs;
} fsimg_setting_records_t;

/* The records a store writes; a row's record must be present. */
typedef struct fsimg_setting_targets {
	fsimg_write_options_t *write;
	fsimg_layer_options_t *layers;
	fsimg_layer_inputs_t *inputs;
} fsimg_setting_targets_t;

extern const fsimg_setting_slice_t fsimg_filesystem_settings;
extern const fsimg_setting_slice_t fsimg_gpt_settings;
extern const fsimg_setting_slice_t fsimg_mbr_settings;
extern const fsimg_setting_slice_t fsimg_dmg_settings;
extern const fsimg_setting_slice_t fsimg_vhd_settings;
extern const fsimg_setting_slice_t fsimg_simg_settings;
extern const fsimg_setting_slice_t fsimg_ifsboot_settings;
extern const fsimg_setting_slice_t fsimg_gzip_settings;

/* The row of a slice whose key is the length bytes at key, which need not
 * be terminated; NULL when the slice has none. Case-sensitive.
 */
const fsimg_setting_t *fsimg_setting_find(const fsimg_setting_slice_t *,
    const char *, size_t);
/* The canonical key, "owner:key" or "key", truncated to fit capacity. */
void fsimg_setting_key(const fsimg_setting_t *, char *, size_t);
/* The choice spellings joined for a message: "zlib or none". */
void fsimg_setting_choices_text(const fsimg_setting_t *, char *, size_t);
/* The choice holding value, or NULL; compared as the row's kind. */
const fsimg_setting_choice_t *fsimg_setting_choice_of(const fsimg_setting_t *,
    const fsimg_setting_value_t *);

/* A row's value in records, or initial for an absent record. */
void fsimg_setting_load(const fsimg_setting_t *,
    const fsimg_setting_records_t *, fsimg_setting_value_t *);
/*
 * Store a value, and set the presence flag of a row with one. A TEXT value
 * must fit the member with its terminator; the member's tail is zeroed.
 */
void fsimg_setting_store(const fsimg_setting_t *,
    const fsimg_setting_targets_t *, const fsimg_setting_value_t *);
/* Whether a row is absent: its presence flag clear, or without one, its
 * value equal to initial.
 */
bool fsimg_setting_is_default(const fsimg_setting_t *,
    const fsimg_setting_records_t *);
/*
 * The structural check of one row: a TEXT member terminated within its
 * size, an enum value one of the row's choices, and a value whose presence
 * flag is clear holding its default. A failure is FSIMG_EINVAL naming the
 * canonical key in error.path.
 */
fsimg_status_t fsimg_setting_check(const fsimg_setting_t *,
    const fsimg_setting_records_t *, fsimg_error_t *);

/* Store every default of a slice's rows of one record. */
void fsimg_settings_init(const fsimg_setting_slice_t *,
    const fsimg_setting_targets_t *, fsimg_setting_record_t);
/*
 * The filesystem settings of write options: each structurally valid, and
 * each one not at its default applicable to the writer whose capability
 * record is given: a row without a support bit always, another when the
 * record's write_settings carries its bit. Without a record (NULL, an
 * operation with no filesystem writer) every row must hold its default.
 * A failure is FSIMG_EINVAL naming the key, in catalogue order.
 */
fsimg_status_t fsimg_write_settings_check(const fsimg_write_options_t *,
    const fsimg_format_info_t *, fsimg_error_t *);

#endif
