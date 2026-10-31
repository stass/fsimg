#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# Walk the newest snapshot of a classic QNX6 capture and print every inode,
# directory slot, long-name record and checksum, and content summary.
# Usage: q6walk.py IMAGE[.gz], the whole image being the volume's slice. The snapshot is the valid header (magic and
# CRC) with the larger serial, the first on a tie, as QNX selects it.
#
# Research aid for trusted local images (the owner's captures and HDD), not a
# test oracle: it does not bound tree depth, fan-out or block references, so a
# malformed image can exhaust memory or raise. Offsets follow the tables in
# specs/design-qnx6.md (header at 8192, tree descriptors at 72/152/232/312,
# 128-byte inodes with block references at 36 and depth at 100).
import gzip, struct, sys
p = sys.argv[1]
d = gzip.open(p).read() if p.endswith('.gz') else open(p, 'rb').read()
H = 8192; D = H + 4096
E = '<' if d[H:H+4] == b'\x22\x11\x19\x68' else '>'
def u(fmt, off, buf=d): return struct.unpack_from(E + fmt, buf, off)[0]
B = u('I', H + 48); N = u('I', H + 60)
# second header is in the slice's last whole 4096-byte unit
S2 = (len(d) // 4096) * 4096 - 4096
if not 12288 < S2 - 0 or S2 + 4096 > len(d): sys.exit('slice too short or wrong length')
def crc(b):
    """QNX6 superblock CRC-32: MSB first, polynomial 0x04c11db7, no inversion."""
    r = 0
    for x in b:
        r ^= x << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04c11db7) & 0xffffffff if r & 0x80000000 else (r << 1) & 0xffffffff
    return r
def valid(o): return d[o:o+4] == d[H:H+4] and u('I', o + 4) == crc(d[o+8:o+512])
cands = [o for o in (H, S2) if o + 512 <= len(d) and valid(o)]
if not cands: sys.exit('no valid header')
sb = max(cands, key=lambda o: u('Q', o + 8))
B = u('I', sb + 48); N = u('I', sb + 60)
print(f'using sb @ {sb:#x} serial={u("Q", sb+8)} free={u("I", sb+64)} freeino={u("I", sb+56)}')
def blk(q): return d[D+q*B:D+(q+1)*B]
def leaves(buf, off, depth):
    ptrs = [u('I', off + 4*i, buf) for i in range(16)]
    meta = []
    for _ in range(depth):
        nxt = []
        for q in ptrs:
            if q == 0xffffffff: continue
            meta.append(q); ib = blk(q)
            nxt += [u('I', 4*i, ib) for i in range(B // 4)]
        ptrs = nxt
    return [q for q in ptrs if q != 0xffffffff], meta, ptrs
def stream(buf, off, length, depth):
    lv, meta, _ = leaves(buf, off, depth)
    return b''.join(blk(q) for q in lv)[:length], lv, meta
itab, _, _ = stream(d, sb + 72 + 8, u('Q', sb + 72), d[sb + 72 + 72])
lnt, lnblocks, _ = stream(d, sb + 232 + 8, u('Q', sb + 232), d[sb + 232 + 72])
print(f'longname tree len={u("Q", sb+232)} depth={d[sb+232+72]} blocks={[hex(x) for x in lnblocks]}')
def cks(name, signed):
    c = 0
    for ch in name:
        v = ch - 256 if signed and ch > 127 else ch
        c = (((c >> 1) + v) & 0xffffffff) ^ (0x80000000 if c & 1 else 0)
    return c
def ino(i):
    r = itab[(i-1)*128:i*128]
    return r
for i in range(1, len(itab) // 128 + 1):
    r = ino(i)
    if r == bytes(128): continue
    ln = u('Q', 0, r); mode = u('H', 32, r); depth = r[100]
    data, lv, meta = stream(r, 36, ln, depth)
    print(f'ino {i}: len={ln} uid={u("I",8,r)} gid={u("I",12,r)} ftime={u("I",16,r):#x} m={u("I",20,r):#x}'
          f' a={u("I",24,r):#x} c={u("I",28,r):#x} mode={mode:#o} w34={u("H",34,r):#06x} depth={depth}'
          f' status={r[101]} 102={r[102:104].hex()} tail={r[104:].hex() if r[104:] != bytes(24) else "0"}')
    allp = [u('I', 36+4*k, r) for k in range(16)]
    print(f'     ptrs={[hex(x) if x != 0xffffffff else "-" for x in allp]} leaves={len(lv)} meta={[hex(x) for x in meta]}')
    if lv:
        last = blk(lv[-1]); used = ln - (len(lv)-1)*B
        print(f'     last-block tail bytes={sorted(set(last[used:]))[:6] if used < B else "full"} contentset={sorted(set(data))[:6]}')
    if mode & 0o170000 == 0o040000:
        for k in range(0, len(data), 32):
            s = data[k:k+32]
            if s == bytes(32): continue
            t = u('I', 0, s)
            if s[4] == 0xff:
                j = u('I', 8, s); rec = lnt[j*B:(j+1)*B]; nl = u('H', 0, rec); nm = rec[2:2+nl]
                st = u('I', 12, s)
                print(f'     slot {k//32}: ino={t} LONG idx={j} b5..8={s[5:8].hex()} cks={st:#010x}'
                      f' unsigned={cks(nm,False):#010x} signed={cks(nm,True):#010x} len={nl} tailzero={rec[2+nl:]==bytes(B-2-nl)} tail16={s[16:].hex()} name={nm[:12]!r}..')
            else:
                print(f'     slot {k//32}: ino={t} len={s[4]} name={s[5:5+s[4]]!r} pad={s[5+s[4]:].hex() if s[5+s[4]:] != bytes(27-s[4]) else "0"}')
        print(f'     {len(data)//32} slots')
    elif mode & 0o170000 == 0o120000:
        print(f'     target={data!r}')
