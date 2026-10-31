#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# Survey one QNX4 volume inside a disk image at a byte offset, read only.
# Prints structure counts and histograms, never names, file contents,
# identities or timestamps (only whether timestamps are equal across a
# record). Usage: pcmq4.py IMAGE OFFSET LENGTH.
#
# Research aid for trusted local images (the owner's HDD), not a test oracle:
# it bounds nothing beyond a visited set, so a malformed image can exhaust
# memory or raise. Offsets follow docs/qnx4-research.md and the draft
# fsspecs/QNX4_fsimg_spec.md sections 3 to 9.
import collections, mmap, struct, sys

path, base, size = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
f = open(path, 'rb')
m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
BS = 512
N = size // BS
C = collections.Counter
stats = collections.defaultdict(C)

def blk(b):
    assert 1 <= b <= N, b
    o = base + (b - 1) * BS
    return m[o:o + BS]

def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u16(b, o): return struct.unpack_from('<H', b, o)[0]

def inode(r):
    return dict(size=u32(r, 0x10), x0=u32(r, 0x14), xn=u32(r, 0x18),
                xblk=u32(r, 0x1c), times=struct.unpack_from('<4I', r, 0x20),
                nx=u16(r, 0x30), mode=u16(r, 0x32), uid=u16(r, 0x34),
                gid=u16(r, 0x36), nlink=u16(r, 0x38), zero=r[0x3a:0x3e],
                type=r[0x3e], st=r[0x3f])

owned = bytearray((N + 8) // 8)
owners = C()

def own(b, n, what):
    for x in range(b, b + n):
        if owned[x >> 3] >> (x & 7) & 1:
            stats['double-owned'][what] += 1
        owned[x >> 3] |= 1 << (x & 7)
    owners[what] += n

def extents(i, what):
    ex = []
    if i['x0'] or i['xn']:
        ex.append((i['x0'], i['xn']))
    xb, prev, nblocks, seen = i['xblk'], 0, 0, set()
    while xb:
        if xb in seen:
            stats['xblk-loop'][what] += 1
            break
        seen.add(xb)
        b = blk(xb)
        own(xb, 1, 'xblk')
        nblocks += 1
        used = b[8]
        e = [(u32(b, 16 + 8 * k), u32(b, 20 + 8 * k)) for k in range(used)]
        stats['xblk-shape'][(u32(b, 4) == prev, used <= 60,
                             u32(b, 12) == sum(n for _, n in e),
                             b[0x1f0:0x1f8] == b'IamXblk\0',
                             b[9:12] == b'\0\0\0',
                             (u32(b, 0x1f8), u32(b, 0x1fc)) ==
                             ((i['x0'], i['xn']) if prev == 0 else (0, 0)))] += 1
        ex.extend(e)
        prev, xb = xb, u32(b, 0)
    stats['extent-count-matches'][len(ex) == i['nx']] += 1
    stats['extent-blocks-per-object'][nblocks] += 1
    return ex

def claim(i, what):
    ex = extents(i, what)
    alloc = sum(n for _, n in ex)
    for s, n in ex:
        own(s, n, what)
    need = (i['size'] + BS - 1) // BS
    stats['alloc-vs-size'][(what, 'exact' if alloc == need else
                            'over' if alloc > need else 'short')] += 1
    return ex

def data(ex, nbytes):
    out = bytearray()
    for s, n in ex:
        for b in range(s, s + n):
            if len(out) >= nbytes:
                return bytes(out[:nbytes])
            out += blk(b)
    return bytes(out[:nbytes])

def record_stats(i, kind):
    stats['kind-mode'][(kind, oct(i['mode']))] += 1
    stats['status-type'][(kind, hex(i['st']), i['type'])] += 1
    stats['reserved-nonzero'][(kind, any(i['zero']))] += 1
    stats['times-equal'][(kind, len(set(i['times'])))] += 1
    stats['extents-per-file'][min(i['nx'], 200)] += 1
    stats['owner-max'][(kind, 'uid<=65534', i['uid'] <= 65534)] += 1
    stats['nlink'][(kind, i['nlink'])] += 1

def name_stats(raw, length_field):
    stats['name-len'][(length_field, len(raw))] += 1
    try:
        raw.decode('utf-8')
        stats['name-utf8'][True] += 1
    except UnicodeDecodeError:
        stats['name-utf8'][False] += 1
    stats['name-bytes'][('ctl' if any(c < 0x20 or c == 0x7f for c in raw)
                         else 'high' if any(c >= 0x80 for c in raw)
                         else 'ascii')] += 1

rb = blk(2)
special = [inode(rb[64 * k:64 * k + 64]) for k in range(5)]
for k, nm in enumerate(('root', '.inodes', '.boot', '.altboot',
                        'lost+found')):
    i = special[k]
    stats['rootblk'][(nm, rb[64 * k:64 * k + 16].rstrip(b'\0') == (
        b'/' if k == 0 else b''), oct(i['mode']), hex(i['st']), i['nlink'],
        i['size'], i['nx'])] += 1
stats['rootblk-label'][any(rb[0x1c0:0x200])] += 1
stats['rootblk-140-1bf-nonzero'][any(rb[0x140:0x1c0])] += 1
own(1, 1, 'loader'); own(2, 1, 'rootblk')
loader = blk(1)
stats['loader'][('zero' if not any(loader) else 'nonzero')] += 1

inodes_ex = claim(special[1], '.inodes')
islots = sum(n for _, n in inodes_ex) * 8
first = data(inodes_ex, 64)
stats['.inodes'][('signature', first[:16] == b'IamTHE.inodeFILE',
                  'slot0-rest', first[16:].hex())] += 1
used_slots = 0
inode_blocks = set()
for s, n in inodes_ex:
    for b in range(s, s + n):
        inode_blocks.add(b)
        r = blk(b)
        for k in range(8):
            if (b, k) != (inodes_ex[0][0], 0) and r[64 * k + 63] & 1:
                used_slots += 1
stats['.inodes'][('slots', islots, 'used', used_slots, 'bytes',
                  special[1]['size'])] += 1
seen_targets = C()
dirs_seen = set()
lfn = None

def walk(i, ident, parent_ident, depth):
    global lfn
    if ident in dirs_seen:
        stats['dir-reached-twice'][1] += 1
        return
    dirs_seen.add(ident)
    stats['depth'][depth] += 1
    ex = claim(i, 'dir')
    stats['dir-size'][(i['size'] % 512 == 0, i['size'] // 512)] += 1
    blocks = [b for s, n in ex for b in range(s, s + n)]
    nslots, subdirs = 0, []
    for k in range(i['size'] // 64):
        b, slot = blocks[k // 8], k % 8
        r = blk(b)[64 * slot:64 * slot + 64]
        st = r[63]
        if k < 2:
            # dot entries
            stats['dot'][(k, hex(st), r[:2] if k == 0 else r[:3],
                          r[2:7] == b'I\x03QNX' if k == 0 else None,
                          r[8] if k == 0 else None,
                          (u32(r, 0x30), r[0x34]) == ident if k == 0 else
                          (u32(r, 0x30), r[0x34]) == parent_ident)] += 1
            continue
        if not st & 0x09:
            if any(r):
                stats['free-slot'][('nonzero', hex(st))] += 1
            continue
        nslots += 1
        if st & 0x08:
            name = r[:48].split(b'\0')[0]
            tb, ts = u32(r, 0x30), r[0x34]
            lb = u32(r, 0x35)
            stats['link'][(hex(st), ts < 8, tb in inode_blocks or tb == 2,
                           lb != 0, any(r[0x39:0x3f]))] += 1
            t = inode(blk(tb)[64 * ts:64 * ts + 64])
            if lb:
                lr = blk(lb)
                stats['lfn'][(u32(lr, 0) == b, lr[4] == slot, hex(lr[5]),
                              lr[511])] += 1
                name = lr[6:511].split(b'\0')[0]
            name_stats(name, 'link')
            seen_targets[(tb, ts)] += 1
            if seen_targets[(tb, ts)] > 1:
                stats['multi-named'][1] += 1
                continue
            tid = (tb, ts)
        else:
            name = r[:16].split(b'\0')[0]
            name_stats(name, 'direct')
            t = inode(r)
            tid = (b, slot)
        kind = {0o040000: 'dir', 0o100000: 'reg', 0o120000: 'lnk',
                0o010000: 'fifo'}.get(t['mode'] & 0o170000, 'other')
        record_stats(t, kind)
        if depth == 0 and name == b'.longfilenames':
            lfn = t
        if depth == 0 and name in (b'.bitmap', b'.inodes', b'.boot',
                                   b'.altboot', b'.longfilenames',
                                   b'.diskroot', b'.bad_blks'):
            stats['root-system'][(name, oct(t['mode']), hex(st))] += 1
            if name in (b'.inodes', b'.boot', b'.altboot'):
                continue
        if kind == 'dir':
            subdirs.append((t, tid))
        else:
            claim(t, kind)
    stats['entries-per-dir'][min(nslots // 10 * 10, 1000)] += 1
    for t, tid in subdirs:
        walk(t, tid, ident, depth + 1)

lf = special[4]
walk(special[0], (2, 0), (2, 0), 0)
if lf['mode']:
    walk(lf, (2, 4), (2, 0), 1)
claim(special[2], '.boot'); claim(special[3], '.altboot')
# .bitmap was claimed by walk as 'reg'; find it again for the comparison.
root_ex = extents(special[0], 'x')
bm = None
for s, n in root_ex:
    for b in range(s, s + n):
        r = blk(b)
        for k in range(8):
            if r[64 * k:64 * k + 8] == b'.bitmap\0' and r[64 * k + 63] & 1:
                bm = inode(r[64 * k:64 * k + 64])
bex = extents(bm, 'bm')
M = bm['size']
stats['bitmap'][('size', M, 'ceil(N/8)', (N + 7) // 8, 'blocks',
                 sum(n for _, n in bex), 'ceil(N/4096)', (N + 4095) // 4096,
                 'at', bex[0][0])] += 1
bits = data(bex, sum(n for _, n in bex) * BS)
pad = all(bits[x >> 3] >> (x & 7) & 1 for x in range(N, 8 * M))
stats['bitmap'][('padding bits set', pad,
                 'bytes past size', tuple(sorted(set(bits[M:]))))] += 1
mismatch = C()
for x in range(N):
    a = bits[x >> 3] >> (x & 7) & 1
    o = owned[(x + 1) >> 3] >> ((x + 1) & 7) & 1
    if a != o:
        mismatch[('set-unowned' if a else 'owned-clear')] += 1
stats['bitmap-vs-reachable'][tuple(sorted(mismatch.items()))] += 1
stats['owned-blocks'] = owners
if lfn:
    stats['.longfilenames'][('size', lfn['size'], 'nx', lfn['nx'])] += 1
stats['clean'][hex(special[0]['st'])] += 1
for k in sorted(stats):
    print(k)
    for v, n in sorted(stats[k].items(), key=lambda kv: str(kv[0])):
        print('   %8d  %s' % (n, v))
