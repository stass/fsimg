#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# Disassemble an SH-4 LE function from an ELF with cstool, annotating PC-relative literals
# and call targets with symbol names.  Usage: shdis.py elf funcname
# Needs Capstone's cstool and radare2's rabin2 (symbol table) on PATH. Input
# binaries are trusted research material; literals outside mapped segments
# raise.
import subprocess, struct, sys, re
elf, fn = sys.argv[1], sys.argv[2]
d = open(elf, 'rb').read()
syms = {}
for line in subprocess.run(['rabin2', '-s', elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) >= 7 and p[2].startswith('0x'): syms.setdefault(int(p[2], 16), p[6]); syms[p[6]] = (int(p[2], 16), int(p[5]) if p[5].isdigit() else 0)
segs = []
for line in subprocess.run(['rabin2', '-S', elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) >= 6 and p[1].startswith('0x') and p[3].startswith('0x'):
        segs.append((int(p[3], 16), int(p[4], 16), int(p[1], 16)))
def off(va):
    for v, sz, o in segs:
        if v <= va < v + sz: return va - v + o
va, size = syms[fn]
o = off(va)
lines = []; pos = 0
while pos < size:
    out = subprocess.run(['cstool', 'sh4', d[o+pos:o+size].hex(), hex(va+pos)], capture_output=True, text=True).stdout.splitlines()
    got = [l for l in out if re.match(r'\s*[0-9a-f]+\s+(?:[0-9a-f]{2} )+', l)]
    lines += got
    if got: pos = int(got[-1].split()[0], 16) - va + 2
    lines.append(f' {va+pos:x}  {d[o+pos:o+pos+2].hex()[:2]} {d[o+pos:o+pos+2].hex()[2:]}   .word'); pos += 2
for line in lines:
    m = re.match(r'\s*([0-9a-f]+)\s+((?:[0-9a-f]{2} )+)\s+(.*)', line)
    if not m: print(line); continue
    a = int(m.group(1), 16); ins = m.group(3); b = bytes.fromhex(m.group(2).replace(' ', ''))
    w = struct.unpack('<H', b[:2])[0]; note = ''
    if w >> 12 == 0xd:
        la = (a & ~3) + 4 + (w & 0xff) * 4; lv = struct.unpack_from('<I', d, off(la))[0]
        note = f'  ; =0x{lv:x} ({lv}) {syms.get(lv, "") if isinstance(syms.get(lv), str) else ""}'
    elif w >> 12 == 0x9:
        la = a + 4 + (w & 0xff) * 2; lv = struct.unpack_from('<h', d, off(la))[0]
        note = f'  ; =w 0x{lv & 0xffff:x} ({lv})'
    print(f'{a:08x}  {ins}{note}')
