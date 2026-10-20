#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# Disassemble an SH-4 LE address range of an ELF (via PT_LOAD), annotating
# PC-relative literal loads with values and C strings.  Usage: shrange.py elf start end
# (hex addresses). Needs Capstone's cstool on PATH; nop lines are omitted.
import sys, struct, subprocess, re
d = open(sys.argv[1], 'rb').read(); a0 = int(sys.argv[2], 16); a1 = int(sys.argv[3], 16)
phoff, = struct.unpack_from('<I', d, 28); pes, pn = struct.unpack_from('<HH', d, 42)
loads = [struct.unpack_from('<8I', d, phoff + i * pes) for i in range(pn)]
loads = [(l[1], l[4], l[2]) for l in loads if l[0] == 1]
def fo(v):
    for f, s, vv in loads:
        if vv <= v < vv + s: return f + v - vv
def cstr(v):
    o = fo(v)
    if o is None: return ''
    e = d.find(b'\0', o, o + 80); s = d[o:e if e > 0 else o + 80]
    return repr(s[:60]) if s and all(32 <= c < 127 or c in (9, 10) for c in s) else ''
pos = a0
while pos < a1:
    o = fo(pos)
    out = subprocess.run(['cstool', 'sh4', d[o:o + (a1 - pos)].hex(), hex(pos)], capture_output=True, text=True).stdout.splitlines()
    got = [l for l in out if re.match(r'\s*[0-9a-f]+\s+(?:[0-9a-f]{2} )+', l)]
    for line in got:
        m = re.match(r'\s*([0-9a-f]+)\s+((?:[0-9a-f]{2} )+)\s+(.*)', line)
        a = int(m.group(1), 16); w = struct.unpack('<H', bytes.fromhex(m.group(2).replace(' ', ''))[:2])[0]; note = ''
        if w >> 12 == 0xd:
            la = (a & ~3) + 4 + (w & 0xff) * 4; lv, = struct.unpack_from('<I', d, fo(la)); note = f'  ; =0x{lv:x} {cstr(lv)}'
        elif w >> 12 == 0x9:
            la = a + 4 + (w & 0xff) * 2; lv, = struct.unpack_from('<h', d, fo(la)); note = f'  ; =w {lv}'
        if 'nop' not in m.group(3): print(f'{a:08x}  {m.group(3)}{note}')
    pos = (int(got[-1].split()[0], 16) + 2) if got else pos
    print(f'{pos:08x}  .word'); pos += 2
