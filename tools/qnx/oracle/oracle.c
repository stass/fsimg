/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 *
 * Black-box codec oracle for fsimg's IFS work. It calls the public APIs of
 * GPL liblzo2 and libucl, so its binary is built out of the tree by
 * build.sh, never by the fsimg build, and is never distributed or linked
 * into fsimg. This source holds no codec internals.
 *
 *   oracle lzo IN OUT MAXOUT      one unframed LZO1X stream
 *   oracle ucl IN OUT MAXOUT      one unframed NRV2B (8-bit) stream
 *   oracle boot IN OFF OUT        decode a QNX boot image's body: startup
 *                                 header at OFF, framed LZO or UCL chunks
 *   oracle chunks IN OFF DIR      like boot, but also write each stored
 *                                 chunk and its decode to DIR/NNNNNN.{c,d}
 *   oracle lzoenc IN OUT          LZO1X-1 encode one stream
 *   oracle lzo999enc IN OUT       LZO1X-999 encode one stream
 *   oracle uclenc IN OUT LEVEL    NRV2B (8-bit) encode one stream
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <lzo/lzo1x.h>
#include <ucl/ucl.h>

static unsigned char *
slurp(const char *p, size_t *n)
{
	FILE *f = fopen(p, "rb");
	if (f == NULL) { perror(p); exit(2); }
	fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
	unsigned char *b = malloc(*n + 1);
	if (fread(b, 1, *n, f) != *n) { perror(p); exit(2); }
	fclose(f);
	return b;
}

static void
spit(const char *p, const void *b, size_t n)
{
	FILE *f = fopen(p, "wb");
	if (f == NULL || fwrite(b, 1, n, f) != n || fclose(f) != 0) {
		perror(p); exit(2);
	}
}

static uint32_t
le32(const unsigned char *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int
boot(int argc, char **argv, int split)
{
	size_t n;
	unsigned char *d = slurp(argv[2], &n);
	size_t off = strtoul(argv[3], NULL, 0);
	if (off + 256 > n || le32(d + off) != 0x00ff7eeb) {
		fprintf(stderr, "no startup\n");
		return 1;
	}
	unsigned f1 = d[off + 6];
	uint32_t ssz = le32(d + off + 0x20), isz = le32(d + off + 0x2c);
	size_t p = off + ssz, cap = (size_t)isz + 65536 * 4;
	unsigned char *out = malloc(cap);
	size_t ol = 0, chunks = 0;
	char name[4096];
	(void)argc;
	for (;;) {
		if (p + 2 > n) { fprintf(stderr, "truncated\n"); return 1; }
		unsigned len = d[p] << 8 | d[p + 1];
		p += 2;
		if (len == 0)
			break;
		if (p + len > n) { fprintf(stderr, "truncated chunk\n"); return 1; }
		int r;
		size_t before = ol;
		if ((f1 & 0x1c) == 0x08) {
			lzo_uint cl = cap - ol;
			r = lzo1x_decompress_safe(d + p, len, out + ol, &cl, NULL);
			ol += cl;
		} else if ((f1 & 0x1c) == 0x0c) {
			ucl_uint cl = cap - ol;
			r = ucl_nrv2b_decompress_safe_8(d + p, len, out + ol, &cl,
			    NULL);
			ol += cl;
		} else {
			fprintf(stderr, "codec %#x\n", f1 & 0x1c);
			return 1;
		}
		if (r != 0) {
			fprintf(stderr, "chunk %zu error %d\n", chunks, r);
			return 1;
		}
		if (split) {
			snprintf(name, sizeof(name), "%s/%06zu.c", argv[4], chunks);
			spit(name, d + p, len);
			snprintf(name, sizeof(name), "%s/%06zu.d", argv[4], chunks);
			spit(name, out + before, ol - before);
		}
		p += len;
		chunks++;
	}
	if (!split)
		spit(argv[4], out, ol);
	printf("chunks %zu decoded %zu imagefs_size %u end %zu\n", chunks, ol,
	    isz, p);
	return 0;
}

int
main(int argc, char **argv)
{
	size_t n;
	if (lzo_init() != LZO_E_OK || ucl_init() != UCL_E_OK)
		return 2;
	if (argc == 5 && (!strcmp(argv[1], "lzo") || !strcmp(argv[1], "ucl"))) {
		unsigned char *in = slurp(argv[2], &n);
		size_t max = strtoul(argv[4], NULL, 0);
		unsigned char *out = malloc(max + 1);
		int r;
		if (!strcmp(argv[1], "lzo")) {
			lzo_uint ol = max;
			r = lzo1x_decompress_safe(in, n, out, &ol, NULL);
			max = ol;
		} else {
			ucl_uint ol = max;
			r = ucl_nrv2b_decompress_safe_8(in, n, out, &ol, NULL);
			max = ol;
		}
		if (r != 0) {
			fprintf(stderr, "decode error %d\n", r);
			return 1;
		}
		spit(argv[3], out, max);
		printf("%zu\n", max);
		return 0;
	}
	if (argc == 5 && !strcmp(argv[1], "boot"))
		return boot(argc, argv, 0);
	if (argc == 5 && !strcmp(argv[1], "chunks"))
		return boot(argc, argv, 1);
	if (argc == 4 && (!strcmp(argv[1], "lzoenc") ||
	    !strcmp(argv[1], "lzo999enc"))) {
		unsigned char *in = slurp(argv[2], &n);
		unsigned char *out = malloc(n + n / 16 + 64 + 3);
		void *wrk = malloc(LZO1X_999_MEM_COMPRESS);
		lzo_uint ol;
		int r = !strcmp(argv[1], "lzoenc") ?
		    lzo1x_1_compress(in, n, out, &ol, wrk) :
		    lzo1x_999_compress(in, n, out, &ol, wrk);
		if (r != 0)
			return 1;
		spit(argv[3], out, ol);
		printf("%zu\n", (size_t)ol);
		return 0;
	}
	if (argc == 5 && !strcmp(argv[1], "uclenc")) {
		unsigned char *in = slurp(argv[2], &n);
		unsigned char *out = malloc(n + n / 8 + 256);
		ucl_uint ol;
		int r = ucl_nrv2b_99_compress(in, n, out, &ol, NULL,
		    atoi(argv[4]), NULL, NULL);
		if (r != 0)
			return 1;
		spit(argv[3], out, ol);
		printf("%zu\n", (size_t)ol);
		return 0;
	}
	fprintf(stderr, "usage: see source\n");
	return 2;
}
