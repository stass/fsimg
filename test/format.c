/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "format.h"
#include "host.h"
#include "test.h"

#define FAKE_COUNT	3

static fsimg_status_t empty_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t empty_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fake_probe(unsigned, size_t, fsimg_error_t *);
static fsimg_status_t fake_probe_0(const unsigned char *, size_t, uint64_t,
    const fsimg_format_variant_t **, fsimg_error_t *);
static fsimg_status_t fake_probe_1(const unsigned char *, size_t, uint64_t,
    const fsimg_format_variant_t **, fsimg_error_t *);
static fsimg_status_t fake_probe_2(const unsigned char *, size_t, uint64_t,
    const fsimg_format_variant_t **, fsimg_error_t *);
static void fake_reset(fsimg_status_t, fsimg_status_t, fsimg_status_t);
static void test_probe_contract(void);
static void test_driver_probes(void);
static void test_fat_identification_precedes_geometry(void);
static void test_short_volumes(void);
static void test_unsigned_tiny_fat_requires_complete_head(void);

/* Each fake descriptor reports a scripted claim and records what it saw. */
static fsimg_status_t fake_result[FAKE_COUNT];
static unsigned fake_calls[FAKE_COUNT];
static size_t fake_length[FAKE_COUNT];
static const fsimg_reader_ops_t fake_reader;

/* When set, the zero-filled volume starts with exFAT's identity signature. */
static bool exfat_signed;

static fsimg_status_t
empty_read(void *argument FSIMG_UNUSED, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	static const char signature[] = "\xeb\x76\x90" "EXFAT   ";

	bzero(buffer, length);
	for (size_t i = 0; exfat_signed && i < length; i++) {
		if (offset + i < sizeof(signature) - 1)
			((unsigned char *)buffer)[i] = (unsigned char)signature[offset + i];
	}
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
empty_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = *(uint64_t *)argument;
	return (FSIMG_OK);
}

static fsimg_status_t
fake_probe(unsigned index, size_t length, fsimg_error_t *error)
{
	fake_calls[index]++;
	fake_length[index] = length;
	if (fake_result[index] == FSIMG_OK)
		return (FSIMG_OK);
	return (fsimg_error_set(error, fake_result[index], 0, FSIMG_OP_READER,
	    fake_result[index] == FSIMG_EFORMAT ? "fake: no signature" :
	    "fake: claimed"));
}

static fsimg_status_t
fake_probe_0(const unsigned char *prefix FSIMG_UNUSED, size_t length,
    uint64_t volume_bytes FSIMG_UNUSED,
    const fsimg_format_variant_t **variant FSIMG_UNUSED, fsimg_error_t *error)
{
	return (fake_probe(0, length, error));
}

static fsimg_status_t
fake_probe_1(const unsigned char *prefix FSIMG_UNUSED, size_t length,
    uint64_t volume_bytes FSIMG_UNUSED,
    const fsimg_format_variant_t **variant FSIMG_UNUSED, fsimg_error_t *error)
{
	return (fake_probe(1, length, error));
}

static fsimg_status_t
fake_probe_2(const unsigned char *prefix FSIMG_UNUSED, size_t length,
    uint64_t volume_bytes FSIMG_UNUSED,
    const fsimg_format_variant_t **variant FSIMG_UNUSED, fsimg_error_t *error)
{
	return (fake_probe(2, length, error));
}

static void
fake_reset(fsimg_status_t first, fsimg_status_t second, fsimg_status_t third)
{
	fake_result[0] = first;
	fake_result[1] = second;
	fake_result[2] = third;
	bzero(fake_calls, sizeof(fake_calls));
	bzero(fake_length, sizeof(fake_length));
}

/* Detection outcomes depend only on the claims, never on registry order, and
 * a forced descriptor is the only one consulted. The fakes need no real
 * filesystem, and the third one is writer-only.
 */
static void
test_probe_contract(void)
{
	static const fsimg_writer_ops_t writer;
	/* Every format names at least one variant, as the registry's do. */
	static const fsimg_format_variant_t variant = { .info.name = "fake" };
	const fsimg_format_t fakes[FAKE_COUNT] = {
		{ .variants = &variant, .variant_count = 1, .probe_bytes = 4,
		    .probe = fake_probe_0, .reader = &fake_reader },
		{ .variants = &variant, .variant_count = 1, .probe_bytes = 64,
		    .probe = fake_probe_1, .reader = &fake_reader },
		{ .variants = &variant, .variant_count = 1, .probe_bytes = 8,
		    .probe = fake_probe_2, .writer = &writer }
	};
	const fsimg_format_t *const registry[FAKE_COUNT] = {
		&fakes[0], &fakes[1], &fakes[2]
	};
	const fsimg_format_t *claimed;
	unsigned char prefix[16];
	fsimg_error_t error;

	bzero(prefix, sizeof(prefix));
	/* No claim is an unrecognized image; every probe saw its own bound. */
	fake_reset(FSIMG_EFORMAT, FSIMG_EFORMAT, FSIMG_EFORMAT);
	CHECK(fsimg_format_claim(registry, FAKE_COUNT, NULL, prefix,
	    sizeof(prefix), 1024, &claimed, &error) == FSIMG_EFORMAT);
	CHECK(claimed == NULL && strstr(error.message, "unrecognized") != NULL);
	CHECK(fake_calls[0] == 1 && fake_calls[1] == 1 && fake_calls[2] == 1);
	CHECK(fake_length[0] == 4 && fake_length[1] == 16 && fake_length[2] == 8);

	/* Exactly one match opens that driver. */
	fake_reset(FSIMG_EFORMAT, FSIMG_OK, FSIMG_EFORMAT);
	CHECK(fsimg_format_claim(registry, FAKE_COUNT, NULL, prefix,
	    sizeof(prefix), 1024, &claimed, &error) == FSIMG_OK);
	CHECK(claimed == &fakes[1] && fake_calls[2] == 1);

	/* A single unsupported or corrupt claim is the result, with its reason. */
	fake_reset(FSIMG_EUNSUPPORTED, FSIMG_EFORMAT, FSIMG_EFORMAT);
	CHECK(fsimg_format_claim(registry, FAKE_COUNT, NULL, prefix,
	    sizeof(prefix), 1024, &claimed, &error) == FSIMG_EUNSUPPORTED);
	CHECK(claimed == NULL && strcmp(error.message, "fake: claimed") == 0);
	fake_reset(FSIMG_EFORMAT, FSIMG_ECORRUPT, FSIMG_EFORMAT);
	CHECK(fsimg_format_claim(registry, FAKE_COUNT, NULL, prefix,
	    sizeof(prefix), 1024, &claimed, &error) == FSIMG_ECORRUPT);
	CHECK(claimed == NULL);

	/* Several claims are ambiguous whatever their kind. */
	fake_reset(FSIMG_OK, FSIMG_OK, FSIMG_EFORMAT);
	CHECK(fsimg_format_claim(registry, FAKE_COUNT, NULL, prefix,
	    sizeof(prefix), 1024, &claimed, &error) == FSIMG_EFORMAT);
	CHECK(claimed == NULL && strstr(error.message, "several") != NULL);
	fake_reset(FSIMG_ECORRUPT, FSIMG_OK, FSIMG_EFORMAT);
	CHECK(fsimg_format_claim(registry, FAKE_COUNT, NULL, prefix,
	    sizeof(prefix), 1024, &claimed, &error) == FSIMG_EFORMAT);
	CHECK(claimed == NULL);

	/* A writer-only format can be recognized but not opened, detected or
	 * forced.
	 */
	fake_reset(FSIMG_EFORMAT, FSIMG_EFORMAT, FSIMG_OK);
	CHECK(fsimg_format_claim(registry, FAKE_COUNT, NULL, prefix,
	    sizeof(prefix), 1024, &claimed, &error) == FSIMG_EUNSUPPORTED);
	CHECK(claimed == NULL);
	CHECK(fsimg_format_claim(registry, FAKE_COUNT, &fakes[2], prefix,
	    sizeof(prefix), 1024, &claimed, &error) == FSIMG_EUNSUPPORTED);
	CHECK(claimed == NULL);

	/* A forced descriptor alone is probed; its absent signature is final
	 * even when another probe would match.
	 */
	fake_reset(FSIMG_OK, FSIMG_EFORMAT, FSIMG_OK);
	CHECK(fsimg_format_claim(registry, FAKE_COUNT, &fakes[1], prefix,
	    sizeof(prefix), 1024, &claimed, &error) == FSIMG_EFORMAT);
	CHECK(claimed == NULL && strcmp(error.message, "fake: no signature") == 0);
	CHECK(fake_calls[0] == 0 && fake_calls[1] == 1 && fake_calls[2] == 0);
	fake_reset(FSIMG_OK, FSIMG_OK, FSIMG_OK);
	CHECK(fsimg_format_claim(registry, FAKE_COUNT, &fakes[0], prefix, 2,
	    1024, &claimed, &error) == FSIMG_OK);
	CHECK(claimed == &fakes[0] && fake_length[0] == 2 && fake_calls[1] == 0);
	fake_reset(FSIMG_EFORMAT, FSIMG_ECORRUPT, FSIMG_EFORMAT);
	CHECK(fsimg_format_claim(registry, FAKE_COUNT, &fakes[1], prefix,
	    sizeof(prefix), 1024, &claimed, &error) == FSIMG_ECORRUPT);
	CHECK(claimed == NULL);
}

/* Signed records identify their driver before full geometry validation. */
static void
test_driver_probes(void)
{
	const fsimg_format_t *const registry[] = {
		&fsimg_fat_format, &fsimg_exfat_format
	};
	const fsimg_format_t *claimed;
	unsigned char boot[512];
	fsimg_error_t error;

	/* A 1.44 MB FAT12 floppy boot record. */
	bzero(boot, sizeof(boot));
	bcopy("\xeb\x3c\x90" "MSDOS5.0", boot, 11);
	boot[11] = 0x00, boot[12] = 0x02;	/* 512-byte sectors */
	boot[13] = 1;				/* sectors per cluster */
	boot[14] = 1;				/* reserved sectors */
	boot[16] = 2;				/* FATs */
	boot[17] = 0xe0;			/* 224 root entries */
	boot[19] = 0x40, boot[20] = 0x0b;	/* 2880 sectors */
	boot[21] = 0xf0;			/* media */
	boot[22] = 9;				/* sectors per FAT */
	boot[510] = 0x55, boot[511] = 0xaa;
	CHECK(fsimg_format_claim(registry, 2, NULL, boot, sizeof(boot),
	    2880 * 512, &claimed, &error) == FSIMG_OK);
	CHECK(claimed == &fsimg_fat_format);
	CHECK(fsimg_format_claim(registry, 2, &fsimg_exfat_format, boot,
	    sizeof(boot), 2880 * 512, &claimed, &error) == FSIMG_EFORMAT);
	CHECK(fsimg_format_claim(registry, 2, NULL, boot, sizeof(boot),
	    2879 * 512, &claimed, &error) == FSIMG_ECORRUPT);
	CHECK(fsimg_format_claim(registry, 2, NULL, boot, 511, 2880 * 512,
	    &claimed, &error) == FSIMG_EFORMAT);

	/* An NTFS boot record has a BPB but no reserved sectors. */
	bcopy("\xeb\x52\x90" "NTFS    ", boot, 11);
	boot[13] = 8;
	boot[14] = 0;
	boot[16] = 0;
	CHECK(fsimg_format_claim(registry, 2, NULL, boot, sizeof(boot),
	    UINT64_C(1) << 30, &claimed, &error) == FSIMG_EFORMAT);
	CHECK(strstr(error.message, "unrecognized") != NULL);

	/* exFAT: jump, name and the zeroed legacy BPB. */
	bzero(boot, sizeof(boot));
	bcopy("\xeb\x76\x90" "EXFAT   ", boot, 11);
	boot[510] = 0x55, boot[511] = 0xaa;
	CHECK(fsimg_format_claim(registry, 2, NULL, boot, sizeof(boot),
	    UINT64_C(1) << 30, &claimed, &error) == FSIMG_OK);
	CHECK(claimed == &fsimg_exfat_format);
	CHECK(fsimg_format_claim(registry, 2, &fsimg_fat_format, boot,
	    sizeof(boot), UINT64_C(1) << 30, &claimed, &error) == FSIMG_EFORMAT);
	boot[40] = 1;
	CHECK(fsimg_format_claim(registry, 2, NULL, boot, sizeof(boot),
	    UINT64_C(1) << 30, &claimed, &error) == FSIMG_EFORMAT);
	CHECK(fsimg_format_claim(registry, 2, NULL, boot, 63,
	    UINT64_C(1) << 30, &claimed, &error) == FSIMG_EFORMAT);
}

/* Unsigned FAT identification precedes geometry validation. */
static void
test_fat_identification_precedes_geometry(void)
{
	const fsimg_format_t *const registry[] = {
		&fsimg_fat_format, &fsimg_exfat_format
	};
	const fsimg_format_t *claimed;
	const fsimg_format_variant_t *variant;
	unsigned char prefix[8704];
	fsimg_error_t error;

	/* A 1.44 MB FAT12 BPB with an Atari jump and no PC signature. */
	bzero(prefix, sizeof(prefix));
	prefix[0] = 0x60, prefix[1] = 0x38;
	prefix[11] = 0x00, prefix[12] = 0x02;
	prefix[13] = 1;
	prefix[14] = 1;
	prefix[16] = 2;
	prefix[17] = 0xe0;
	prefix[19] = 0x40, prefix[20] = 0x0b;
	prefix[21] = 0xf0;
	prefix[22] = 9;
	prefix[512] = 0xf0, prefix[513] = 0xff, prefix[514] = 0xff;

	/* The media echo identifies FAT only when all its bytes were seen. */
	CHECK(fsimg_format_claim(registry, 2, NULL, prefix, sizeof(prefix),
	    2880 * 512, &claimed, &error) == FSIMG_OK);
	CHECK(claimed == &fsimg_fat_format);
	CHECK(fsimg_format_claim(registry, 2, NULL, prefix, 512,
	    2880 * 512, &claimed, &error) == FSIMG_EFORMAT);
	prefix[512] = 0x00;
	CHECK(fsimg_format_claim(registry, 2, NULL, prefix, sizeof(prefix),
	    2880 * 512, &claimed, &error) == FSIMG_EFORMAT);
	prefix[512] = 0xf0;

	/* An identified unsigned volume has the same geometry classification. */
	CHECK(fsimg_format_claim(registry, 2, NULL, prefix, sizeof(prefix),
	    2879 * 512, &claimed, &error) == FSIMG_ECORRUPT);

	/* Three tables are unsupported after identification, even unsigned. */
	prefix[16] = 3;
	CHECK(fsimg_format_claim(registry, 2, NULL, prefix, sizeof(prefix),
	    2880 * 512, &claimed, &error) == FSIMG_EUNSUPPORTED);
	prefix[16] = 2;

	/* FAT12 FAT[1] = 0xFF8 keeps FAT[0] intact but breaks mtools' head. */
	prefix[513] = 0x8f;
	CHECK(fsimg_format_claim(registry, 2, NULL, prefix, sizeof(prefix),
	    2880 * 512, &claimed, &error) == FSIMG_EFORMAT);
	CHECK(fsimg_fat_format.probe(prefix, sizeof(prefix), 2880 * 512,
	    &variant, &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "unrecognized volume boot record") == 0);
	prefix[513] = 0xff;

	/* The last sector in the 8 KiB prefix can supply the media echo. */
	bzero(prefix + 512, 3);
	prefix[14] = 15;
	prefix[7680] = 0xf0, prefix[7681] = 0xff, prefix[7682] = 0xff;
	CHECK(fsimg_format_claim(registry, 2, NULL, prefix, sizeof(prefix),
	    2880 * 512, &claimed, &error) == FSIMG_OK);
	CHECK(claimed == &fsimg_fat_format);

	/* Even a larger supplied buffer cannot extend the probe's prefix. */
	bzero(prefix + 7680, 3);
	prefix[14] = 16;
	prefix[8192] = 0xf0, prefix[8193] = 0xff, prefix[8194] = 0xff;
	CHECK(fsimg_format_claim(registry, 2, NULL, prefix, sizeof(prefix),
	    2880 * 512, &claimed, &error) == FSIMG_EFORMAT);

	/* A PC signature claims FAT without checking the table's media echo. */
	prefix[14] = 1;
	bcopy("\xeb\x3c\x90", prefix, 3);
	prefix[510] = 0x55, prefix[511] = 0xaa;
	CHECK(fsimg_format_claim(registry, 2, NULL, prefix, sizeof(prefix),
	    2880 * 512, &claimed, &error) == FSIMG_OK);
	CHECK(claimed == &fsimg_fat_format);
	CHECK(fsimg_format_claim(registry, 2, NULL, prefix, sizeof(prefix),
	    2879 * 512, &claimed, &error) == FSIMG_ECORRUPT);
}

/* A tiny unsigned volume needs all three identifying FAT head bytes. */
static void
test_unsigned_tiny_fat_requires_complete_head(void)
{
	const fsimg_format_t *const registry[] = { &fsimg_fat_format };
	const fsimg_format_t *claimed;
	const fsimg_format_variant_t *variant;
	unsigned char volume[4096] = { 0 };
	fsimg_error_t error;

	volume[12] = 2; /* 512-byte sectors */
	volume[13] = 1;
	volume[14] = 1;
	volume[16] = 1;
	volume[17] = 16;
	volume[19] = 8;
	volume[21] = 0xf8;
	volume[22] = 1;
	volume[512] = 0xf8, volume[513] = 0xff, volume[514] = 0xff;
	CHECK(fsimg_format_claim(registry, 1, NULL, volume, sizeof(volume),
	    sizeof(volume), &claimed, &error) == FSIMG_OK);
	CHECK(claimed == &fsimg_fat_format);
	CHECK(fsimg_format_claim(registry, 1, NULL, volume, 514,
	    sizeof(volume), &claimed, &error) == FSIMG_EFORMAT);
	CHECK(fsimg_fat_format.probe(volume, 514, sizeof(volume),
	    &variant, &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "unrecognized volume boot record") == 0);
}

/* Volumes too short for any signature are unrecognized, forced or not. A
 * volume that carries exFAT's signature but is too short for exFAT is a
 * corrupt exFAT claim, and the driver never reads past the volume.
 */
static void
test_short_volumes(void)
{
	fsimg_ctx_t *ctx;
	fsimg_reader_t *reader;
	fsimg_io_t *io;
	fsimg_io_ops_t ops;
	fsimg_read_options_t options;
	fsimg_error_t error;
	uint64_t length;
	fsimg_status_t status, expected;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = empty_read;
	ops.get_size = empty_size;
	for (unsigned pass = 0; pass < 2; pass++) {
		exfat_signed = pass == 1;
		for (length = 0; length < 8192; length += length < 512 ? 1 : 1536) {
			CHECK(fsimg_io_from_callbacks(ctx, &ops, &length, &io, NULL) == FSIMG_OK);
			for (unsigned forced = 0; forced < 3; forced++) {
				fsimg_read_options_init(&options);
				options.format = forced == 2 ? "exfat" : forced == 1 ? "fat16" : NULL;
				expected = exfat_signed && length >= 64 && forced != 1 ?
				    FSIMG_ECORRUPT : FSIMG_EFORMAT;
				status = fsimg_reader_open(ctx, io, &options, &reader, &error);
				if (status != expected)
					fprintf(stderr, "short volume length=%llu signed=%u forced=%u status=%d: %s\n",
					    (unsigned long long)length, pass, forced, status, error.message);
				CHECK(status == expected && reader == NULL);
			}
			fsimg_io_free(io);
		}
	}
	exfat_signed = false;
	fsimg_ctx_free(ctx);
}

int
main(void)
{
	test_probe_contract();
	test_driver_probes();
	test_fat_identification_precedes_geometry();
	test_unsigned_tiny_fat_requires_complete_head();
	test_short_volumes();
	return (0);
}
