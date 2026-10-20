#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# List paths in a classic little-endian QNX6 volume at a byte offset of an image.
# Usage: q6ls.py image offset length [extract-path outfile] (slice in bytes)
# Uses the valid header (magic and CRC) with the larger serial. Names are
# printed raw; extraction writes the file's contents to outfile, so keep it
# outside the source tree.
#
# Research aid for trusted local images (the owner's captures and HDD), not a
# test oracle: it does not bound tree depth, fan-out or block references, so a
# malformed image can exhaust memory or raise. Offsets follow the tables in
# specs/design-qnx6.md (header at 8192, tree descriptors at 72/152/232/312,
# 128-byte inodes with block references at 36 and depth at 100).
import mmap, struct, sys
f = open(sys.argv[1], 'rb'); m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
P = int(sys.argv[2]); V = int(sys.argv[3]); H = P + 8192; D = H + 4096
u = lambda fmt, off: struct.unpack_from('<' + fmt, m, off)[0]
assert u('I', H) == 0x68191122
B = u('I', H + 48)
def blk(q): return m[D+q*B:D+(q+1)*B]
def stream(off, length, depth):
    ptrs = [u('I', off + 4*i) for i in range(16)]
    for _ in range(depth):
        nxt = []
        for q in ptrs:
            if q == 0xffffffff: continue
            nxt += list(struct.unpack('<%dI' % (B//4), blk(q)))
        ptrs = nxt
    out = bytearray()
    for q in ptrs:
        if q == 0xffffffff or len(out) >= length: break
        out += blk(q)
    return bytes(out[:length])
def crc(b):
    """QNX6 superblock CRC-32: MSB first, polynomial 0x04c11db7, no inversion."""
    r = 0
    for x in b:
        r ^= x << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04c11db7) & 0xffffffff if r & 0x80000000 else (r << 1) & 0xffffffff
    return r
def valid(o): return u('I', o) == 0x68191122 and u('I', o + 4) == crc(m[o+8:o+512])
# second header is in the slice's last whole 4096-byte unit
S2 = P + (V // 4096) * 4096 - 4096
if not 12288 < S2 - P or S2 + 4096 > len(m): sys.exit('slice too short or wrong length')
cands = [o for o in (H, S2) if o + 512 <= len(m) and valid(o)]
if not cands: sys.exit('no valid header')
sb = max(cands, key=lambda o: u('Q', o + 8))
B = u('I', sb + 48)
itab = stream(sb + 80, u('Q', sb + 72), m[sb + 144])
lnt = stream(sb + 240, u('Q', sb + 232), m[sb + 304])
def ino(n): return itab[(n-1)*128:n*128]
def data(n):
    r = ino(n); size = struct.unpack_from('<Q', r)[0]
    # inode pointers at 36, depth at 100; read via a fake offset buffer
    ptrs = list(struct.unpack_from('<16I', r, 36)); depth = r[100]
    for _ in range(depth):
        nxt = []
        for q in ptrs:
            if q == 0xffffffff: continue
            nxt += list(struct.unpack('<%dI' % (B//4), blk(q)))
        ptrs = nxt
    out = bytearray()
    for q in ptrs:
        if q == 0xffffffff or len(out) >= size: break
        out += blk(q)
    return bytes(out[:size])
def walk(n, path, depth=0):
    if depth > 40: return
    d = data(n)
    for i in range(0, len(d), 32):
        e = d[i:i+32]; en, ln = struct.unpack_from('<IB', e)
        if en == 0: continue
        if ln <= 27: name = e[5:5+ln]
        else:
            li = struct.unpack_from('<I', e, 8)[0]
            rec = lnt[li*B:(li+1)*B]; name = rec[2:2+struct.unpack_from('<H', rec)[0]]
        name = name.decode(errors='replace')
        if name in ('.', '..'): continue
        r = ino(en); mode = struct.unpack_from('<H', r, 32)[0]; size = struct.unpack_from('<Q', r)[0]
        p = path + '/' + name
        yield p, mode, size, en
        if mode & 0o170000 == 0o040000: yield from walk(en, p, depth+1)
if len(sys.argv) > 4:
    for p, mode, size, en in walk(1, ''):
        if p == sys.argv[4]: open(sys.argv[5], 'wb').write(data(en)); print('wrote', size); break
else:
    for p, mode, size, en in walk(1, ''): print(f'{mode:06o} {size:10d} {p}')
