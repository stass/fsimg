#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# Decode a classic QNX6 capture (gzip or raw): superblocks, trees, inodes,
# root directory, bitmap summary, and which byte ranges the formatter wrote
# (the medium was pre-filled with 0xAA). Usage: q6dump.py IMAGE[.gz].
# Both headers are printed with their CRC check; inodes and the root
# directory are decoded from the first header's snapshot, which on a fresh
# format is the only one.
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
le = d[H:H+4] == b'\x22\x11\x19\x68'
E = '<' if le else '>'
def u(fmt, off): return struct.unpack_from(E + fmt, d, off)[0]
def crc(b):
    r = 0
    for x in b:
        r ^= x << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04c11db7) & 0xffffffff if r & 0x80000000 else (r << 1) & 0xffffffff
    return r
def tree(off):
    ln = u('Q', off); ptr = [u('I', off + 8 + 4*i) for i in range(16)]
    return dict(len=ln, ptr=[hex(x) if x != 0xffffffff else '-' for x in ptr],
                depth=d[off+72], mode=d[off+73], ext=d[off+74:off+80].hex())
def sb(off, name):
    print(f'== {name} @ {off:#x} magic={d[off:off+4].hex()} order={"LE" if le else "BE"}')
    print(f'  crc stored={u("I", off+4):#010x} computed={crc(d[off+8:off+512]):#010x}')
    print(f'  serial={u("Q", off+8)} ctime={u("I", off+16):#x} atime={u("I", off+20):#x}'
          f' flags={u("I", off+24):#010x} w28={u("H", off+28):#06x} w30={u("H", off+30):#06x}')
    print(f'  id={d[off+32:off+48].hex()} B={u("I", off+48)} inodes={u("I", off+52)}'
          f' freeino={u("I", off+56)} N={u("I", off+60)} free={u("I", off+64)} ag={u("I", off+68)}')
    for nm, o in (('inode', 72), ('bitmap', 152), ('longname', 232), ('tree312', 312), ('tree392', 392)):
        print(f'  {nm:9s}', tree(off + o))
    print(f'  472={d[off+472:off+476].hex()} 476={d[off+476:off+480].hex()} 480..512={d[off+480:off+512].hex()}')
    print(f'  pad 512..4096 all-AA={set(d[off+512:off+4096]) <= {0xaa}} values={sorted(set(d[off+512:off+4096]))[:8]}')
B = u('I', H + 48); N = u('I', H + 60)
S2 = D + N * B
sb(H, 'sb0'); sb(S2, 'sb1')
print(f'== geometry B={B} N={N} D={D:#x} S2={S2:#x} E={S2+4096:#x} disk={len(d):#x}')
# Boot area and tail.
ba = d[:H]
print(f'== boot area nonAA bytes={sum(1 for x in ba if x != 0xaa)} zero={ba.count(0)}  head={ba[:16].hex()}')
print(f'   boot 0x1fe..0x200={ba[0x1fe:0x200].hex()}')
tail = d[S2+4096:]
print(f'== tail after E: len={len(tail)} AA={tail.count(0xaa)}')
# Written-block map.
written = [i for i in range(N) if d[D+i*B:D+(i+1)*B] != b'\xaa' * B]
runs = []
for i in written:
    if runs and runs[-1][1] == i - 1: runs[-1][1] = i
    else: runs.append([i, i])
print(f'== written blocks: {len(written)} runs={[(hex(a), hex(b)) for a, b in runs][:20]}')
def blk(p): return d[D+p*B:D+(p+1)*B]
def stream(t_off, limit):
    ln = u('Q', t_off); depth = d[t_off+72]
    ptrs = [u('I', t_off + 8 + 4*i) for i in range(16)]
    for _ in range(depth):
        nxt = []
        for q in ptrs:
            if q == 0xffffffff: continue
            nxt += [u('I', D + q*B + 4*i) for i in range(B // 4)]
        ptrs = nxt
    out = b''.join(blk(q) for q in ptrs if q != 0xffffffff)
    return out[:min(ln, limit)]
it = stream(H + 72, 128 * 8)
for i in range(min(8, len(it) // 128)):
    r = it[i*128:(i+1)*128]
    if r == b'\xaa' * 128: print(f'  ino {i+1}: all AA'); continue
    if r == bytes(128): print(f'  ino {i+1}: all zero'); continue
    f = struct.unpack_from(E + 'QIIIIIIHH', r)
    ptr = [struct.unpack_from(E+'I', r, 36+4*k)[0] for k in range(16)]
    print(f'  ino {i+1}: len={f[0]} uid={f[1]} gid={f[2]} ftime={f[3]:#x} m={f[4]:#x} a={f[5]:#x} c={f[6]:#x}'
          f' mode={f[7]:#o} w34={f[8]:#06x} ptr={[hex(x) for x in ptr if x != 0xffffffff]} depth={r[100]}'
          f' status={r[101]} 102={r[102:104].hex()} 104..128={r[104:128].hex()}')
# Directory slots of inodes 1 and 2, and the bitmap's set range and padding.
for ino in (1, 2):
    r = it[(ino-1)*128:ino*128]
    if len(r) < 128 or r in (bytes(128), b'\xaa' * 128): continue
    ln = struct.unpack_from(E+'Q', r)[0]; p0 = struct.unpack_from(E+'I', r, 36)[0]
    db = blk(p0)[:ln]
    for k in range(0, len(db), 32):
        s = db[k:k+32]
        if s == bytes(32): continue
        print(f'  dir {ino} slot {k//32}: ino={struct.unpack_from(E+"I", s)[0]} len={s[4]} name={s[5:5+min(s[4],27)]!r} raw={s.hex()}')
    nz = sum(1 for k in range(0, len(db), 32) if db[k:k+32] == bytes(32))
    print(f'  dir {ino}: {len(db)//32} slots, {nz} all-zero')
bm = stream(H + 152, 1 << 30)
bits = ''.join(f'{x:08b}'[::-1] for x in bm)
print(f'== bitmap len={len(bm)} set={bits.count("1")} first-clear={bits.find("0")} last-set={bits.rfind("1")}')
# Bytes in the bitmap blocks past the declared length.
bt = H + 152; bptr = [u('I', bt + 8 + 4*i) for i in range(16) if u('I', bt + 8 + 4*i) != 0xffffffff]
raw = b''.join(blk(q) for q in bptr) if d[bt+72] == 0 else b''
print(f'   bitmap block bytes past length: {sorted(set(raw[len(bm):]))}')
