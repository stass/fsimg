#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
"""ifscorrupt.py IN OUT KIND: write a copy of the little-endian QNX boot
image IN with one defect, a boot gate's negative control. The startup
header is found at offset 0 or behind the 1088-byte x86 BIOS loader.

  signature  the signature's first byte flipped (0xeb to 0x14)
  checksum   the startup checksum word, the startup section's last word,
             plus one
  body       the third byte of the stored body flipped: inside a raw
             ImageFS's header, or inside a framed body's first chunk
"""
import struct
import sys

SIGNATURE = 0x00FF7EEB
PREFIX = 1088


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    data = bytearray(open(sys.argv[1], 'rb').read())
    base = next((b for b in (0, PREFIX)
                 if len(data) >= b + 256 and
                 struct.unpack_from('<I', data, b)[0] == SIGNATURE), None)
    if base is None:
        sys.exit('ifscorrupt.py: no startup header at 0 or 1088')
    startup = struct.unpack_from('<I', data, base + 0x20)[0]
    if startup < 260 or startup % 4 != 0 or base + startup + 4 > len(data):
        sys.exit('ifscorrupt.py: startup_size %d does not fit the image' % startup)
    kind = sys.argv[3]
    if kind == 'signature':
        at = base
        data[at] ^= 0xff
    elif kind == 'checksum':
        at = base + startup - 4
        word = struct.unpack_from('<I', data, at)[0]
        struct.pack_into('<I', data, at, (word + 1) & 0xffffffff)
    elif kind == 'body':
        at = base + startup + 2
        data[at] ^= 0xff
    else:
        sys.exit('ifscorrupt.py: unknown kind %s' % kind)
    with open(sys.argv[2], 'xb') as out:
        out.write(data)
    print('%s: %s at %d' % (sys.argv[2], kind, at))


if __name__ == '__main__':
    main()
