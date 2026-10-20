#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# Analyze one classic QNX6 volume inside a disk image at a byte offset, read
# only. Prints structure and statistics, never file contents. Names are
# printed only with --names. Usage: pcmq6.py IMAGE OFFSET LENGTH [--names].
# Statistics come from the valid header (magic and CRC) with the larger
# serial.
#
# Research aid for trusted local images (the owner's captures and HDD), not a
# test oracle: it does not bound tree depth, fan-out or block references, so a
# malformed image can exhaust memory or raise. Offsets follow the tables in
# specs/design-qnx6.md (header at 8192, tree descriptors at 72/152/232/312,
# 128-byte inodes with block references at 36 and depth at 100).
import mmap, struct, sys, collections
show_names = '--names' in sys.argv
args = [a for a in sys.argv[1:] if a != '--names']
path, base, size = args[0], int(args[1]), int(args[2])
f = open(path, 'rb'); m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
H = base + 8192; D = H + 4096
if m[H:H+4] == b'\x22\x11\x19\x68': E = '<'
elif m[H:H+4] == b'\x68\x19\x11\x22': E = '>'
else: sys.exit(f'no classic qnx6 header at {base:#x}+8192: {m[H:H+4].hex()}')
def u(fmt, off): return struct.unpack_from(E + fmt, m, off)[0]
def crc(b):
    r = 0
    for x in b:
        r ^= x << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04c11db7) & 0xffffffff if r & 0x80000000 else (r << 1) & 0xffffffff
    return r
B = u('I', H+48); N = u('I', H+60)
# second header is in the slice's last whole 4096-byte unit
S2 = base + (size // 4096) * 4096 - 4096
if not 12288 < S2 - base or S2 + 4096 > len(m): sys.exit('slice too short or wrong length')
hdr = {}
for nm, o in (('h0', H), ('h1', S2)):
    ok = m[o:o+4] == m[H:H+4] and u('I', o+4) == crc(m[o+8:o+512])
    if ok: hdr[nm] = o
    print(f'{nm} @part+{o-base:#x}: valid={ok} serial={u("Q",o+8)} fmt={u("I",o+16)} sync={u("I",o+20)} flags={u("I",o+24):#x}'
          f' w28={u("H",o+28)} w30={u("H",o+30)} id={m[o+32:o+48].hex()} B={u("I",o+48)} inodes={u("I",o+52)}'
          f' freeino={u("I",o+56)} N={u("I",o+60)} free={u("I",o+64)} groups={u("I",o+68)}')
    for tn, to in (('inode',72),('bitmap',152),('lname',232),('t312',312)):
        q = [u('I', o+to+8+4*i) for i in range(16)]
        print(f'   {tn}: len={u("Q",o+to)} depth={m[o+to+72]} mode={m[o+to+73]} ext={m[o+to+74:o+to+80].hex()} roots={[hex(x) for x in q if x!=0xffffffff]}')
    print(f'   392..512 zero={m[o+392:o+512]==bytes(120)}  512..4096 zero={m[o+512:o+4096]==bytes(3584)}')
E_end = D + N*B
V = size; Ecalc = base + (V // 4096) * 4096
print(f'geometry: B={B} N={N} part_bytes={V} data_end=part+{E_end-base:#x} second_header=part+{S2-base:#x} expected_end(floor V/4096)=part+{Ecalc-base:#x} N_expected={(((V//4096)*4096)-16384)//B} unused_before_second_header={S2-E_end}')
if not hdr: sys.exit('no valid header')
sb = max(hdr.values(), key=lambda o: u('Q', o+8))
B = u('I', sb+48); N = u('I', sb+60)
print(f'selected header part+{sb-base:#x} serial={u("Q", sb+8)}')
def blk(q): return m[D+q*B:D+(q+1)*B]
def tree(buf, off, depth):
    """Return (leaf list, indirect list) for 16 root refs at buf[off]."""
    ptrs = list(struct.unpack_from(E+'16I', buf, off)); ind = []
    for _ in range(depth):
        nxt = []
        for q in ptrs:
            if q == 0xffffffff: continue
            if q >= N: raise ValueError(f'ref {q} >= N')
            ind.append(q); nxt += struct.unpack_from(E+f'{B//4}I', blk(q))
        ptrs = nxt
    return [q for q in ptrs if q != 0xffffffff], ind
il, ii = tree(m, sb+72+8, m[sb+72+72]); bl, bi = tree(m, sb+152+8, m[sb+152+72])
ll, li = tree(m, sb+232+8, m[sb+232+72])
sysblocks = sorted(il + ii + bl + bi); R = len(sysblocks)
print(f'system blocks: R={R} bitmap leaves={len(bl)} ind={len(bi)} inode leaves={len(il)} ind={len(ii)} min={min(sysblocks)} max={max(sysblocks)}'
      f' all<2R={max(sysblocks) < 2*R}')
itab = b''.join(blk(q) for q in il)[:u('Q', sb+72)]
bm = b''.join(blk(q) for q in bl)
def allocated(q): return bm[q >> 3] >> (q & 7) & 1
print(f'bitmap: len={u("Q", sb+152)} set={sum(bin(x).count("1") for x in bm[:N//8])} (N-free={N-u("I", sb+64)}) twin-half-allocated={all(allocated(q) for q in range(2*R))}'
      f' pad-past-N={sorted(set(bm[(N+7)//8:]))[:4]}')
lnt = b''.join(blk(q) for q in ll)[:u('Q', sb+232)]
st = collections.Counter(); modes = collections.Counter(); w34 = collections.Counter(); owners = collections.Counter()
tail_nz = 0; o102 = collections.Counter(); times = []; ftime_eq = 0; zero_inodes = 0; live = {}
for i in range(len(itab) // 128):
    r = itab[i*128:(i+1)*128]
    if r == bytes(128): zero_inodes += 1; continue
    s = r[101]; st[s] += 1
    if s & 1:
        mode = struct.unpack_from(E+'H', r, 32)[0]; live[i+1] = r
        modes[oct(mode & 0o170000)] += 1; w34[struct.unpack_from(E+'H', r, 34)[0]] += 1
        owners[struct.unpack_from(E+'II', r, 8)] += 1
        t = struct.unpack_from(E+'IIII', r, 16); times.append(t)
        ftime_eq += t[0] <= min(t[1:])
        if r[104:] != bytes(24): tail_nz += 1
        o102[r[102:104].hex()] += 1
print(f'inodes: table={len(itab)//128} zero={zero_inodes} status={dict(st)} live={len(live)} types={dict(modes)}')
print(f'   link-count values={dict(sorted(w34.items())[:8])} owners(top)={owners.most_common(5)} off102={dict(o102)} tail104 nonzero={tail_nz}')
if times:
    print(f'   ftime<=other times: {ftime_eq}/{len(times)}  ftime range={min(t[0] for t in times)}..{max(t[0] for t in times)}  mtime range={min(t[1] for t in times)}..{max(t[1] for t in times)}')
for k in (1, 2):
    if k in live:
        r = live[k]; print(f'   inode {k}: mode={struct.unpack_from(E+"H", r, 32)[0]:#o} links={struct.unpack_from(E+"H", r, 34)[0]} status={r[101]} uid/gid={struct.unpack_from(E+"II", r, 8)}')
def cks(nm):
    c = 0
    for ch in nm: c = (((c >> 1) + ch) & 0xffffffff) ^ (0x80000000 if c & 1 else 0)
    return c or 0xffffffff
nslots = live_slots = longn = cks_ok = cks_bad = cks_zero = stale = nonzero_long_pad = 0; maxlen = 0
nonutf8 = 0; ctl = 0; dirs = 0; boot_name = None; tailsz = collections.Counter()
for ino, r in live.items():
    mode = struct.unpack_from(E+'H', r, 32)[0]
    if mode & 0o170000 != 0o040000: continue
    dirs += 1; ln = struct.unpack_from(E+'Q', r, 0)[0]
    lv, _ = tree(r, 36, r[100]); data = b''.join(blk(q) for q in lv)[:ln]
    for k in range(0, len(data), 32):
        s = data[k:k+32]; nslots += 1
        t = struct.unpack_from(E+'I', s)[0]
        if t == 0:
            stale += s != bytes(32); continue
        live_slots += 1
        if s[4] == 0xff:
            longn += 1; j = struct.unpack_from(E+'I', s, 8)[0]; rec = lnt[j*B:(j+1)*B]
            nl = struct.unpack_from(E+'H', rec)[0]; nm = rec[2:2+nl]
            stored = struct.unpack_from(E+'I', s, 12)[0]
            if stored == 0: cks_zero += 1
            elif stored == cks(nm): cks_ok += 1
            else: cks_bad += 1
            nonzero_long_pad += s[5:8] != bytes(3) or s[16:] != bytes(16)
        else:
            nm = s[5:5+s[4]]
        maxlen = max(maxlen, len(nm))
        try: nm.decode('utf-8')
        except UnicodeDecodeError: nonutf8 += 1
        ctl += any(c < 0x20 or c == 0x7f for c in nm)
        if ino == 1 and t == 2: boot_name = nm
        if show_names: print('   name', ino, '->', t, nm)
print(f'dirs={dirs} slots={nslots} live={live_slots} unused-with-stale-bytes={stale} long={longn} cks ok/bad/zero={cks_ok}/{cks_bad}/{cks_zero}'
      f' longslot-nonzero-pad={nonzero_long_pad} maxlen={maxlen} non-utf8={nonutf8} control-bytes={ctl} root->inode2 name={boot_name!r}')
ba = m[base:base+8192]
print(f'boot area: zero={ba==bytes(8192)} loader-text={b"QNX v1.2b Boot Loader" in ba} sig={ba[510:512].hex()}')
