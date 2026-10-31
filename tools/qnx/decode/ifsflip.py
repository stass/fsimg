#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
"""Build the E10 oracle-sensitivity disk: mutated copies of native images.

Research aid. Usage: ifsflip.py NONBOOT.ifs COMPRESSED.ifs OUT.img

NONBOOT is an uncompressed nonbootable image with at least one file and one
symbolic link; COMPRESSED a nonbootable LZO image. Each case is one copy
with one change, written at a 512 KiB slot of OUT.img. The script prints the
guest loop that runs `dumpifs -v` on every slot and the case table
(slot, name, length, offset, change) for the capture record.
"""
import struct
import sys

SLOT = 524288


def records(img):
    hds, doff = struct.unpack_from("<II", img, 12)
    p = doff
    while p < hds:
        size, xo, ino, mode = struct.unpack_from("<HHII", img, p)
        if size == 0:
            return
        yield p, size, mode
        p += size


def flip(img, off, xor=0x01):
    b = bytearray(img)
    b[off] ^= xor
    return bytes(b), "byte %d ^= %#x" % (off, xor)


def put32(img, off, val):
    b = bytearray(img)
    struct.pack_into("<I", b, off, val)
    return bytes(b), "u32 at %d = %#x" % (off, val)


def main():
    nb = open(sys.argv[1], "rb").read()
    cz = open(sys.argv[2], "rb").read()
    isz, hds, doff = struct.unpack_from("<III", nb, 8)
    recs = list(records(nb))
    frec = next(r for r in recs if r[2] & 0o170000 == 0o100000)
    lrec = next(r for r in recs if r[2] & 0o170000 == 0o120000)
    fdata = struct.unpack_from("<I", nb, frec[0] + 24)[0]
    ssz, st = struct.unpack_from("<II", cz, 0x20)
    cases = [
        ("unchanged", nb, "none"),
        ("record-name", *flip(nb, frec[0] + 32)),
        ("record-mode", *flip(nb, frec[0] + 9, 0x10)),
        ("record-size", *flip(nb, frec[0], 0x04)),
        ("link-target", *flip(nb, lrec[0] + lrec[1] - 6)),
        ("file-data", *flip(nb, fdata)),
        ("imagefs-trailer", *flip(nb, isz - 1)),
        ("image-size-plus4", *put32(nb, 8, isz + 4)),
        ("hdr-dir-size-minus4", *put32(nb, 12, hds - 4)),
        ("terminator", *flip(nb, hds - 4)),
        ("data-offset-past-end", *put32(nb, frec[0] + 24, isz)),
        ("trailer-byteswapped",
         nb[:isz - 4] + nb[isz - 4:isz][::-1] + nb[isz:], "trailer reversed"),
        ("compressed-unchanged", cz, "none"),
        ("compressed-chunk", *flip(cz, ssz + 40)),
        ("stored-trailer", *flip(cz, st - 1)),
        ("startup-byte", *flip(cz, 0x30 + 2)),
    ]
    out = bytearray(SLOT * len(cases))
    for i, (name, img, change) in enumerate(cases):
        out[i * SLOT:i * SLOT + len(img)] = img
        print("SLOTCASE %d %s len=%d %s" % (i, name, len(img), change))
    open(sys.argv[3], "wb").write(out)
    lens = " ".join("%d:%d" % (i, len(c[1])) for i, c in enumerate(cases))
    print("GUEST for c in %s; do i=${c%%:*}; n=${c#*:}; "
          "dd if=/dev/hd0 of=/tmp/x bs=1 skip=$((i * %d)) count=$n "
          "2>/dev/null; echo \"SLOT $i\"; dumpifs -v /tmp/x 2>&1; "
          "echo \"rc=$?\"; done" % (lens, SLOT))


if __name__ == "__main__":
    main()
