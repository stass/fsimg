/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdint.h>

/* A freestanding x86-64 UEFI application: no firmware structures, runtime
 * library, or imported code. The PE entry runs with firmware I/O authority.
 * It initializes COM1 and reports execution independently of console routing.
 */
typedef uint64_t efi_status_t;

static void
out_byte(uint16_t port, uint8_t value)
{
	__asm__ volatile ("outb %0, %w1" : : "a" (value), "Nd" (port));
}

static uint8_t
in_byte(uint16_t port)
{
	uint8_t value;

	__asm__ volatile ("inb %w1, %0" : "=a" (value) : "Nd" (port));
	return (value);
}

efi_status_t
efi_main(void *image, void *system)
{
	static const char marker[] = "FSIMG_EFI_OK\n";

	(void)image;
	(void)system;
	out_byte(0x3fb, 0x80);
	out_byte(0x3f8, 1);
	out_byte(0x3f9, 0);
	out_byte(0x3fb, 3);
	for (unsigned i = 0; marker[i] != '\0'; i++) {
		while ((in_byte(0x3fd) & 0x20) == 0)
			;
		out_byte(0x3f8, (uint8_t)marker[i]);
	}
	/* The test VM supplies isa-debug-exit at this port. */
	out_byte(0xf4, 0x10);
	return (0);
}
