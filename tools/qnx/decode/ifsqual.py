#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
"""Pack and judge the native ImageFS qualification of fsimg writer output.

Research aid. Usage:
  ifsqual.py pack CASEDIR OUT.img
  ifsqual.py compare CASEDIR RECORD.txt

CASEDIR holds NAME.ifs and NAME.manifest for every case, as
`test-ifs-write --create CASEDIR` writes them. pack writes the disk
tools/qnx/guest/qualifyifs.sh reads: an index, then for each case its image,
its directory list, a mkifs buildfile and the contents of its files, each
at a 512-byte sector. It prints the index length the guest script takes.

compare reads the capture record of that run and checks, for each case:
  1. dumpifs -v lists every record of the fsimg image in table order with
     the manifest's offset, size, name, link target, owner, mode, inode and
     time, the header geometry, mountpoint and trailer word, and prints no
     error beyond the "Early end reading directory" line it prints for
     native images; dumpifs -m gives each file's MD5;
  2. dumpifs -x extracts every regular file with the manifest's size,
     cksum, mode and time, exactly as it extracts the native image of the
     same tree, apart from times (mkifs -nn stores zero);
  3. mkifs -nn builds that native image, and its dumpifs -v and -m agree
     with the fsimg image's in every name, kind, size, owner, mode and MD5;
     record order, offsets, inodes and times are free choices or the -nn
     setting, and mkifs sets the 01000 bit on every file and link with an
     execute bit, which step 2 sees in the native extraction too.
It prints counts per case and step, and the differences of a failing step.
"""
import calendar
import hashlib
import os
import re
import sys
import time

SECTOR = 512
EARLY_END = b"dumpifs: Early end reading directory"
T_TYPE, T_DIR, T_FILE, T_LINK, T_FIFO = 0o170000, 0o040000, 0o100000, \
    0o120000, 0o010000


def manifest(path):
    """The header and the records of a manifest, in table order."""
    header, entries, sums = None, [], {}
    with open(path, "rb") as f:
        for line in f.read().split(b"\n"):
            if not line:
                continue
            fields = line.split(b"\t")
            if fields[0] == b"H":
                header = {
                    "flags": int(fields[1], 16),
                    "dir_offset": int(fields[2], 16),
                    "hdr_dir_size": int(fields[3], 16),
                    "image_size": int(fields[4], 16),
                    "trailer": int(fields[5], 16),
                    "mountpoint": fields[6],
                }
            elif fields[0] == b"E":
                entries.append({
                    "path": fields[1],
                    "mode": int(fields[2], 8),
                    "uid": int(fields[3]),
                    "gid": int(fields[4]),
                    "mtime": int(fields[5], 16),
                    "ino": int(fields[6]),
                    "data": int(fields[7], 16),
                    "size": int(fields[8]),
                    "target": fields[9] if len(fields) > 9 else None,
                })
            elif fields[0] == b"C":
                sums[fields[1]] = (int(fields[2]), int(fields[3]))
            else:
                sys.exit("bad manifest line in %s" % path)
    return header, entries, sums


def cases(casedir):
    names = sorted(n[:-len(".manifest")] for n in os.listdir(casedir)
                   if n.endswith(".manifest"))
    order = ["one", "names", "limits", "kinds", "metadata", "data", "empty",
             "mount-empty", "mount-long", "big-endian"]
    return sorted(names, key=lambda n: (order.index(n) if n in order
                                        else len(order), n))


def kind(e):
    return e["mode"] & T_TYPE


def cksum(data):
    crc = 0
    for b in data + length_bytes(len(data)):
        crc ^= b << 24
        for _ in range(8):
            crc = (crc << 1 ^ 0x04c11db7) if crc & 0x80000000 else crc << 1
            crc &= 0xffffffff
    return ~crc & 0xffffffff


def length_bytes(n):
    out = b""
    while n:
        out += bytes([n & 0xff])
        n >>= 8
    return out


# pack

def buildfile(header, entries):
    """mkifs lines for the same tree: every directory named with type=dir,
    every owner and mode given, file contents read from the members the
    guest copies into /tmp/s10q/c."""
    lines = []
    if header["flags"] == 5:
        lines.append(b"[+bigendian]")
    # Without mount=, mkifs stores an empty mountpoint; [mount=/] stores /.
    lines.append(b"[mount=" + header["mountpoint"] + b"]")
    k = 0
    for e in entries[1:]:
        owner = b"uid=%d gid=%d" % (e["uid"], e["gid"])
        perms = e["mode"] & 0o7777
        name = b'"' + e["path"] + b'"'
        if kind(e) == T_DIR:
            lines.append(b"[type=dir dperms=%04o %s]%s" % (perms, owner, name))
        elif kind(e) == T_FILE:
            lines.append(b"[type=file perms=%04o %s]%s=/tmp/s10q/c/%d" %
                         (perms, owner, name, k))
            k += 1
        elif kind(e) == T_LINK:
            lines.append(b'[type=link perms=%04o %s]%s="%s"' %
                         (perms, owner, name, e["target"]))
        elif kind(e) == T_FIFO:
            lines.append(b"[type=fifo perms=%04o %s]%s" % (perms, owner, name))
        else:
            sys.exit("unexpected kind")
    return b"\n".join(lines) + b"\n"


def members(casedir, name):
    image = open(os.path.join(casedir, name + ".ifs"), "rb").read()
    header, entries, _ = manifest(os.path.join(casedir, name + ".manifest"))
    out = [(name + ".ifs", image)]
    dirs = [e["path"] for e in entries[1:] if kind(e) == T_DIR]
    out.append((name + ".dirs", b"".join(d + b"\n" for d in dirs)))
    out.append((name + ".bld", buildfile(header, entries)))
    files = [e for e in entries if kind(e) == T_FILE]
    for k, e in enumerate(files):
        out.append(("%s.%d" % (name, k), image[e["data"]:e["data"] + e["size"]]))
    return out


def pack(casedir, out):
    names = cases(casedir)
    stored = [m for n in names for m in members(casedir, n)]
    index_sectors = 1
    while True:
        sector, lines = index_sectors, [b"case %s" % n.encode() for n in names]
        layout = []
        for member, data in stored:
            lines.append(b"member %s %d %d %d" % (member.encode(), sector,
                                                  len(data), cksum(data)))
            layout.append((sector, data))
            sector += (len(data) + SECTOR - 1) // SECTOR
        index = b"\n".join(lines) + b"\n"
        if len(index) <= index_sectors * SECTOR:
            break
        index_sectors = (len(index) + SECTOR - 1) // SECTOR
    disk = bytearray(sector * SECTOR)
    disk[:len(index)] = index
    for at, data in layout:
        disk[at * SECTOR:at * SECTOR + len(data)] = data
    with open(out, "wb") as f:
        f.write(disk)
    print("%d cases, %d members, %d bytes; index %d bytes, cksum %d" %
          (len(names), len(stored), len(disk), len(index), cksum(index)))
    print('capture.sh -i %s NAME "sh /tmp/qualifyifs.sh %d"' %
          (out, len(index)))


# compare

def sections(record):
    """{case: {label: [lines]}} and the record's top-level sections."""
    out, top, case, label = {}, {}, None, None
    body = record.split(b"---- output ----\n", 1)[1]
    for line in body.split(b"\n"):
        if line.startswith(b"=== case "):
            case, label = line[9:].decode(), None
            out[case] = {}
        elif line.startswith(b"=== "):
            case, label = None, line[4:].decode()
            top[label] = []
        elif line.startswith(b"--- ") and case is not None:
            label = line[4:].decode()
            out[case][label] = []
        elif case is not None and label is not None:
            out[case][label].append(line)
        elif case is None and label is not None:
            top[label].append(line)
    return out, top


ENTRY = re.compile(rb"^ *(----|[0-9a-f]+) +(----|[0-9a-f]+)  (.*)$")
META = re.compile(rb"^ +gid=(-?\d+) uid=(-?\d+) mode=([0-7]+) ino=(\d+) "
                  rb"mtime=([0-9a-f]+)$")
FLAGS = re.compile(rb"^ +flags=0x([0-9a-f]+) mntflg=([0-9a-f]+)$")
SUMS = re.compile(rb"^Checksums: image=0x([0-9a-f]+)$")
STATUS = re.compile(rb"^status (\d+)$")
MD5 = re.compile(rb"^ *([0-9a-f]+) +([0-9a-f]+)  (.*) ([0-9a-f]{32})$")


def hexfield(text):
    return None if text == b"----" else int(text, 16)


def listing(lines):
    """A dumpifs -v listing: header fields, records, trailer word, status
    and any line it does not explain."""
    out = {"records": [], "unexpected": [], "early_end": 0, "status": None,
           "trailer": None, "header": None, "directory": None,
           "mountpoint": None, "flags": None}
    lines = [l for l in lines if l != b"   Offset     Size  Name"]
    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        if line == EARLY_END:
            out["early_end"] += 1
            continue
        if STATUS.match(line):
            out["status"] = int(STATUS.match(line).group(1))
            continue
        if SUMS.match(line):
            out["trailer"] = int(SUMS.match(line).group(1), 16)
            continue
        if FLAGS.match(line):
            out["flags"] = (int(FLAGS.match(line).group(1), 16),
                            int(FLAGS.match(line).group(2), 16))
            continue
        m = ENTRY.match(line)
        if m is None:
            if line:
                out["unexpected"].append(line)
            continue
        offset, size, name = hexfield(m.group(1)), hexfield(m.group(2)), \
            m.group(3)
        if name.startswith(b"Image-header"):
            out["header"] = (offset, size)
            # dumpifs names a nonempty mountpoint only.
            rest = name[len(b"Image-header"):]
            out["mountpoint"] = rest[len(b" mountpoint="):] \
                if rest.startswith(b" mountpoint=") else rest
            continue
        if name == b"Image-directory":
            out["directory"] = (offset, size)
            continue
        # Skip the early-end line where stderr interleaves it.
        while i < len(lines) and lines[i] == EARLY_END:
            out["early_end"] += 1
            i += 1
        meta = META.match(lines[i]) if i < len(lines) else None
        if meta is None:
            out["unexpected"].append(line)
            continue
        i += 1
        out["records"].append((offset, size, name,
                               int(meta.group(1)) & 0xffffffff,
                               int(meta.group(2)) & 0xffffffff,
                               int(meta.group(3), 8), int(meta.group(4)),
                               int(meta.group(5), 16)))
    return out


def md5s(lines):
    out = {"files": [], "unexpected": [], "status": None}
    for line in lines:
        if line == EARLY_END or not line:
            continue
        if STATUS.match(line):
            out["status"] = int(STATUS.match(line).group(1))
            continue
        m = MD5.match(line)
        if m is None:
            out["unexpected"].append(line)
            continue
        out["files"].append((int(m.group(1), 16), int(m.group(2), 16),
                             m.group(3), m.group(4).decode()))
    return out


# dumpifs reads each record into a 1024-byte buffer, so it shows a path,
# and a link's target after the path's NUL, only as far as the record's
# first 1024 bytes; past them it prints unrelated memory, for native images
# alike. It shows at most 512 bytes of the mountpoint.
RECORD_BUFFER = 1024
PATH_AT = {T_DIR: 0x18, T_FILE: 0x20, T_LINK: 0x1c, T_FIFO: 0x20}
MOUNTPOINT_SHOWN = 512
# The shortest name text any record kind shows whole.
NAME_SHOWN = RECORD_BUFFER - 0x20


def name_field(e):
    """The name text dumpifs prints for a record and whether all of it lies
    in the record buffer; if not, the text is the part that does."""
    if e["path"] == b"":
        return b"Root-dirent", True
    window = RECORD_BUFFER - PATH_AT[kind(e)]
    path = e["path"]
    if len(path) >= window:
        return path[:window], False
    if kind(e) == T_LINK:
        shown = window - len(path) - 1
        target = e["target"]
        if len(target) >= shown:
            return path + b" -> " + target[:shown], False
        return path + b" -> " + target, True
    if kind(e) == T_FIFO:
        return path + b" dev=0 rdev=0", True
    return path, True


def name_matches(listed, e):
    text, whole = name_field(e)
    return listed == text if whole else listed.startswith(text)


def record_matches(listed, e):
    offset, size, name, gid, uid, mode, ino, mtime = listed
    return (offset == (e["data"] if kind(e) == T_FILE else None) and
            size == (e["size"] if kind(e) in (T_FILE, T_LINK) else None) and
            name_matches(name, e) and
            (gid, uid, mode, ino, mtime) == (e["gid"], e["uid"],
                                             e["mode"] & 0o7777, e["ino"],
                                             e["mtime"]))


def step1(header, entries, image, v, m):
    """dumpifs -v and -m of the fsimg image against the manifest."""
    problems = []
    got = listing(v)
    if got["status"] != 0:
        problems.append("dumpifs -v status %s" % got["status"])
    if got["unexpected"]:
        problems.append("unexplained lines: %r" % got["unexpected"][:3])
    if got["header"] != (0, 0x5c):
        problems.append("header %r" % (got["header"],))
    if got["flags"] != (header["flags"], 0):
        problems.append("flags %r" % (got["flags"],))
    if got["mountpoint"] != header["mountpoint"][:MOUNTPOINT_SHOWN]:
        problems.append("mountpoint %r" % got["mountpoint"][:40])
    if got["directory"] != (header["dir_offset"],
                            header["hdr_dir_size"] - header["dir_offset"]):
        problems.append("directory %r" % (got["directory"],))
    # dumpifs prints the trailer word little-endian in either byte order.
    if got["trailer"] != int.from_bytes(image[-4:], "little"):
        problems.append("trailer %r" % got["trailer"])
    if got["early_end"] > 1:
        problems.append("%d early-end lines" % got["early_end"])
    bad = [i for i, (a, e) in enumerate(zip(got["records"], entries))
           if not record_matches(a, e)]
    if len(got["records"]) != len(entries) or bad:
        problems.append("records: %d listed, %d expected, %d differ, first "
                        "%r" % (len(got["records"]), len(entries), len(bad),
                                [got["records"][i] for i in bad[:1]]))
    sums = md5s(m)
    if sums["status"] != 0 or sums["unexpected"]:
        problems.append("dumpifs -m status %s, %r" % (sums["status"],
                                                      sums["unexpected"][:3]))
    files = [e for e in entries if kind(e) == T_FILE]
    if len(sums["files"]) != len(files) or not all(
            (offset, size, digest) == (e["data"], e["size"], hashlib.md5(
                image[e["data"]:e["data"] + e["size"]]).hexdigest()) and
            name_matches(name, e)
            for (offset, size, name, digest), e in zip(sums["files"], files)):
        problems.append("dumpifs -m differs")
    cut = sum(not name_field(e)[1] for e in entries)
    return problems, "%d records%s, %d files" % (
        len(got["records"]),
        " (%d names past the record buffer)" % cut if cut else "",
        len(sums["files"]))


LS = re.compile(rb"^([-dlpn][-rwxsStT]{9}) +(\d+) +(\d+) +(\d+) +(\d+) "
                rb"(\w{3} [ \d]\d (?: \d{4}|\d\d:\d\d)) (.*)$")
CKSUM = re.compile(rb"^ *(\d+) +(\d+) (.*)$")


def tree(lines):
    """The extraction status, ls -ldn lines by path and cksums by path."""
    out = {"ls": {}, "sum": {}, "unexpected": []}
    for line in lines:
        m = LS.match(line)
        if m:
            path = m.group(7)
            out["ls"][path[2:] if path.startswith(b"./") else path] = {
                "mode": m.group(1), "uid": int(m.group(3)),
                "gid": int(m.group(4)), "size": int(m.group(5)),
                "date": m.group(6)}
            continue
        m = CKSUM.match(line)
        if m:
            path = m.group(3)
            out["sum"][path[2:] if path.startswith(b"./") else path] = \
                (int(m.group(1)), int(m.group(2)))
            continue
        if line:
            out["unexpected"].append(line)
    return out


def extraction_status(lines):
    statuses = [int(STATUS.match(l).group(1)) for l in lines
                if STATUS.match(l)]
    messages = [l for l in lines if l and not STATUS.match(l) and
                l != EARLY_END]
    return (statuses[0] if statuses else None), messages


def mode_string(mode):
    kinds = {T_DIR: "d", T_LINK: "l", T_FIFO: "p", T_FILE: "-"}
    out = list(kinds[mode & T_TYPE] + "".join(
        c if mode & (0o400 >> i) else "-" for i, c in enumerate("rwxrwxrwx")))
    for bit, at, on, off in ((0o4000, 3, "s", "S"), (0o2000, 6, "s", "S"),
                             (0o1000, 9, "t", "T")):
        if mode & bit:
            out[at] = on if out[at] == "x" else off
    return "".join(out).encode()


def dates(seconds):
    """Both forms ls -l may print for a UTC time."""
    t = time.gmtime(seconds)
    month = time.strftime("%b", t).encode()
    day = b"%02d" % t.tm_mday
    return {month + b" " + day + b"  %d" % t.tm_year,
            month + b" " + day + b" %02d:%02d" % (t.tm_hour, t.tm_min)}


def sticky_free(mode):
    """An ls mode string without the 01000 bit mkifs adds to an executable."""
    if mode[:1] == b"-" and mode[9:10] == b"t":
        return mode[:9] + b"x"
    return mode


def step2(entries, sums, x, t, native_x, native_t):
    """The extracted tree against the manifest and the native extraction."""
    problems = []
    status, messages = extraction_status(x)
    got = tree(t)
    files = [e for e in entries if kind(e) == T_FILE]
    extracted = [e for e in files if e["path"] in got["sum"]]
    if status == 0:
        if messages:
            problems.append("extraction messages %r" % messages[:2])
        if len(extracted) != len(files):
            problems.append("%d of %d files extracted" % (len(extracted),
                                                          len(files)))
    for e in extracted:
        ls = got["ls"].get(e["path"])
        if got["sum"][e["path"]] != sums[e["path"]]:
            problems.append("cksum of %r" % e["path"][:40])
        if ls is None or ls["mode"] != mode_string(e["mode"]) or \
                ls["size"] != e["size"] or ls["date"] not in dates(e["mtime"]):
            problems.append("ls of %r: %r" % (e["path"][:40], ls))
    regular = [p for p, ls in got["ls"].items() if ls["mode"][:1] == b"-"]
    if len(regular) != len(extracted):
        problems.append("%d regular files in the tree" % len(regular))
    # The native image of the same tree extracts alike, times aside.
    nstatus, nmessages = extraction_status(native_x)
    ngot = tree(native_t)
    if (status == 0) != (nstatus == 0):
        problems.append("status %s, native %s" % (status, nstatus))

    def strip(g):
        return ({p: dict(v, date=None, mode=sticky_free(v["mode"]))
                 for p, v in g["ls"].items()}, g["sum"])
    if strip(got) != strip(ngot):
        problems.append("the native extraction differs")
    note = "status %d, %d of %d files" % (status, len(extracted), len(files))
    if status != 0:
        note += " (%s)" % (b" ".join(messages)[:60].decode(errors="replace")
                           if messages else "native status %s" % nstatus)
    return problems, note


def step3(build, v, m, nv, nm):
    """The native image's listing against the fsimg image's."""
    problems = []
    status, messages = extraction_status(build)
    if status != 0 or messages:
        return ["mkifs status %s %r" % (status, messages[:2])], "no image"
    mine, native = listing(v), listing(nv)
    if native["status"] != 0 or native["unexpected"]:
        problems.append("native listing status %s %r" %
                        (native["status"], native["unexpected"][:2]))

    def shape(l):
        """Records as sorted (name, kind, size, owner, mode), names cut to
        the part every record kind shows."""
        out = []
        for offset, size, name, gid, uid, mode, ino, mtime in l["records"]:
            cls = "f" if offset is not None else (
                "l" if size is not None else "d")
            if cls != "d" and mode & 0o111:
                mode &= ~0o1000
            out.append((name[:NAME_SHOWN], cls, size, gid, uid, mode))
        return sorted(out), l["mountpoint"], l["flags"], l["early_end"] > 0
    if shape(mine) != shape(native):
        a, b = shape(mine), shape(native)
        diff = [(x, y) for x, y in zip(a[0], b[0]) if x != y]
        problems.append("listings differ: %d and %d records, first %r; "
                        "header %r" % (len(a[0]), len(b[0]), diff[:2],
                                       (a[1:] == b[1:])))
    a = sorted((p[:NAME_SHOWN], s) for _, _, p, s in md5s(m)["files"])
    b = sorted((p[:NAME_SHOWN], s) for _, _, p, s in md5s(nm)["files"])
    if a != b or md5s(nm)["status"] != 0:
        problems.append("dumpifs -m differs from the native image's")
    return problems, "%d records, %d files" % (len(native["records"]), len(b))


def compare(casedir, record_path):
    record = open(record_path, "rb").read()
    found, top = sections(record)
    failed = 0
    want_members = {}
    for name in cases(casedir):
        for member, data in members(casedir, name):
            want_members[member] = cksum(data)
    got_members = {}
    for line in top.get("members", []):
        parts = line.split()
        if len(parts) >= 3:
            got_members[parts[0].decode()] = int(parts[1])
    if got_members != want_members:
        print("members: %d of %d arrived intact" % (
            sum(got_members.get(k) == v for k, v in want_members.items()),
            len(want_members)))
        failed += 1
    else:
        print("members: all %d arrived intact" % len(want_members))
    for name in cases(casedir):
        image = open(os.path.join(casedir, name + ".ifs"), "rb").read()
        header, entries, sums = manifest(os.path.join(casedir,
                                                      name + ".manifest"))
        s = found.get(name)
        if s is None:
            print("%s: not run" % name)
            failed += 1
            continue
        results = [
            ("1 list", step1(header, entries, image, s["dumpifs-v fsimg"],
                             s["dumpifs-m fsimg"])),
            ("2 extract", step2(entries, sums, s["extract fsimg"],
                                s["tree fsimg"], s.get("extract native", []),
                                s.get("tree native", []))),
            ("3 mkifs", step3(s["mkifs"], s["dumpifs-v fsimg"],
                              s["dumpifs-m fsimg"],
                              s.get("dumpifs-v native", []),
                              s.get("dumpifs-m native", []))),
        ]
        for step, (problems, note) in results:
            print("%s step %s: %s; %s" % (name, step,
                                          "FAIL" if problems else "pass",
                                          note))
            for p in problems:
                print("    " + p)
            failed += bool(problems)
    print("%s" % ("FAIL: %d checks failed" % failed if failed else
                  "every step passed"))
    return 1 if failed else 0


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "pack":
        pack(sys.argv[2], sys.argv[3])
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "compare":
        return compare(sys.argv[2], sys.argv[3])
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
