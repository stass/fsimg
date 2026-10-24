/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fsimg-endian.h"
#include "test.h"

/*
 * The scalar codecs against byte arrays written out by hand or built one
 * byte at a time from a bit's position. No expected array comes from a
 * store, so a load and a store that were wrong in matching ways would
 * still fail. The program is built with both signed and unsigned plain
 * char; the high bytes 0x80, 0xf1 and 0xff would expose a sign-extended
 * byte in either build.
 */

typedef enum byte_order {
	ORDER_LE,
	ORDER_BE
} byte_order_t;

static uint64_t load(byte_order_t, unsigned, const unsigned char *);
static void store(byte_order_t, unsigned, unsigned char *, uint64_t);
static void check_scalar(byte_order_t, unsigned, uint64_t,
    const unsigned char *);
static void test_known_vectors(void);
static void test_extremes(void);
static void test_single_bits(void);
static void test_unaligned_extent(void);
static void test_allocation_end(void);

/* Dispatch by width so one table drives all twelve functions. */
static uint64_t
load(byte_order_t order, unsigned width, const unsigned char *p)
{
	switch (width) {
	case 2:
		return (order == ORDER_LE ? fsimg_load_le16(p) :
		    fsimg_load_be16(p));
	case 4:
		return (order == ORDER_LE ? fsimg_load_le32(p) :
		    fsimg_load_be32(p));
	default:
		CHECK(width == 8);
		return (order == ORDER_LE ? fsimg_load_le64(p) :
		    fsimg_load_be64(p));
	}
}

static void
store(byte_order_t order, unsigned width, unsigned char *p, uint64_t value)
{
	switch (width) {
	case 2:
		CHECK(value <= UINT16_MAX);
		if (order == ORDER_LE)
			fsimg_store_le16(p, (uint16_t)value);
		else
			fsimg_store_be16(p, (uint16_t)value);
		break;
	case 4:
		CHECK(value <= UINT32_MAX);
		if (order == ORDER_LE)
			fsimg_store_le32(p, (uint32_t)value);
		else
			fsimg_store_be32(p, (uint32_t)value);
		break;
	default:
		CHECK(width == 8);
		if (order == ORDER_LE)
			fsimg_store_le64(p, value);
		else
			fsimg_store_be64(p, value);
		break;
	}
}

/* bytes is the value's expected encoding: it must decode to value, and a
 * store of value must produce exactly it.
 */
static void
check_scalar(byte_order_t order, unsigned width, uint64_t value,
    const unsigned char *bytes)
{
	unsigned char stored[8];

	CHECK(load(order, width, bytes) == value);
	memset(stored, 0x5a, sizeof(stored));
	store(order, width, stored, value);
	CHECK(memcmp(stored, bytes, width) == 0);
}

static void
test_known_vectors(void)
{
	static const unsigned char le16[] = { 0xf1, 0x80 };
	static const unsigned char be16[] = { 0x80, 0xf1 };
	static const unsigned char le32[] = { 0xa5, 0x23, 0xf1, 0x80 };
	static const unsigned char be32[] = { 0x80, 0xf1, 0x23, 0xa5 };
	static const unsigned char le64[] = {
		0xe9, 0xd8, 0xc7, 0xb6, 0xa5, 0x23, 0xf1, 0x80
	};
	static const unsigned char be64[] = {
		0x80, 0xf1, 0x23, 0xa5, 0xb6, 0xc7, 0xd8, 0xe9
	};

	check_scalar(ORDER_LE, 2, 0x80f1, le16);
	check_scalar(ORDER_BE, 2, 0x80f1, be16);
	check_scalar(ORDER_LE, 4, 0x80f123a5, le32);
	check_scalar(ORDER_BE, 4, 0x80f123a5, be32);
	check_scalar(ORDER_LE, 8, UINT64_C(0x80f123a5b6c7d8e9), le64);
	check_scalar(ORDER_BE, 8, UINT64_C(0x80f123a5b6c7d8e9), be64);
}

static void
test_extremes(void)
{
	static const unsigned char zero[8];
	static const unsigned char ones[8] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};

	for (byte_order_t order = ORDER_LE; order <= ORDER_BE; order++) {
		check_scalar(order, 2, 0, zero);
		check_scalar(order, 4, 0, zero);
		check_scalar(order, 8, 0, zero);
		check_scalar(order, 2, UINT16_MAX, ones);
		check_scalar(order, 4, UINT32_MAX, ones);
		check_scalar(order, 8, UINT64_MAX, ones);
	}
}

/*
 * Bit b of a value lives in byte b / 8 of a little-endian encoding and in
 * byte width - 1 - b / 8 of a big-endian one, as bit b % 8 of that byte.
 */
static void
test_single_bits(void)
{
	static const unsigned widths[] = { 2, 4, 8 };
	unsigned char bytes[8];

	for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
		unsigned width = widths[w];

		for (unsigned bit = 0; bit < width * 8; bit++) {
			unsigned le_byte = bit / 8;
			unsigned be_byte = width - 1 - bit / 8;
			unsigned char mask = (unsigned char)(1u << (bit % 8));
			uint64_t value = UINT64_C(1) << bit;

			memset(bytes, 0, sizeof(bytes));
			bytes[le_byte] = mask;
			check_scalar(ORDER_LE, width, value, bytes);

			memset(bytes, 0, sizeof(bytes));
			bytes[be_byte] = mask;
			check_scalar(ORDER_BE, width, value, bytes);
		}
	}
}

/*
 * A scalar at any of the first eight offsets of a buffer decodes in place,
 * and a store there writes its own bytes and leaves the canary bytes on
 * either side alone.
 */
static void
test_unaligned_extent(void)
{
	static const unsigned widths[] = { 2, 4, 8 };
	static const unsigned char be64[] = {
		0x80, 0xf1, 0x23, 0xa5, 0xb6, 0xc7, 0xd8, 0xe9
	};
	unsigned char buffer[24], expected[24];

	for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
		unsigned width = widths[w];
		/* The value's big-endian bytes are the first width of be64. */
		uint64_t value = UINT64_C(0x80f123a5b6c7d8e9) >> (64 - 8 * width);

		for (unsigned offset = 0; offset < 8; offset++) {
			for (byte_order_t order = ORDER_LE; order <= ORDER_BE;
			    order++) {
				memset(expected, 0xc3, sizeof(expected));
				for (unsigned i = 0; i < width; i++)
					expected[offset + i] = order == ORDER_BE ?
					    be64[i] : be64[width - 1 - i];
				memset(buffer, 0xc3, sizeof(buffer));
				store(order, width, buffer + offset, value);
				CHECK(memcmp(buffer, expected, sizeof(buffer)) == 0);
				CHECK(load(order, width, expected + offset) == value);
			}
		}
	}
}

/*
 * A scalar ending at the last byte of its allocation: under
 * AddressSanitizer an access past its width would fault here.
 */
static void
test_allocation_end(void)
{
	static const unsigned widths[] = { 2, 4, 8 };

	for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
		unsigned width = widths[w];

		for (byte_order_t order = ORDER_LE; order <= ORDER_BE; order++) {
			unsigned char *bytes;

			bytes = malloc(width);
			CHECK(bytes != NULL);
			memset(bytes, 0xff, width);
			CHECK(load(order, width, bytes) ==
			    UINT64_MAX >> (64 - 8 * width));
			store(order, width, bytes, 1);
			CHECK(load(order, width, bytes) == 1);
			CHECK(bytes[order == ORDER_LE ? 0 : width - 1] == 1);
			free(bytes);
		}
	}
}

int
main(void)
{
	test_known_vectors();
	test_extremes();
	test_single_bits();
	test_unaligned_extent();
	test_allocation_end();
	return (0);
}
