#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
"""Report the chunk framing of a compressed QNX boot image, without decoding.

Research aid; trusts its input. Usage: ifschunks.py [-o OFFSET] FILE

Prints the startup fields that locate the body, the stored chunk lengths
(16-bit big-endian, terminated by zero), the bytes between the terminator
and the stored trailer, and both additive checksums (the startup section,
and the stored body through stored_size).
"""
import argparse
import collections
import struct


def wordsum(b):
    return sum(struct.unpack_from("<%dI" % (len(b) // 4), b)) & 0xFFFFFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--offset", type=int, default=0)
    ap.add_argument("file")
    a = ap.parse_args()
    d = open(a.file, "rb").read()
    o = a.offset
    sig, ver, f1, f2, hsz, mach = struct.unpack_from("<IHBBHH", d, o)
    ram_size, ssz, st = struct.unpack_from("<III", d, o + 0x1C)
    isz = struct.unpack_from("<I", d, o + 0x2C)[0]
    pre = struct.unpack_from("<H", d, o + 0x30)[0]
    print("sig=%#x version=%d flags1=%#x flags2=%#x header_size=%d "
          "machine=%d preboot=%d startup_size=%d stored_size=%d "
          "imagefs_size=%d ram_size=%d file=%d" % (
              sig, ver, f1, f2, hsz, mach, pre, ssz, st, isz, ram_size,
              len(d) - o))
    p = o + ssz
    lens = []
    while True:
        n = struct.unpack_from(">H", d, p)[0]
        p += 2
        if n == 0:
            break
        lens.append(n)
        p += n
    end = o + st
    pad = d[p:end - 4]
    print("chunks=%d stored_len min=%s max=%s top=%s" % (
        len(lens), min(lens or [0]), max(lens or [0]),
        collections.Counter(lens).most_common(3)))
    print("terminator_end=%d stored_end=%d pad_len=%d pad_nonzero=%d "
          "(terminator_end - body) mod 4=%d" % (
              p - o, st, len(pad), sum(1 for b in pad if b),
              (p - o - ssz) % 4))
    print("startup_sum=%#x stored_sum=%#x stored_sum_from_startup_end=%#x" % (
        wordsum(d[o:o + ssz]), wordsum(d[o:o + st]),
        wordsum(d[o + ssz:o + st])))


if __name__ == "__main__":
    main()
