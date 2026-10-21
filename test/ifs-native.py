# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
"""Native ImageFS evidence for test/ifs-integration.sh, in three commands.

ifs-native.py compare FSIMG TEST_IFS IFSDUMP IMAGE...

  Compare fsimg's view of each bare ImageFS with an independent decoder.
  The research decoder tools/qnx/decode/ifsdump.py --manifest lists every
  record of the table in order, one line each of tab-separated fields: the
  path relative to the mountpoint, the root's empty; the kind (file, dir,
  link, fifo); the octal mode; uid; gid; mtime; and a file's POSIX cksum
  and size separated by a blank, a link's target, or "-". This script
  relies on that format and nothing else of the decoder. fsimg lists the
  image through `fsimg t --json`, which also gives the info counts, and
  `test-ifs --manifest` reads every file's and link's content through the
  library.

  The comparison derives fsimg's whole tree from the decoder's records
  alone, by the rules of specs/design-ifs.md, and compares both ways:

  * placement: the last record of a path decides it; earlier records of
    the path are shadowed, and so is every path below a non-directory;
  * implied parents: each proper prefix of a placed path that no record
    names is synthesized, and the root when no record names it; their
    number is the implicit_directories count, hidden ones included;
  * omission: a placed or synthesized node whose own name lies outside
    the name profile of lib/core/unicode.c while every ancestor's lies
    inside is omitted with its subtree, and the omitted count is the
    number of such subtree roots;
  * listing: fsimg lists exactly the placed and synthesized nodes no
    omission hides, a recorded one with its last record's kind,
    permissions, owner, time and size, and a file's cksum or a link's
    target, and a synthesized one with the implicit-directory values.

  A file that does not begin with an ImageFS, such as a boot image, is
  skipped and counted. An image fsimg refuses, exiting with status 1 and
  a corrupt or unsupported error record, is counted as refused; test-ifs
  --native checks the library's refusal of each capture against its own
  model. Any other failure of fsimg, of the decoder or of the library
  reading, such as a crash or a leak report, is a mismatch. Exit status 0
  means every image agreed.

  The decoder's manifest is line- and tab-separated, so a name or link
  target holding a tab or newline byte makes an image's comparison fail
  as a decoder-manifest mismatch.

ifs-native.py container FSIMG TEST_IFS IFSDUMP IMAGE DECODED

  Compare fsimg's view of the ImageFS inside a container file, such as an
  hbcifs image, with the decoder's manifest of DECODED, an oracle's decode
  of the container's payload: the listing and content comparison above,
  with fsimg and test-ifs reading IMAGE through its layers, and then an
  extraction. fsimg extracts IMAGE into a private temporary directory
  with -k, which skips the links safe extraction does not create and
  would also omit a name outside the extraction profile. Every placed
  directory and file must be extracted, so an omission is a mismatch,
  each file with the cksum and size of its last record, and nothing
  else. cksum(1) reads each extracted file on its standard input, so no
  name reaches a command line. Exit status 0 means both comparisons
  agreed.

ifs-native.py body IMAGE

  Print where the ImageFS of an image file lies, as "OFFSET LENGTH PATH",
  PATH the file's real path, since fsimg opens no symbolic link: at 0 in a
  bare ImageFS, or after the startup section of an uncompressed boot
  image, such as a `dumpifs -u` copy, whose little-endian startup header
  the qnxboot profile of specs/design-layers.md describes. Print
  "compressed" for a boot image with a compressed body and "none" for
  anything else.

Member names are data of the images and never printed or passed on a
command line: each line gives counts only, a mismatch is reported by
category and count, and an unexpected failure by its exception type alone.
"""

import collections
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

KINDS = {"file": "file", "directory": "dir", "symlink": "link", "fifo": "fifo"}
IMPLICIT_DIRECTORY = ("directory", 0o755, 0, 0, 0)
# The status codes of a refusal: FSIMG_ECORRUPT and FSIMG_EUNSUPPORTED.
REFUSALS = (-5, -6)
# The startup header: its signature, and the offsets it may start at, after
# no boot prefix or after the x86 BIOS loader.
STARTUP_SIGNATURE = b"\xeb\x7e\xff\x00"
STARTUP_PREFIXES = (0, 1088)
STARTUP_HEADER_BYTES = 256
COMPRESSION_BITS = 0x1C

Record = collections.namedtuple("Record", "kind mode uid gid mtime value")
# The tree fsimg derives from the records: last maps each recorded path to
# its last record; parents holds the synthesized paths, root included;
# shadowed and omitted are the counts info reports; listed holds the paths
# fsimg lists.
Tree = collections.namedtuple("Tree", "last parents shadowed omitted listed")
# The comparison's counts and the info details they must equal.
DETAILS = {"implicit": "implicit_directories", "shadowed": "shadowed",
           "omitted": "omitted"}


class Refused(Exception):
    """fsimg refused the image as corrupt or unsupported."""


class ImageFailure(Exception):
    """A comparison that could not run on one image. It carries only its
    category, never output bytes."""


def run(arguments, category, stdin=None):
    """The command's stdout; raises ImageFailure(category) when it fails.
    Its stderr is dropped: a diagnostic may quote a member name."""
    result = subprocess.run(arguments, stdin=stdin, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    if result.returncode != 0:
        raise ImageFailure(category)
    return result.stdout


def fields(line, count, category):
    """The tab-separated fields of line, which must number count."""
    parts = line.split(b"\t")
    if len(parts) != count:
        raise ImageFailure(category)
    return parts


def number(text, category, base=10):
    try:
        return int(text, base)
    except ValueError:
        raise ImageFailure(category) from None


def in_profile(name):
    """fsimg_name_in_qnx_profile: a name other than "", "." and "..", with
    no control byte, DEL or slash, in strict UTF-8 (no overlong form, no
    surrogate, nothing past U+10FFFF), as Python's decoder enforces."""
    if name in (b"", b".", b".."):
        return False
    if any(byte < 0x20 or byte in (0x2F, 0x7F) for byte in name):
        return False
    try:
        name.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return True


def oracle_records(ifsdump, image):
    """The decoder's records in table order, as (path, Record)."""
    category = "decoder manifest"
    output = run([sys.executable, "-I", ifsdump, "--manifest", image],
                 category)
    records = []
    for line in filter(None, output.split(b"\n")):
        path, kind, mode, uid, gid, mtime, value = fields(line, 7, category)
        records.append((path, Record(
            kind.decode("ascii", "replace"), number(mode, category, 8),
            number(uid, category), number(gid, category),
            number(mtime, category), value)))
    return records


def fsimg_view(fsimg, image):
    """fsimg's info details and listed entries, by relative path. Raises
    Refused when fsimg refuses the image as corrupt or unsupported, and
    ImageFailure when it fails any other way."""
    category = "fsimg listing"
    result = subprocess.run([fsimg, "t", "--json", "-f", image],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    try:
        lines = [json.loads(line) for line in result.stdout.splitlines()]
    except ValueError:
        raise ImageFailure(category) from None
    if result.returncode != 0:
        errors = [line for line in lines if line.get("type") == "error"]
        if result.returncode == 1 and errors and \
                errors[-1].get("code") in REFUSALS:
            raise Refused
        raise ImageFailure(category)
    info = [line for line in lines if line.get("type") == "info"]
    listed = [line for line in lines if line.get("type") == "entry"]
    entries = {line["path"].encode("utf-8")[1:]: line for line in listed}
    if len(info) != 1 or len(entries) != len(listed):
        raise ImageFailure(category)
    return ({name: int(value) for name, value in info[0]["details"].items()},
            entries)


def library_content(test_ifs, image):
    """Each file's (cksum, size) and each link's target bytes, by relative
    path, as test-ifs --manifest reads them."""
    category = "library content"
    output = run([test_ifs, "--manifest", image], category)
    content = {}
    for line in filter(None, output.split(b"\n")):
        if line.startswith(b"F\t"):
            _, path, crc, size = fields(line, 4, category)
            content[path[1:]] = (number(crc, category),
                                 number(size, category))
        elif line.startswith(b"L\t"):
            _, path, target = fields(line, 3, category)
            try:
                content[path[1:]] = bytes.fromhex(target.decode("ascii"))
            except ValueError:
                raise ImageFailure(category) from None
        else:
            raise ImageFailure(category)
    return content


def prefixes(path):
    """The proper prefixes of a non-root path, shortest first."""
    parts = path.split(b"/")
    return [b"/".join(parts[:depth]) for depth in range(1, len(parts))]


def below_non_directory(path, last):
    """Whether a proper prefix of path names a record that is not a
    directory, the last record of that prefix deciding."""
    return any(prefix in last and last[prefix].kind != "dir"
               for prefix in prefixes(path))


def expected_tree(records):
    """The Tree fsimg derives from records."""
    last = dict(records)
    placed = {path for path in last
              if not path or not below_non_directory(path, last)}
    shadowed = len(records) - len(placed)
    parents = set() if b"" in placed else {b""}
    for path in placed:
        if path:
            parents.update(prefix for prefix in prefixes(path)
                           if prefix not in last)
    listed = set()
    omitted = 0
    for path in (placed | parents) - {b""}:
        components = path.split(b"/")
        inside = [in_profile(component) for component in components]
        if all(inside[:-1]) and not inside[-1]:
            omitted += 1
        elif all(inside):
            listed.add(path)
    return Tree(last, parents, shadowed, omitted, listed)


def entry_values(entry):
    return (entry["kind"], entry["mode"], entry["uid"], entry["gid"],
            int(entry["modified"]["seconds"]))


def compare_entry(entry, expected, content, counts, mismatches):
    """Compare one listed entry the table records with its last record and
    the library's reading of its content."""
    path = entry["path"].encode("utf-8")[1:]
    kind, mode, uid, gid, mtime = entry_values(entry)
    if (KINDS.get(kind), mode, uid, gid, mtime) != (
            expected.kind, expected.mode & 0o7777, expected.uid,
            expected.gid, expected.mtime):
        mismatches["kind, mode, owner or time"] += 1
    if expected.kind == "file":
        crc_text, _, size_text = expected.value.partition(b" ")
        crc = number(crc_text, "decoder manifest")
        size = number(size_text, "decoder manifest")
        counts["files"] += 1
        if content.get(path) != (crc, size) or \
                int(entry["size_bytes"]) != size:
            mismatches["file size or bytes"] += 1
    elif expected.kind == "link":
        counts["links"] += 1
        if content.get(path) != expected.value:
            mismatches["link target"] += 1


def compare(fsimg, test_ifs, ifsdump, image, decoded=None):
    """Compare one image, whose ImageFS the decoder reads from decoded
    when it is in a container; returns (counts, mismatches) of category to
    count and the expected Tree. Raises Refused or ImageFailure as
    fsimg_view does."""
    details, entries = fsimg_view(fsimg, image)
    content = library_content(test_ifs, image)
    tree = expected_tree(oracle_records(ifsdump, decoded or image))
    counts = collections.Counter(implicit=len(tree.parents),
                                 shadowed=tree.shadowed,
                                 omitted=tree.omitted, entries=len(entries))
    mismatches = collections.Counter()

    mismatches["listed entries the table does not place"] = len(
        entries.keys() - tree.listed)
    mismatches["placed entries fsimg does not list"] = len(
        tree.listed - entries.keys())
    for path in entries.keys() & tree.listed:
        if path in tree.last:
            compare_entry(entries[path], tree.last[path], content, counts,
                          mismatches)
        elif entry_values(entries[path]) != IMPLICIT_DIRECTORY:
            mismatches["implicit directory values"] += 1
    for name, detail in DETAILS.items():
        if counts[name] != details.get(detail):
            mismatches[f"{name} count"] += 1
    return counts, +mismatches, tree


def file_cksum(path):
    """The POSIX cksum and size of a file, from cksum(1) reading it on its
    standard input."""
    category = "extracted content"
    with open(path, "rb") as stream:
        parts = run(["cksum"], category, stdin=stream).split()
    if len(parts) != 2:
        raise ImageFailure(category)
    return number(parts[0], category), number(parts[1], category)


def extracted_entries(root):
    """What lies under root by relative path, following no link: ("dir",),
    ("file", cksum, size) or ("other",)."""
    found = {}
    for directory, subdirectories, files in os.walk(os.fsencode(root)):
        for name in subdirectories + files:
            path = os.path.join(directory, name)
            mode = os.lstat(path).st_mode
            relative = os.path.relpath(path, os.fsencode(root))
            if stat.S_ISDIR(mode):
                found[relative] = ("dir",)
            elif stat.S_ISREG(mode):
                found[relative] = ("file",) + file_cksum(path)
            else:
                found[relative] = ("other",)
    return found


def expected_extraction(tree):
    """The directories and files a safe extraction of the tree creates."""
    expected = {}
    for path in tree.listed:
        record = tree.last.get(path)
        if record is None or record.kind == "dir":
            expected[path] = ("dir",)
        elif record.kind == "file":
            crc, _, size = record.value.partition(b" ")
            expected[path] = ("file", number(crc, "decoder manifest"),
                              number(size, "decoder manifest"))
    return expected


def compare_extraction(fsimg, image, tree):
    """Extract image with -k, which skips links and would omit a name
    outside the profile, and compare what lands with the tree; returns
    (counts, mismatches)."""
    root = tempfile.mkdtemp(prefix="fsimg-ifs-extract-")
    try:
        run([fsimg, "x", "-k", "-E", "-C", root, "-f", image],
            "fsimg extraction")
        found = extracted_entries(root)
    finally:
        shutil.rmtree(root)
    expected = expected_extraction(tree)
    mismatches = collections.Counter()
    mismatches["extracted entries the table does not place"] = len(
        found.keys() - expected.keys())
    mismatches["placed entries fsimg does not extract"] = len(
        expected.keys() - found.keys())
    mismatches["extracted kind, size or bytes"] = sum(
        found[path] != expected[path] for path in found.keys() & expected.keys())
    counts = collections.Counter(
        extracted_files=sum(entry[0] == "file" for entry in found.values()),
        extracted_directories=sum(entry[0] == "dir" for entry in found.values()))
    return counts, +mismatches


def compare_images(fsimg, test_ifs, ifsdump, images):
    """The compare command."""
    skipped = refused = failed = 0
    total = collections.Counter()
    for image in images:
        name = os.path.basename(image)
        with open(image, "rb") as stream:
            if stream.read(7) != b"imagefs":
                skipped += 1
                continue
        try:
            counts, mismatches, _ = compare(fsimg, test_ifs, ifsdump, image)
        except Refused:
            refused += 1
            print(f"{name}: refused by fsimg")
            continue
        except ImageFailure as failure:
            counts = collections.Counter()
            mismatches = collections.Counter({f"{failure} failure": 1})
        total.update(counts)
        print(f"{name}: {counts['entries']} entries, {counts['files']} "
              f"files' bytes, {counts['links']} link targets, "
              f"{counts['implicit']} implicit directories, "
              f"{counts['shadowed']} shadowed records, "
              f"{counts['omitted']} omitted subtrees")
        for category, count in sorted(mismatches.items()):
            print(f"{name}: MISMATCH {category}: {count}")
            failed += 1
    print(f"compare: {len(images) - skipped} images, {refused} refused, "
          f"{total['entries']} entries, {total['files']} files' bytes, "
          f"{total['links']} link targets, {failed} mismatches; "
          f"{skipped} files without an ImageFS at offset 0")
    return 1 if failed else 0


def compare_container(fsimg, test_ifs, ifsdump, image, decoded):
    """The container command."""
    name = os.path.basename(image)
    try:
        counts, mismatches, tree = compare(fsimg, test_ifs, ifsdump, image,
                                           decoded)
        extracted, extraction_mismatches = compare_extraction(fsimg, image,
                                                              tree)
    except Refused:
        print(f"{name}: refused by fsimg")
        return 1
    except ImageFailure as failure:
        print(f"{name}: MISMATCH {failure} failure: 1")
        return 1
    mismatches.update(extraction_mismatches)
    print(f"{name}: {counts['entries']} entries, {counts['files']} "
          f"files' bytes, {counts['links']} link targets, "
          f"{counts['implicit']} implicit directories, "
          f"{counts['shadowed']} shadowed records, "
          f"{counts['omitted']} omitted subtrees; extracted "
          f"{extracted['extracted_files']} files and "
          f"{extracted['extracted_directories']} directories")
    for category, count in sorted(mismatches.items()):
        print(f"{name}: MISMATCH {category}: {count}")
    return 1 if mismatches else 0


def body_report(image):
    """The body command's answer for image."""
    with open(image, "rb") as stream:
        prefix = stream.read(max(STARTUP_PREFIXES) + STARTUP_HEADER_BYTES)
        size = os.fstat(stream.fileno()).st_size
        if prefix[:7] == b"imagefs":
            return f"0 {size} {os.path.realpath(image)}"
        for start in STARTUP_PREFIXES:
            header = prefix[start:start + STARTUP_HEADER_BYTES]
            if len(header) < STARTUP_HEADER_BYTES or \
                    header[:4] != STARTUP_SIGNATURE:
                continue
            if header[6] & COMPRESSION_BITS:
                return "compressed"
            startup_size, = struct.unpack_from("<I", header, 0x20)
            imagefs_size, = struct.unpack_from("<I", header, 0x2C)
            offset = start + startup_size
            stream.seek(offset)
            if offset + imagefs_size > size or stream.read(7) != b"imagefs":
                return "none"
            return f"{offset} {imagefs_size} {os.path.realpath(image)}"
    return "none"


def main(arguments):
    if len(arguments) >= 5 and arguments[0] == "compare":
        return compare_images(arguments[1], arguments[2], arguments[3],
                              arguments[4:])
    if len(arguments) == 6 and arguments[0] == "container":
        return compare_container(*arguments[1:])
    if len(arguments) == 2 and arguments[0] == "body":
        print(body_report(arguments[1]))
        return 0
    sys.exit("usage: ifs-native.py compare FSIMG TEST_IFS IFSDUMP IMAGE...\n"
             "       ifs-native.py container FSIMG TEST_IFS IFSDUMP IMAGE "
             "DECODED\n"
             "       ifs-native.py body IMAGE")


if __name__ == "__main__":
    # The one boundary: an unexpected failure is reported by its type, as a
    # traceback could quote member bytes.
    try:
        sys.exit(main(sys.argv[1:]))
    except Exception as failure:  # pylint: disable=broad-except
        sys.exit(f"ifs-native.py: {type(failure).__name__}")
