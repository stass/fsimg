#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
"""Convert QEMU's binary PPM (P6, maxval 255) screendump to PNG.

Usage: ppm2png.py IN.ppm OUT.png

Exits nonzero on a truncated or malformed dump, which shot.sh relies on to
retry while QEMU is still writing it.
"""
import struct
import sys
import zlib

data = open(sys.argv[1], 'rb').read()
# Header: magic, width, height, maxval, separated by whitespace, then exactly
# one whitespace byte before the pixels (which may themselves be whitespace).
fields = []
pos = 0
while len(fields) < 4:
    while data[pos:pos + 1].isspace():
        pos += 1
    start = pos
    while pos < len(data) and not data[pos:pos + 1].isspace():
        pos += 1
    fields.append(data[start:pos])
pos += 1
if fields[0] != b'P6' or fields[3] != b'255':
    sys.exit('ppm2png.py: not an 8-bit P6 image')
width, height = int(fields[1]), int(fields[2])
pixels = data[pos:]
if len(pixels) != width * height * 3:
    sys.exit('ppm2png.py: truncated image')
raw = b''.join(b'\0' + pixels[y * width * 3:(y + 1) * width * 3]
               for y in range(height))


def chunk(kind, body):
    return (struct.pack('>I', len(body)) + kind + body +
            struct.pack('>I', zlib.crc32(kind + body)))


with open(sys.argv[2], 'wb') as out:
    out.write(b'\x89PNG\r\n\x1a\n')
    out.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2,
                                         0, 0, 0)))
    out.write(chunk(b'IDAT', zlib.compress(raw)))
    out.write(chunk(b'IEND', b''))
