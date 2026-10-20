#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# Decode a QNX4 capture (gzip, zstd or raw): the loader block, the four
# root-block records and the rest of block 2, every directory slot of every
# directory reachable from the root, the .inodes slots, extent-block chains,
# a bitmap summary, and which byte ranges were written over the harness's
# 0xAA fill. Usage: q4dump.py [-o OFFSET] [-l LENGTH] [-q] IMAGE
#
# OFFSET and LENGTH select a slice in bytes (a partition of a disk image).
# -q omits the per-directory slot listing and the written-range map.
#
# Research aid for trusted local images (the owner's captures), not a test
# oracle and independent of lib/: it bounds nothing beyond a visited set, so
# a malformed image can exhaust memory or raise. Field offsets follow the
# research draft fsspecs/QNX4_fsimg_spec.md sections 3, 4, 5 and 6.
import getopt, gzip, struct, subprocess, sys

opts, args = getopt.getopt(sys.argv[1:], 'o:l:q')
opts = dict(opts)
p = args[0]
if p.endswith('.gz'):
    img = gzip.open(p).read()
elif p.endswith('.zst'):
    img = subprocess.run(['zstd', '-dc', p], check=True,
                         stdout=subprocess.PIPE).stdout
else:
    img = open(p, 'rb').read()
base = int(opts.get('-o', '0'), 0)
size = int(opts.get('-l', str(len(img) - base)), 0)
d = img[base:base + size]
quiet = '-q' in opts
BS = 512

def blk(b):
    return d[(b - 1) * BS:b * BS]

def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u16(b, o): return struct.unpack_from('<H', b, o)[0]

ST = {0x01: 'USED', 0x02: 'MOD', 0x04: 'BUSY', 0x08: 'LINK',
      0x10: 'INODE', 0x20: 'FSYS_CLEAN', 0x40: 'x40', 0x80: 'x80'}

def status(s):
    return '%02x[%s]' % (s, '|'.join(v for k, v in ST.items() if s & k))

def name(b, n):
    z = b[:n].find(b'\0')
    return b[:n] if z < 0 else b[:z]

def tail_bytes(b, n):
    t = b[n:]
    return '' if not any(t) else ' tail=' + t.hex()

def inode(r):
    return dict(name=name(r, 16), tail=tail_bytes(r[:16], len(name(r, 16))),
                size=u32(r, 0x10), x0=u32(r, 0x14), xn=u32(r, 0x18),
                xblk=u32(r, 0x1c), ftime=u32(r, 0x20), mtime=u32(r, 0x24),
                atime=u32(r, 0x28), ctime=u32(r, 0x2c), nx=u16(r, 0x30),
                mode=u16(r, 0x32), uid=u16(r, 0x34), gid=u16(r, 0x36),
                nlink=u16(r, 0x38), zero=r[0x3a:0x3e].hex(), type=r[0x3e],
                st=r[0x3f])

def fmt_inode(i):
    return ('%-18r size=%d ext=(%d,%d) xblk=%d nx=%d mode=%06o uid=%d '
            'gid=%d nlink=%d t=%d/%d/%d/%d z=%s type=%02x st=%s%s' % (
                i['name'], i['size'], i['x0'], i['xn'], i['xblk'], i['nx'],
                i['mode'], i['uid'], i['gid'], i['nlink'], i['ftime'],
                i['mtime'], i['atime'], i['ctime'], i['zero'], i['type'],
                status(i['st']), i['tail']))

def link(r):
    return dict(name=name(r, 48), tail=tail_bytes(r[:48], len(name(r, 48))),
                iblk=u32(r, 0x30), islot=r[0x34], lblk=u32(r, 0x35),
                spare=r[0x39:0x3f].hex(), st=r[0x3f])

def fmt_link(l):
    return '%-50r -> (%d,%d) lfn=%d spare=%s st=%s%s' % (
        l['name'], l['iblk'], l['islot'], l['lblk'], l['spare'],
        status(l['st']), l['tail'])

def extents(i, label):
    """Extent list of an inode, printing every extent block on the way."""
    out = []
    if i['xn'] or i['x0']:
        out.append((i['x0'], i['xn']))
    xb, prev, seen = i['xblk'], 0, set()
    while xb and xb not in seen:
        seen.add(xb)
        b = blk(xb)
        nxt, prv, used, cov = u32(b, 0), u32(b, 4), b[8], u32(b, 12)
        ents = [(u32(b, 16 + 8 * k), u32(b, 20 + 8 * k)) for k in range(used)]
        print('  xblk %d of %s: next=%d prev=%d%s used=%d res=%s covered=%d'
              ' sum=%d sig=%r copy=(%d,%d) unused-nonzero=%s' % (
                  xb, label, nxt, prv, '' if prv == prev else '(!)', used,
                  b[9:12].hex(), cov, sum(n for _, n in ents),
                  b[0x1f0:0x1f8], u32(b, 0x1f8), u32(b, 0x1fc),
                  any(b[16 + 8 * used:0x1f0])))
        out.extend(ents)
        prev, xb = xb, nxt
    if len(out) != i['nx']:
        print('  ! %s: %d extents found, extent_count %d' % (
            label, len(out), i['nx']))
    return out

def content(i, label):
    ex = extents(i, label)
    data = b''.join(d[(s - 1) * BS:(s - 1 + n) * BS] for s, n in ex)
    alloc = sum(n for _, n in ex)
    if alloc * BS < i['size'] or alloc > (i['size'] + BS - 1) // BS:
        print('  %s: size %d, allocated %d blocks (%d bytes)' % (
            label, i['size'], alloc, alloc * BS))
    return ex, data

def show_written():
    runs, start = [], None
    for off in range(0, len(d), BS):
        w = d[off:off + BS] != b'\xaa' * BS
        if w and start is None:
            start = off
        if not w and start is not None:
            runs.append((start, off)); start = None
    if start is not None:
        runs.append((start, len(d)))
    print('written (not 0xAA) 512-byte blocks, 1-based QNX numbers:')
    for a, b in runs:
        print('  %d..%d (%d blocks)' % (a // BS + 1, b // BS, (b - a) // BS))

print('image %s slice %d+%d (%d blocks)' % (p, base, len(d), len(d) // BS))
loader = blk(1)
print('loader: %s' % ('all 0xAA' if loader == b'\xaa' * BS else
                      'all zero' if not any(loader) else
                      'nonzero, sha-prefix %s' % loader[:16].hex()))
rb = blk(2)
special = []
for k, label in enumerate(('/', '.inodes', '.boot', '.altboot')):
    i = inode(rb[64 * k:64 * k + 64])
    special.append(i)
    print('rootblk[%d] %s' % (k, fmt_inode(i)))
upper = rb[0x100:0x200]
print('rootblk 0x100..0x1ff: %s' % (
    'all zero' if not any(upper) else
    'all 0xAA' if upper == b'\xaa' * 256 else upper.hex()))
for o in range(0x100, 0x200, 32):
    if any(rb[o:o + 32]) and rb[o:o + 32] != b'\xaa' * 32:
        print('  %03x: %s' % (o, rb[o:o + 32].hex()))

inodes_ex, inodes_data = content(special[1], '.inodes')
print('.inodes extents %r, %d slots' % (inodes_ex, len(inodes_data) // 64))
inode_blocks = {}
for s, n in inodes_ex:
    for b in range(s, s + n):
        inode_blocks[b] = True
for k in range(len(inodes_data) // 64):
    r = inodes_data[64 * k:64 * k + 64]
    if any(r) and r != b'\xaa' * 64:
        print('  inodes[%d] %s' % (k, fmt_inode(inode(r))))

seen_dirs = set()
bitmap = None

def walk(i, path, ident):
    global bitmap
    if ident in seen_dirs:
        print('! %s: directory reached twice' % path)
        return
    seen_dirs.add(ident)
    ex, data = content(i, path)
    print('dir %s ident=%r size=%d extents=%r' % (path, ident, i['size'], ex))
    subdirs = []
    blocks = [b for s, n in ex for b in range(s, s + n)]
    for k in range(i['size'] // 64):
        r = data[64 * k:64 * k + 64]
        pos = (blocks[k // 8], k % 8)
        st = r[0x3f]
        if st & 0x08:
            l = link(r)
            if not quiet:
                print('  %r L %s' % (pos, fmt_link(l)))
            tr = blk(l['iblk'])[64 * l['islot']:64 * l['islot'] + 64]
            t = inode(tr)
            if not quiet:
                print('      target %s%s' % (
                    fmt_inode(t), '' if l['iblk'] in inode_blocks else
                    ' (outside .inodes)'))
            if t['xblk'] and (t['mode'] & 0o170000) != 0o040000 and k >= 2:
                extents(t, path + l['name'].decode('latin1'))
            if (t['mode'] & 0o170000) == 0o040000 and k >= 2:
                subdirs.append((t, path + l['name'].decode('latin1') + '/',
                                (l['iblk'], l['islot'])))
        elif st & 0x01:
            t = inode(r)
            if not quiet:
                print('  %r D %s' % (pos, fmt_inode(t)))
            if t['xblk'] and (t['mode'] & 0o170000) != 0o040000:
                extents(t, path + t['name'].decode('latin1'))
            if path == '/' and t['name'] == b'.bitmap':
                bitmap = t
            if (t['mode'] & 0o170000) == 0o040000 and k >= 2:
                subdirs.append((t, path + t['name'].decode('latin1') + '/',
                                pos))
        elif any(r) and not quiet:
            print('  %r free-nonzero %s' % (pos, r.hex()))
        if k < 2 and not quiet:
            sig = r.find(b'I\x03QNX')
            print('      dot%d signature offset %s' % (k, sig))
    for t, sp, sid in subdirs:
        walk(t, sp, sid)

walk(special[0], '/', (2, 0))

if bitmap:
    ex, bm = content(bitmap, '.bitmap')
    m = bitmap['size']
    bits = len(d) // BS
    print('.bitmap size=%d bytes (%d bits) extents=%r slice blocks=%d '
          'ceil(N/8)=%d' % (m, 8 * m, ex, bits, (bits + 7) // 8))
    used = sum(bin(x).count('1') for x in bm[:m])
    print('  set bits in size: %d, free %d' % (used, 8 * m - used))
    last = bm[m - 1] if m else 0
    print('  last byte %02x; bytes past size (to block end): %s' % (
        last, bm[m:].hex() if len(bm) - m <= 64 else
        '%d bytes, distinct %r' % (len(bm) - m, sorted(set(bm[m:])))))
    nb = min(bits, 8 * m)
    alloc = [b + 1 for b in range(nb) if bm[b >> 3] >> (b & 7) & 1]
    runs, a = [], None
    for b in alloc:
        if a is None or b != runs[-1][1] + 1:
            runs.append([b, b])
        else:
            runs[-1][1] = b
        a = b
    print('  allocated runs: %s' % ' '.join(
        '%d-%d' % (x, y) for x, y in runs[:40]))

if not quiet:
    show_written()
