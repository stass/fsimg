#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
"""Dump the structure of an uncompressed QNX boot image or bare ImageFS.

Research aid, written independently of lib/; it trusts its input and decodes
no compression. Feed it a `dumpifs -u` copy or an oracle decode.

  ifsdump.py [-o OFFSET] FILE            structure-only survey (no names)
  ifsdump.py [-o OFFSET] --records FILE  one line per record, with names
  ifsdump.py [-o OFFSET] --manifest FILE path, kind, mode, uid, gid, mtime,
                                         size and POSIX cksum per entry

Names and manifests are member data: write them outside the source tree.
"""
import argparse
import collections
import struct
import sys

STARTUP_SIG = 0x00FF7EEB
KINDS = {0o040000: "dir", 0o100000: "file", 0o120000: "link",
         0o020000: "chr", 0o060000: "blk", 0o010000: "fifo",
         0o050000: "nam", 0o140000: "sock"}


def crc_table():
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) if c & 0x80000000 else c << 1
            c &= 0xFFFFFFFF
        t.append(c)
    return t


CRC = crc_table()


def posix_cksum(data):
    c = 0
    for b in data:
        c = ((c << 8) & 0xFFFFFFFF) ^ CRC[(c >> 24) ^ b]
    n = len(data)
    while n:
        c = ((c << 8) & 0xFFFFFFFF) ^ CRC[(c >> 24) ^ (n & 0xFF)]
        n >>= 8
    return (~c) & 0xFFFFFFFF


def wordsum(data, e):
    n = len(data) // 4
    return sum(struct.unpack_from(e + "%dI" % n, data, 0)) & 0xFFFFFFFF


def startup(d, off):
    sig, ver, f1, f2, hsz, mach = struct.unpack_from("<IHBBHH", d, off)
    if sig != STARTUP_SIG:
        return None
    names = ["startup_vaddr", "paddr_bias", "image_paddr", "ram_paddr",
             "ram_size", "startup_size", "stored_size", "imagefs_paddr",
             "imagefs_size"]
    vals = struct.unpack_from("<9I", d, off + 0x0C)
    s = dict(zip(names, vals))
    s.update(version=ver, flags1=f1, flags2=f2, header_size=hsz,
             machine=mach)
    s["preboot_size"], s["zero0"] = struct.unpack_from("<HH", d, off + 0x30)
    ss, st = s["startup_size"], s["stored_size"]
    s["startup_sum"] = wordsum(d[off:off + ss], "<")
    s["stored_sum"] = wordsum(d[off + ss:off + st], "<")
    s["tail_nonzero"] = sum(1 for b in d[off + 0x34:off + 0x100] if b)
    return s


def header(img):
    fl = img[7]
    e = ">" if fl & 1 else "<"
    isz, hds, doff = struct.unpack_from(e + "III", img, 8)
    h = dict(flags=fl, endian=e, image_size=isz, hdr_dir_size=hds,
             dir_offset=doff)
    h["boot_ino"] = list(struct.unpack_from(e + "4I", img, 0x14))
    h["script_ino"], h["chain_paddr"] = struct.unpack_from(e + "II", img, 0x24)
    h["spare"] = list(struct.unpack_from(e + "10I", img, 0x2C))
    h["mountflags"] = struct.unpack_from(e + "I", img, 0x54)[0]
    nul = img.index(b"\0", 0x58)
    h["mountpoint"] = img[0x58:nul]
    h["header_pad"] = img[nul + 1:doff]
    return h


def records(img, h):
    e = h["endian"]
    p = h["dir_offset"]
    end = h["hdr_dir_size"]
    out = []
    while p < end:
        if p + 2 > end:
            raise ValueError("record prefix crosses table end at %d" % p)
        size = struct.unpack_from(e + "H", img, p)[0]
        if size == 0:
            break
        xo, ino, mode, gid, uid, mt = struct.unpack_from(e + "HIIIII", img,
                                                         p + 2)
        r = dict(off=p, size=size, extattr=xo, ino=ino, mode=mode, gid=gid,
                 uid=uid, mtime=mt, kind=KINDS.get(mode & 0o170000, "?"))
        if r["kind"] == "file" or r["kind"] in ("chr", "blk", "fifo", "nam",
                                                "sock"):
            r["a"], r["b"] = struct.unpack_from(e + "II", img, p + 24)
            name0 = p + 32
        elif r["kind"] == "link":
            r["sym_offset"], r["sym_size"] = struct.unpack_from(e + "HH", img,
                                                                p + 24)
            name0 = p + 28
        else:
            name0 = p + 24
        nul = img.index(b"\0", name0)
        r["name"] = img[name0:nul]
        r["name_end"] = nul + 1
        if r["kind"] == "link":
            t0 = name0 + r["sym_offset"]
            r["target"] = img[t0:t0 + r["sym_size"]]
            r["target_nul"] = img[t0 + r["sym_size"]] if t0 + r["sym_size"] \
                < len(img) else None
            r["name_end"] = t0 + r["sym_size"] + 1
        r["tail"] = img[r["name_end"]:p + size]
        out.append(r)
        p += size
    return out, p


def survey(img, label, show=None):
    h = header(img)
    e = h["endian"]
    recs, stop = records(img, h)
    term = img[stop:stop + 4]
    isz = h["image_size"]
    print("%s imagefs flags=%#x image_size=%d decoded=%d hdr_dir_size=%d "
          "dir_offset=%d mountpoint_len=%d mountflags=%#x chain=%#x "
          "spare_nonzero=%d header_pad_nonzero=%d sum=%#x" % (
              label, h["flags"], isz, len(img), h["hdr_dir_size"],
              h["dir_offset"], len(h["mountpoint"]), h["mountflags"],
              h["chain_paddr"], sum(1 for x in h["spare"] if x),
              sum(1 for b in h["header_pad"] if b),
              wordsum(img[:isz], e)))
    print("  boot_ino=%s script_ino=%#x" % (
        [hex(x) for x in h["boot_ino"]], h["script_ino"]))
    print("  table stop=%d (hdr_dir_size-4=%d) terminator=%s" % (
        stop, h["hdr_dir_size"] - 4, term.hex()))
    kinds = collections.Counter(r["kind"] for r in recs)
    hi = collections.Counter(r["ino"] >> 28 for r in recs)
    pad = collections.Counter(r["size"] % 4 for r in recs)
    tails = collections.Counter(
        ("zero" if not any(r["tail"]) else "nonzero", len(r["tail"]))
        for r in recs)
    print("  records=%d kinds=%s ino_hi_nibble=%s size_mod4=%s extattr=%d "
          "zero_ino=%d" % (len(recs), dict(kinds),
                           {hex(k): v for k, v in hi.items()}, dict(pad),
                           sum(1 for r in recs if r["extattr"]),
                           sum(1 for r in recs if r["ino"] == 0)))
    print("  record tails (zero?, len)=%s" % dict(sorted(tails.items())))
    sl = collections.Counter()
    for r in recs:
        if r["kind"] == "link":
            sl[(r["sym_offset"] - len(r["name"]), r["target_nul"],
                len(r["tail"]))] += 1
    if sl:
        print("  links (sym_offset-namelen, byte after target, tail)=%s" %
              dict(sl))
    paths = [r["name"] for r in recs]
    depth = collections.Counter(p.count(b"/") + 1 if p else 0 for p in paths)
    lens = [len(p) for p in paths]
    comp = [len(c) for p in paths for c in p.split(b"/") if p]
    utf8 = 0
    for p in paths:
        try:
            p.decode("utf-8")
            utf8 += 1
        except UnicodeDecodeError:
            pass
    explicit = set(r["name"] for r in recs if r["kind"] == "dir")
    implied = set()
    for p in paths:
        parts = p.split(b"/")
        for i in range(1, len(parts)):
            implied.add(b"/".join(parts[:i]))
    print("  depth=%s path_len_max=%d comp_len_max=%d utf8=%d/%d "
          "explicit_dirs=%d implicit_dirs=%d root_record=%d dup_paths=%d "
          "leading_slash=%d dotdot=%d empty_comp=%d" % (
              dict(sorted(depth.items())), max(lens or [0]),
              max(comp or [0]), utf8, len(paths), len(explicit),
              len(implied - explicit), sum(1 for p in paths if p == b""),
              len(paths) - len(set(paths)),
              sum(1 for p in paths if p.startswith(b"/")),
              sum(1 for p in paths for c in p.split(b"/") if c in (b"..", b".")),
              sum(1 for p in paths if p and b"" in p.split(b"/"))))
    order = [r["name"] for r in recs]
    print("  table order sorted bytewise=%s" % (order == sorted(order)))
    inos = collections.Counter(r["ino"] for r in recs)
    low = collections.Counter(r["ino"] & 0x0FFFFFFF for r in recs)
    print("  dup_ino_words=%d dup_low28=%d ino_low_range=%s" % (
        sum(1 for v in inos.values() if v > 1),
        sum(1 for v in low.values() if v > 1),
        (min(low), max(low)) if low else None))
    for i, b in enumerate(h["boot_ino"] + [h["script_ino"]]):
        if b == 0:
            continue
        exact = [r for r in recs if r["ino"] == b]
        masked = [r for r in recs if r["ino"] & 0x0FFFFFFF == b & 0x0FFFFFFF]
        print("  ref[%s]=%#x exact=%d low28=%d kinds=%s ino_words=%s" % (
            "script" if i == 4 else i, b, len(exact), len(masked),
            [r["kind"] for r in masked], [hex(r["ino"]) for r in masked]))
    files = [r for r in recs if r["kind"] == "file"]
    data_lo = h["hdr_dir_size"]
    data_hi = isz - 4
    bad = [r for r in files if r["b"] and not (
        data_lo <= r["a"] and r["a"] + r["b"] <= data_hi)]
    zero = [r for r in files if r["b"] == 0]
    align = collections.Counter()
    for r in files:
        a = r["a"]
        align[(hex(r["ino"] >> 28),
               4096 if a % 4096 == 0 else 4 if a % 4 == 0 else 1)] += 1
    ranges = sorted((r["a"], r["a"] + r["b"]) for r in files if r["b"])
    shared = collections.Counter(ranges)
    overl = 0
    hi_end = 0
    for a, z in ranges:
        if a < hi_end:
            overl += 1
        hi_end = max(hi_end, z)
    gaps = []
    cur = data_lo
    for a, z in ranges:
        if a > cur:
            gaps.append((cur, a))
        cur = max(cur, z)
    if cur < data_hi:
        gaps.append((cur, data_hi))
    gapbytes = sum(z - a for a, z in gaps)
    gapnz = sum(1 for a, z in gaps for b in img[a:z] if b)
    gaplen = collections.Counter(z - a for a, z in gaps)
    print("  files=%d out_of_range=%d zero_len=%d zero_len_offsets=%s "
          "identical_ranges=%d overlapping=%d" % (
              len(files), len(bad), len(zero),
              dict(collections.Counter(
                  "data_lo" if r["a"] == data_lo else
                  "0" if r["a"] == 0 else "other" for r in zero)),
              sum(v - 1 for v in shared.values() if v > 1), overl))
    print("  first_data=%s data_lo=%d gaps=%d gap_bytes=%d gap_nonzero=%d "
          "gap_len_top=%s" % (ranges[0][0] if ranges else None, data_lo,
                              len(gaps), gapbytes, gapnz,
                              gaplen.most_common(6)))
    print("  (ino_hi, data_align)=%s" % dict(sorted(align.items())))
    other = [r for r in recs if r["kind"] not in ("dir", "file", "link")]
    for r in other:
        print("  special kind=%s mode=%#o a=%#x b=%#x size=%d" % (
            r["kind"], r["mode"], r["a"], r["b"], r["size"]))
    modes = collections.Counter((r["kind"], oct(r["mode"] & 0o7777))
                                for r in recs)
    print("  modes=%s" % dict(modes.most_common(12)))
    print("  uid=%s gid=%s mtime_zero=%d" % (
        dict(collections.Counter(r["uid"] for r in recs).most_common(4)),
        dict(collections.Counter(r["gid"] for r in recs).most_common(4)),
        sum(1 for r in recs if r["mtime"] == 0)))
    if show == "records":
        for r in recs:
            extra = ""
            if r["kind"] == "file":
                extra = " off=%#x len=%d" % (r["a"], r["b"])
            elif r["kind"] == "link":
                extra = " sym_offset=%d sym_size=%d -> %r" % (
                    r["sym_offset"], r["sym_size"], r["target"])
            elif "a" in r:
                extra = " a=%#x b=%#x" % (r["a"], r["b"])
            print("%6d %4d x=%d ino=%#010x mode=%#o uid=%d gid=%d "
                  "mtime=%#x %s %r%s tail=%s" % (
                      r["off"], r["size"], r["extattr"], r["ino"],
                      r["mode"], r["uid"], r["gid"], r["mtime"], r["kind"],
                      r["name"], extra, r["tail"].hex()))
    return h, recs


def manifest(img, h, recs):
    for r in recs:
        if r["kind"] == "file":
            data = img[r["a"]:r["a"] + r["b"]]
            val = "%d %d" % (posix_cksum(data), r["b"])
        elif r["kind"] == "link":
            val = r["target"].decode("latin-1")
        else:
            val = "-"
        sys.stdout.buffer.write(b"%s\t%s\t%o\t%d\t%d\t%d\t%s\n" % (
            r["name"], r["kind"].encode(), r["mode"], r["uid"], r["gid"],
            r["mtime"], val.encode("latin-1")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--offset", type=int, default=0)
    ap.add_argument("--records", action="store_true")
    ap.add_argument("--manifest", action="store_true")
    ap.add_argument("file")
    a = ap.parse_args()
    with open(a.file, "rb") as f:
        d = f.read()
    off = a.offset
    s = startup(d, off)
    if s is not None:
        print("startup @%d %s" % (off, " ".join(
            "%s=%#x" % (k, v) for k, v in s.items())))
        comp = s["flags1"] & 0x1C
        print("  ram_size-(startup_size+imagefs_size)=%d "
              "stored_end-file_end=%d" % (
                  s["ram_size"] - s["startup_size"] - s["imagefs_size"],
                  off + s["stored_size"] - len(d)))
        if comp:
            print("  compressed body (%#x); decode it first" % comp)
            return
        body = off + s["startup_size"]
    else:
        body = off
    img = d[body:]
    if img[:7] != b"imagefs":
        sys.exit("no imagefs at %d" % body)
    if a.manifest:
        h = header(img)
        recs, _ = records(img, h)
        manifest(img, h, recs)
        return
    survey(img, "@%d" % body, "records" if a.records else None)


if __name__ == "__main__":
    main()
