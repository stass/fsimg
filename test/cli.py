# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
"""Exercise the installed command contract through independent JSON decoding."""

import base64
import errno
import gzip
import hashlib
import json
import os
import pty
import random
from pathlib import Path
import re
import resource
import shutil
import signal
import socket
import string
import struct
import subprocess
import sys
import tempfile
import time
import uuid
import zlib


def invoke(executable, root, arguments, expected=0, **kwargs):
    """Run the command in root; expected is its exit status, or None for any."""
    result = subprocess.run(
        [str(executable), *arguments], cwd=root, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=30, **kwargs,
    )
    assert expected is None or result.returncode == expected, \
        (arguments, result.returncode, result.stderr)
    return result


def records(result):
    decoded = [json.loads(line) for line in result.stdout.splitlines()]
    assert decoded and all(record["schema"] == 1 for record in decoded)
    return decoded


def completed(result):
    decoded = records(result)
    assert decoded[-1]["type"] == "summary" and decoded[-1]["complete"] is True
    assert not any(record["type"] == "error" for record in decoded)
    return decoded


def failed(result):
    decoded = records(result)
    assert decoded[-1]["type"] == "error" and decoded[-1]["complete"] is False
    assert not any(record["type"] == "summary" for record in decoded)
    return decoded[-1]


def same_bytes(first, second):
    with first.open("rb") as left, second.open("rb") as right:
        while True:
            data = left.read(65536)
            assert data == right.read(65536)
            if not data:
                return


def listing(executable, root, image):
    return invoke(executable, root, ["tf", image]).stdout.decode().splitlines()


# One t -v line: the kind and nine permission characters, the size or '-'
# right-aligned in 12 columns, the modification time or '-' padded to 19
# columns, and the quoted name with '@' after a symbolic link and '|' after
# a FIFO.
LONG_LISTING = re.compile(
    r'([-dlp?])([-rwx?]{9}) ([ 0-9-]{12}) '
    r'(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d|- {18}) (".*")([@|]?)')


def long_listing(result):
    """Map each quoted name to its (kind, permissions, size, time) columns."""
    rows = {}
    for line in result.stdout.decode().splitlines():
        match = LONG_LISTING.fullmatch(line)
        assert match, line
        kind, permissions, size, modified, name, marker = match.groups()
        assert re.fullmatch(r" *(\d+|-)", size), line
        assert marker == {"l": "@", "p": "|"}.get(kind, ""), line
        rows[name] = (kind, permissions, size.strip(), modified.strip())
    return rows


def private_directory(root, name, mode=0o700):
    path = root / name
    path.mkdir()
    path.chmod(mode)
    return path


def test_formats(executable, root):
    decoded = completed(invoke(executable, root, ["--formats", "--json"]))
    assert decoded[-1]["command"] == "formats"
    formats = filesystems(decoded)
    assert set(formats) == {"fat12", "fat16", "fat32", "exfat", "qnx6", "qnx4"}
    layers = {record["name"]: record for record in decoded
              if record["type"] == "format" and record["kind"] != "filesystem"}
    assert {name: record["kind"] for name, record in layers.items()} == {
        "gpt": "scheme", "mbr": "scheme", "dmg": "container", "vhd": "container",
        "simg": "container", "gzip": "compression", "zstd": "compression",
        "xz": "compression"}
    assert layers["gpt"]["settings"] == ["sector", "disk", "type", "name", "disk-guid", "guid"]
    assert layers["mbr"]["settings"] == ["disk", "type", "active", "id"]
    assert layers["dmg"]["settings"] == ["compress", "level", "chunk", "uuid"]
    assert layers["simg"]["settings"] == ["block", "crc"]
    assert layers["gzip"]["settings"] == ["level"]
    assert layers["gpt"]["capabilities"] == 7 and layers["vhd"]["capabilities"] == 7
    assert layers["gzip"]["capabilities"] == 7 and layers["zstd"]["suffix"] == ".zst"
    assert layers["gzip"]["codec_version"] == layers["dmg"]["codec_version"]
    assert {name: record["codecs"] for name, record in layers.items()} == {
        "gpt": None, "mbr": None, "dmg": "raw,zero,zlib", "vhd": "raw",
        "simg": "raw,zero,fill", "gzip": None, "zstd": None, "xz": None}
    assert re.fullmatch(r"zlib [0-9][0-9A-Za-z.-]*", layers["dmg"]["codec_version"])
    assert layers["vhd"]["codec_version"] is None
    assert layers["vhd"]["suffix"] == ".vhd" and layers["gpt"]["suffix"] is None
    assert all(record["capabilities"] == 7 for record in formats.values())
    assert formats["exfat"]["write_cluster_bytes"][-1] == 32 * 1024 * 1024
    assert isinstance(formats["exfat"]["max_file_bytes"], str)
    capabilities = next(record for record in decoded if record["type"] == "capabilities")
    assert capabilities["sandbox_runtime"] in {"unavailable", "not-probed"}
    assert capabilities["source_device_open_exclusion"] is False
    assert b"usage: fsimg" in invoke(executable, root, ["-h"]).stdout
    assert invoke(executable, root, ["-V"]).stdout.startswith(b"fsimg ")


def filesystems(decoded):
    """The --formats JSON records of the filesystem formats, by name."""
    return {record["name"]: record for record in decoded
            if record["type"] == "format" and record["kind"] == "filesystem"}


def test_posix_metadata(executable, root):
    """FAT and exFAT store no POSIX fields or links: -p imports become counted
    losses that leave image bytes unchanged, and a link fails planning. The
    QNX6 and QNX4 formats store every kind and the POSIX fields."""
    decoded = completed(invoke(executable, root, ["--formats", "--json"]))
    formats = filesystems(decoded)
    qnx6 = formats.pop("qnx6")
    assert qnx6["entry_kinds"] == 15 and qnx6["metadata_fields"] == 0x77
    assert qnx6["max_label_bytes"] == 0 and qnx6["write_settings"] == 15
    qnx4 = formats.pop("qnx4")
    assert qnx4["entry_kinds"] == 15 and qnx4["metadata_fields"] == 0x77
    assert qnx4["max_label_bytes"] == 48 and qnx4["write_settings"] == 4
    assert all(record["write_settings"] == 0 for record in formats.values())
    assert all(record["entry_kinds"] == 3 for record in formats.values())
    assert all(record["metadata_fields"] == 15 for record in formats.values())
    assert formats["fat16"]["max_label_bytes"] == 11
    assert formats["exfat"]["max_label_bytes"] == 22

    plans = []
    for flags in [[], ["-p"]]:
        decoded = completed(invoke(executable, root, [
            "c", "-n", "--json", "-R", *flags, "-F", "fat16", "-C", "source", ".",
        ]))
        plans.append(next(record for record in decoded if record["type"] == "plan"))
    # Three entries each lose an owner and a mode.
    assert int(plans[1]["metadata_losses"]) == int(plans[0]["metadata_losses"]) + 6
    completed(invoke(executable, root, [
        "c", "--json", "-R", "-p", "-F", "fat16", "-f", "posix.img", "-C", "source", ".",
    ]))
    same_bytes(root / "fat16.img", root / "posix.img")

    listed = completed(invoke(executable, root, ["t", "--json", "-f", "posix.img"]))
    entries = {record["path"]: record for record in listed if record["type"] == "entry"}
    assert entries["/nested"]["kind"] == "directory"
    assert entries["/HELLO.TXT"]["kind"] == "file"
    assert all(record["uid"] is None and record["mode"] is None and
               record["changed"] is None for record in entries.values())
    info = completed(invoke(executable, root, ["i", "--json", "-f", "posix.img"]))[1]
    assert info["uuid"] is None
    assert b"UUID" not in invoke(executable, root, ["i", "-f", "posix.img"]).stdout

    linked = root / "linked-source"
    linked.mkdir()
    (linked / "target").write_bytes(b"data")
    os.symlink("target", linked / "link")
    # Unconfined, the scan imports the link and planning rejects its kind.
    error = failed(invoke(executable, root, [
        "c", "-n", "--json", "-S", "off", "-F", "fat16", "-C", "linked-source", ".",
    ], expected=1))
    assert error["code"] == -6 and error["path"] == "/link"
    assert "entry kind" in error["message"]
    human = invoke(executable, root, [
        "c", "-n", "-S", "off", "-F", "fat16", "-C", "linked-source", ".",
    ], expected=1)
    assert b"cannot store this entry kind" in human.stderr
    # A confined worker may be denied link inspection; either way the
    # failure is unsupported and names the link.
    error = failed(invoke(executable, root, [
        "c", "-n", "--json", "-F", "fat16", "-C", "linked-source", ".",
    ], expected=1))
    assert error["code"] == -6 and error["path"] == "/link"
    assert ("entry kind" in error["message"] or
            "link inspection is not permitted" in error["message"])

    skipped = completed(invoke(executable, root, ["x", "--json", "-k", "-f", "posix.img"]))
    assert skipped[-1]["skipped_entries"] == "0" and skipped[-1]["command"] == "x"
    assert (root / "posix/HELLO.TXT").read_bytes() == b"hello\n"
    invoke(executable, root, ["t", "-k", "-f", "posix.img"], expected=2)


def accepted_letters(executable, root, mode):
    """The option letters mode accepts: each is given with an argument, and
    one the parser does not refuse as unknown or foreign to the mode is
    accepted. Mode letters (except -W, which verifies in c), -V and -M are
    left out."""
    def accepted(letter):
        stderr = invoke(executable, root, [mode, f"-{letter}", "1"], expected=None).stderr
        return not any(refusal in stderr for refusal in [
            b"unknown option", b"does not apply to mode", b"-V takes no other"])
    skipped = set("cxtiM") | (set() if mode.startswith("c") else {"W"})
    return [letter for letter in string.ascii_letters
            if letter not in skipped and accepted(letter)]


def test_human_help(executable, root):
    empty = invoke(executable, root, [], expected=2)
    assert empty.stdout == b"" and empty.stderr.startswith(b"usage: fsimg")
    assert b"modes:" in empty.stderr and b"errno" not in empty.stderr
    help_output = invoke(executable, root, ["-h"])
    assert help_output.stdout == empty.stderr and help_output.stderr == b""
    for mode, usage in [("c", "c[W]"), ("x", "x"), ("t", "t"), ("i", "i"),
                        ("W", "W"), ("--formats", "--formats")]:
        help_output = invoke(executable, root, [mode, "-h"])
        assert help_output.stdout.startswith(f"usage: fsimg {usage} ".encode())
        assert b"options:" in help_output.stdout and b"example" in help_output.stdout
        assert b"modes:" not in help_output.stdout and help_output.stderr == b""
    # Each mode's help describes every option letter the mode accepts, as
    # the parser itself reports acceptance, except mode letters, -V and -M,
    # which is recognized only to be reported as unsupported.
    for mode in ["c", "cW", "x", "t", "W", "i"]:
        mode_help = invoke(executable, root, [mode, "-h"]).stdout.decode()
        for letter in accepted_letters(executable, root, mode):
            assert f"\n    -{letter} " in mode_help, (mode, letter)
    create_help = invoke(executable, root, ["c", "-h"]).stdout.decode()
    for option in ["--free-space=", "--json"]:
        assert f"\n    {option}" in create_help, option
    assert re.search(r"-F stack .*gpt", create_help, re.S), create_help
    missing = invoke(executable, root, ["i"], expected=2)
    assert missing.stderr == b"fsimg: -f image is required\nusage: fsimg i -f image [options]\n"
    for arguments, detail in [
        (["i", "-Q"], b'unknown option "-Q"'),
        (["i", "-f"], b'option requires an argument "-f"'),
        (["no-such-command"], b'unknown mode or option letter in the first argument "-"'),
        (["extract"], b'unknown mode or option letter in the first argument "r"'),
        ([""], b"the first argument names no mode"),
        (["-v"], b"a mode is required"),
        (["-V", "x"], b"-V takes no other option or operand"),
    ]:
        result = invoke(executable, root, arguments, expected=2)
        assert detail in result.stderr and b"usage:" in result.stderr, (arguments, result.stderr)
        assert b"errno" not in result.stderr and result.stdout == b""
    formats = invoke(executable, root, ["--formats"])
    # The DMG and gzip rows are each followed by the zlib version fsimg runs
    # against.
    version = re.search(rb"\n {20}(zlib [0-9][0-9A-Za-z.-]*)\n", formats.stdout)
    assert version is not None, formats.stdout
    assert formats.stdout.count(version.group(0)) == 2, formats.stdout
    assert formats.stdout.replace(version.group(0), b"\n") == (b"Format  Read  Write  Verify\n"
                              b"fat12   yes   yes    yes\n"
                              b"fat16   yes   yes    yes\n"
                              b"fat32   yes   yes    yes\n"
                              b"exfat   yes   yes    yes\n"
                              b"qnx6    yes   yes    yes\n"
                              b"qnx4    yes   yes    yes\n"
                              b"\n"
                              b"Layer  Kind         Read  Write  Verify  Codecs         Settings\n"
                              b"gpt    scheme       yes   yes    yes     -              "
                              b"sector,disk,type,name,disk-guid,guid\n"
                              b"mbr    scheme       yes   yes    yes     -              "
                              b"disk,type,active,id\n"
                              b"dmg    container    yes   yes    yes     raw,zero,zlib  "
                              b"compress,level,chunk,uuid\n"
                              b"vhd    container    yes   yes    yes     raw            align,uuid\n"
                              b"simg   container    yes   yes    yes     raw,zero,fill  block,crc\n"
                              b"gzip   compression  yes   yes    yes     -              level\n"
                              b"zstd   compression  no    no     no      -              -\n"
                              b"xz     compression  no    no     no      -              -\n")
    assert formats.stderr == b""
    verbose = invoke(executable, root, ["--formats", "-v"])
    assert verbose.stdout == formats.stdout and b"Sandbox backend:" in verbose.stderr
    hostile_option = invoke(executable, root, ["t", "--" + "\x1b\n" * 12000], expected=2)
    assert b"unknown option" in hostile_option.stderr and b"truncated" in hostile_option.stderr
    assert b"usage:" in hostile_option.stderr and b"\x1b" not in hostile_option.stderr
    assert len(hostile_option.stderr) < 4096


def test_human_commands(executable, root):
    image = "human.img"
    created = invoke(executable, root, ["c", "-F", "fat12", "-f", image, "-C", "source", "."])
    assert created.stdout == b""
    assert created.stderr.count(b"metadata fields") == 1
    assert b"fields=0x" not in created.stderr and b"use -v" in created.stderr
    if b"sandbox unavailable" in created.stderr:
        assert created.stderr.count(b"sandbox unavailable") == 1
    names = ['"/HELLO.TXT"', '"/nested"', '"/nested/caf\u00e9.txt"']
    assert listing(executable, root, image) == names
    assert invoke(executable, root, ["t", "-f", image]).stdout.decode().splitlines() == names
    one_level = invoke(executable, root, ["t", "--no-recursion", "-f", image])
    assert one_level.stdout.decode().splitlines() == names[:2]
    # FAT stores no permission bits; the sizes and times are the image's.
    columns = long_listing(invoke(executable, root, ["tvf", image]))
    assert list(columns) == names
    assert columns[names[0]][:3] == ("-", "?????????", "6")
    assert columns[names[1]][:3] == ("d", "?????????", "-")
    assert columns[names[2]][:3] == ("-", "?????????", "131101")
    assert all(column[3] != "-" for column in columns.values())
    info = invoke(executable, root, ["i", "-f", image])
    assert info.stdout.startswith(b"Stack:        fat12\nCompression:  none\n")
    assert b"Format:       FAT12\n" in info.stdout
    assert b"Validation:   header only\n" in info.stdout
    assert b"Free space:   unknown (not scanned)\n" in info.stdout
    assert b"complete:" not in info.stdout and b"profile=" not in info.stdout
    assert invoke(executable, root, ["--info", "-f", image]).stdout == info.stdout
    plan = invoke(executable, root, ["c", "-n", "-R", "-F", "fat12", "-C", "source", "."])
    assert b"Volume size:" in plan.stdout and b"Metadata:" in plan.stdout
    assert b"source profile" not in plan.stdout and b"complete:" not in plan.stdout
    verbose = invoke(executable, root, [
        "c", "-vv", "-F", "fat12", "-f", "human-verbose.img", "-C", "source", ".",
    ])
    assert verbose.stdout == b"" and b"Volume size:" in verbose.stderr
    assert verbose.stderr.count(b"source profile requires") == 1
    assert b"metadata example:" in verbose.stderr and b"modification time" in verbose.stderr
    assert b"fields=0x" not in verbose.stderr
    verified = invoke(executable, root, ["W", "-f", image])
    assert verified.stdout == b'"human.img": filesystem structure verified (2 files, 2 directories)\n'
    payload = invoke(executable, root, ["W", "-d", "-f", image])
    assert b"filesystem structure and initialized file data verified" in payload.stdout
    # The fresh directory is named after the image, beside the working one.
    extracted = invoke(executable, root, ["x", "-f", image])
    assert extracted.stdout == b"" and extracted.stderr.count(b"metadata fields") == 1
    same_bytes(root / "source/nested/caf\u00e9.txt", root / "human/nested/caf\u00e9.txt")
    assert (root / "human").stat().st_mode & 0o777 == 0o700
    raw = invoke(executable, root, ["x", "-O", "-f", image, "/HELLO.TXT"])
    assert raw.stdout == b"hello\n"
    missing = invoke(executable, root, ["i", "-f", "missing\n\x1b.img"], expected=1)
    assert missing.stdout == b"" and b'cannot open image "missing\\u000a\\u001b.img"' in missing.stderr
    assert b"No such file or directory" in missing.stderr and b"errno=" not in missing.stderr
    assert b"\x1b" not in missing.stderr and len(missing.stderr.splitlines()) == 1
    geometry = invoke(executable, root, [
        "c", "-R", "-F", "fat32", "-s", "8M", "-f", "small.img", "-C", "source", ".",
    ], expected=1)
    assert b"too small for FAT32" in geometry.stderr and b"8388608 bytes" in geometry.stderr
    assert b"errno" not in geometry.stderr and not (root / "small.img").exists()
    for settings, explanation in [
        (["-F", "fat12", "-s", "1G"], b"too large for FAT12"),
        (["-F", "fat12", "-s", "1M", "--free-space=1M"],
         b"cannot fit the source tree and requested free space"),
        (["-F", "fat12", "--free-space=1G"],
         b"source tree and requested free space are too large for FAT12"),
        (["-F", "fat32", "-o", "cluster=32K", "--free-space=4T"],
         b"required image exceeds FAT32 size limits"),
        (["-F", "fat32", "--free-space=4T"], b"requested size and allocation settings conflict"),
    ]:
        rejected = invoke(executable, root, [
            "c", "-n", "-R", *settings, "-C", "source", ".",
        ], expected=1)
        assert explanation in rejected.stderr, (settings, rejected.stderr)
        assert b"image size 0" not in rejected.stderr
        assert b"free-space reserve 0" not in rejected.stderr


def test_bundled_modes(executable, root):
    """The first argument may bundle the mode with option letters, whose
    arguments follow in letter order; a leading dash spells the same bundle."""
    created = completed(invoke(executable, root, [
        "cfF", "bundled.img", "fat32", "--json", "-R", "-C", "source", ".",
    ]))
    assert created[-1]["command"] == "c" and created[-1]["published"] is True
    information = completed(invoke(executable, root, ["if", "bundled.img", "--json"]))
    assert information[0]["type"] == "stack" and information[0]["scheme"] is None
    assert information[1]["format"] == "fat32" and information[1]["stack"] == "fat32"
    assert information[1]["compression"] is None and information[-1]["command"] == "i"
    assert listing(executable, root, "bundled.img") == \
        invoke(executable, root, ["t", "-f", "bundled.img"]).stdout.decode().splitlines()
    assert invoke(executable, root, ["xOf", "bundled.img", "/HELLO.TXT"]).stdout == b"hello\n"
    for spelling, parent in [("xvf", "bundled-plain"), ("-xf", "bundled-dashed")]:
        (root / parent).mkdir()
        invoke(executable, root, [spelling, "bundled.img", "-C", parent])
        same_bytes(root / "source/nested/caf\u00e9.txt",
                   root / parent / "bundled/nested/caf\u00e9.txt")


def test_option_syntax(executable, root):
    """A usage error exits 2 before any work and names the offending word; a
    recognized request the build does not support exits 1 with its reason."""
    for arguments, detail in [
        (["x", "-k", "-k", "-f", "human.img"], b'option is given more than once "-k"'),
        (["t", "-l", "work=1", "-l", "work=1", "-f", "human.img"],
         b'resource-limit key is given more than once "work=1"'),
        (["c", "-n", "-F", "qnx4", "-o", "inodes=200", "-o", "inodes=200", "-C", "source", "."],
         b'setting key is given more than once "inodes=200"'),
        (["t", "-k", "-f", "human.img"], b'option does not apply to mode t "-k"'),
        (["i", "-d", "-f", "human.img"], b'option does not apply to mode i "-d"'),
        (["x", "-a", "-f", "human.img"], b'option does not apply to mode x "-a"'),
        (["tx", "-f", "human.img"], b"two modes are given; only c and W combine"),
        (["t", "-x", "-f", "human.img"], b"two modes are given; only c and W combine"),
        (["tt", "-f", "human.img"], b"a mode is given more than once"),
        (["-f", "human.img"], b"a mode is required"),
        (["if", "human.img", "extra"], b'unexpected operand "extra"'),
        (["x", "-O", "-C", "source", "-f", "human.img", "/HELLO.TXT"],
         b"-O rejects --json, --mtree, -C, -E and -k"),
        (["x", "-O", "-E", "-f", "human.img", "/HELLO.TXT"], b"-O rejects --json, --mtree, -C, -E and -k"),
        (["x", "-O", "-k", "-f", "human.img", "/HELLO.TXT"], b"-O rejects --json, --mtree, -C, -E and -k"),
        (["x", "-O", "-f", "human.img"], b"-O requires exactly one member"),
        (["x", "-O", "-f", "human.img", "/HELLO.TXT", "/nested"], b"-O requires exactly one member"),
        (["cf", "syntax.img", "-F", "fat12", "source/../source"],
         b'an operand contains a .. component "source/../source"'),
        (["cf", "syntax.img", "-F", "fat12", "-C", "source", ".", "-C", "source"],
         b"-C applies to no operand"),
        (["cf", "syntax.img", "-F", "fat12"], b"c requires at least one operand"),
        (["cf", "syntax.img", "-C", "source", "."], b"c requires -F with a filesystem"),
        (["c", "-F", "fat12", "-C", "source", "."], b"c requires -f image unless -n is given"),
        (["c", "-n", "-F", "fat12", "-e", "5", "-C", "source", "."], b"-e requires -R"),
    ]:
        result = invoke(executable, root, arguments, expected=2)
        assert detail in result.stderr and b"usage: fsimg" in result.stderr, (arguments, result.stderr)
        assert result.stdout == b""
    assert failed(invoke(executable, root, [
        "x", "-O", "--json", "-f", "human.img", "/HELLO.TXT",
    ], expected=2))["message"] == "-O rejects --json, --mtree, -C, -E and -k"
    for arguments, detail in [
        # Options precede operands: after the first member, -v is a member.
        (["t", "-f", "human.img", "/nested", "-v"], b"selecting several members is not supported"),

        # A forced container runs only its own probe, which a bare volume fails.
        (["t", "-F", "fat32/-/vhd", "-f", "human.img"], b"no VHD footer"),
        # -a takes a reserved compression from the outermost suffix.
        (["cf", "syntax.img.zst", "-a", "-F", "fat32", "-C", "source", "."],
         b"writing this compression is not supported"),
        (["cf", "syntax.img", "-M", "-F", "fat32", "-C", "source", "."],
         b"-M minimization is not supported"),
        (["cf", "syntax.img", "-F", "apfs", "-C", "source", "."],
         b"the named filesystem is reserved and not supported"),
    ]:
        result = invoke(executable, root, arguments, expected=1)
        assert detail in result.stderr and result.stdout == b"", (arguments, result.stderr)
    assert not [path for path in root.iterdir() if path.name.startswith("syntax.")]
    assert not (root / "-").exists()
    (root / "dotdot-parent").mkdir()
    result = invoke(executable, root, ["xf", "human.img", "-C", "dotdot-parent", "../x"], expected=2)
    assert b'a member contains a .. component "../x"' in result.stderr
    assert not list((root / "dotdot-parent").iterdir())


def test_create_operands(executable, root):
    """Each operand is stored under its own path, opened relative to the -C
    before it; -C may appear among the operands, -Cdir is one word, and
    after -- a word spelled -C is an operand."""
    invoke(executable, root, ["cf", "nested.img", "-F", "fat12", "-Csource", "nested"])
    assert listing(executable, root, "nested.img") == ['"/nested"', '"/nested/caf\u00e9.txt"']
    (root / "-C").mkdir()
    (root / "-C/dash.txt").write_bytes(b"dash\n")
    invoke(executable, root, ["cf", "dash.img", "-F", "fat12", "--", "-C"])
    assert listing(executable, root, "dash.img") == ['"/-C"', '"/-C/dash.txt"']
    for name, member in [("merge-a", "x"), ("merge-b", "y")]:
        (root / name / "sub").mkdir(parents=True)
        (root / name / "sub" / member).write_bytes(member.encode())
    invoke(executable, root, [
        "cf", "disjoint.img", "-F", "fat12", "-C", "merge-a", "sub", "-C", "source", "nested",
    ])
    assert sorted(listing(executable, root, "disjoint.img")) == [
        '"/nested"', '"/nested/caf\u00e9.txt"', '"/sub"', '"/sub/x"']

    # A trailing "/" or "/." would resolve a link before O_NOFOLLOW applies.
    (root / "source-link").symlink_to("source")
    for operand in ["source-link", "source-link/", "source-link/.", "source-link//./"]:
        result = invoke(executable, root, ["cf", "linked.img", "-F", "fat12", operand], expected=1)
        assert b"operand is a symbolic link" in result.stderr, (operand, result.stderr)
    assert not (root / "linked.img").exists()
    # A refused kind is unsupported, as the scanner reports it, not an I/O
    # failure.
    os.mkfifo(root / "operand-fifo")
    # A socket address holds a short path, so the socket is bound by its
    # path relative to the working directory, which holds root.
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(os.path.relpath(root / "operand-socket"))
    for operand in ["source-link", "source-link//./", "operand-fifo", "operand-socket"]:
        error = failed(invoke(executable, root, ["cf", "kind.img", "-F", "fat12", "--json",
                                                 operand], expected=1))
        assert error["status"] == "unsupported capability", error
        assert error["path"] == operand, error
    assert not (root / "kind.img").exists()
    # So is an image that is a link or a FIFO.
    (root / "image-link").symlink_to("disjoint.img")
    for image in ["image-link", "operand-fifo", "operand-socket"]:
        error = failed(invoke(executable, root, ["tf", image, "--json"], expected=1))
        assert error["status"] == "unsupported capability" and error["path"] == image, error
    (root / "image-link").unlink()
    (root / "operand-fifo").unlink()
    listener.close()
    (root / "operand-socket").unlink()
    # A trailing slash requires a directory, as it does for the host.
    result = invoke(executable, root, ["cf", "slashed.img", "-F", "fat12", "-C", "source",
                                       "HELLO.TXT/"], expected=1)
    assert b"Not a directory" in result.stderr and not (root / "slashed.img").exists()

    verified = completed(invoke(executable, root, [
        "cWf", "verified.img", "-F", "exfat", "--json", "-R", "-C", "source", ".",
    ]))
    assert verified[-1]["command"] == "cW" and verified[-1]["published"] is True
    report = next(record for record in verified if record["type"] == "verify")
    assert report["complete"] is True
    same_bytes(root / "exfat.img", root / "verified.img")
    invoke(executable, root, ["c", "-W", "-d", "-R", "-F", "exfat", "-f", "verified-data.img",
                              "-C", "source", "."])
    same_bytes(root / "exfat.img", root / "verified-data.img")
    print("SKIP: cW verification failure (no cheap way to stage an image that fails "
          "verification)")


def test_merged_operands(executable, root):
    """Operands whose directories coincide merge into one image directory,
    and a regular-file operand is stored as a file."""
    invoke(executable, root, [
        "cf", "merged.img", "-F", "fat12", "-C", "merge-a", ".", "-C", "merge-b", "sub",
    ])
    assert sorted(listing(executable, root, "merged.img")) == ['"/sub"', '"/sub/x"', '"/sub/y"']
    invoke(executable, root, ["cf", "file.img", "-F", "fat12", "-C", "source", "HELLO.TXT"])
    assert listing(executable, root, "file.img") == ['"/HELLO.TXT"']
    assert invoke(executable, root, ["xOf", "file.img", "/HELLO.TXT"]).stdout == b"hello\n"


def seeded_bytes(seed, size):
    """Seeded generated content as specs/design-mtree.md defines it."""
    blocks = (hashlib.sha256(struct.pack("<QQ", seed, index)).digest()
              for index in range((size + 31) // 32))
    return b"".join(blocks)[:size]


def generated_manifest(directory, name):
    """Write a manifest of generated files only into a new directory that
    also holds an unrelated file, and return the seeded file's bytes."""
    directory.mkdir()
    (directory / "unrelated.txt").write_bytes(b"not a source\n")
    seeded = seeded_bytes(7, 100)
    lines = [
        ". type=dir",
        "./sub type=dir mode=0755",
        f"./sub/gen type=file fsimg.content=seeded fsimg.seed=7 size=100 "
        f"sha256digest={hashlib.sha256(seeded).hexdigest()}",
        "./zero type=file fsimg.content=zero size=5000",
    ]
    (directory / name).write_text("\n".join(lines) + "\n")
    (directory / f"reversed-{name}").write_text("\n".join(reversed(lines)) + "\n")
    return seeded


def test_manifest_generated(executable, root):
    """An @file operand is an mtree manifest opened relative to the -C in
    effect. Generated files need no content directory, so a working
    directory holding other files is never a source; reordered lines
    produce the same reproducible image; after -- an @ word is still a
    manifest; and a manifest image streams to stdout and compresses like
    any other."""
    work = root / "manifest-generated"
    seeded = generated_manifest(work, "gen.mtree")
    created = completed(invoke(executable, work, [
        "cf", "gen.img", "-F", "fat12", "--json", "-R", "@gen.mtree"]))
    assert created[-1]["entries_processed"] == "3"
    assert listing(executable, work, "gen.img") == ['"/sub"', '"/sub/gen"', '"/zero"']
    assert invoke(executable, work, ["xOf", "gen.img", "/sub/gen"]).stdout == seeded
    assert invoke(executable, work, ["xOf", "gen.img", "/zero"]).stdout == bytes(5000)
    invoke(executable, work, ["cf", "reversed.img", "-F", "fat12", "-R",
                              "@reversed-gen.mtree"])
    same_bytes(work / "gen.img", work / "reversed.img")
    invoke(executable, root, ["cf", "dashed.img", "-F", "fat12", "-R",
                              "-C", "manifest-generated", "--", "@gen.mtree"])
    same_bytes(work / "gen.img", root / "dashed.img")
    streamed = invoke(executable, work, ["cf", "-", "-z", "-F", "fat12", "-R",
                                         "@gen.mtree"]).stdout
    assert gzip.decompress(streamed) == (work / "gen.img").read_bytes()


def test_manifest_host(executable, root):
    """A manifest's host files are read beneath the -C directory in effect,
    by the entry's own path or its contents= path; without a -C the manifest
    has no content directory, so an entry needing host bytes is refused even
    where the working directory holds the file; a manifest merges with a
    host root; and ./@name names a host object whose name begins with @."""
    work = root / "manifest-host"
    (work / "content/sub").mkdir(parents=True)
    (work / "content/sub/file.txt").write_bytes(b"host bytes\n")
    (work / "content/elsewhere.bin").write_bytes(b"other bytes\n")
    (work / "content/host.mtree").write_text(
        ". type=dir\n./sub type=dir\n./sub/file.txt type=file size=11\n"
        "./sub/named type=file contents=elsewhere.bin\n")
    (work / "tree/sub").mkdir(parents=True)
    (work / "tree/sub/x").write_bytes(b"x")
    operands = ["-C", "content", "@host.mtree", "-C", "tree", "."]
    invoke(executable, work, ["cf", "host.img", "-F", "fat12", *operands])
    assert sorted(listing(executable, work, "host.img")) == [
        '"/sub"', '"/sub/file.txt"', '"/sub/named"', '"/sub/x"']
    assert invoke(executable, work, ["xOf", "host.img", "/sub/file.txt"]).stdout == \
        b"host bytes\n"
    assert invoke(executable, work, ["xOf", "host.img", "/sub/named"]).stdout == \
        b"other bytes\n"
    error = failed(invoke(executable, work / "content", [
        "cf", "unanchored.img", "-F", "fat12", "--json", "@host.mtree"], expected=1))
    assert error["status"] == "invalid argument" and error["path"] == "/sub/file.txt", error
    assert error["message"] == ("manifest line 3: entry needs host bytes but the "
                                "manifest has no content directory"), error
    assert not (work / "content/unanchored.img").exists()
    invoke(executable, work / "content", ["cf", "anchored.img", "-F", "fat12",
                                          "-C", ".", "@host.mtree"])
    assert invoke(executable, work, ["xOf", "content/anchored.img",
                                     "/sub/file.txt"]).stdout == b"host bytes\n"
    # The path limit bounds image paths only: a limit shorter than the
    # worker's tag for the content directory still admits /x.
    (work / "content/short.mtree").write_text("./x type=file fsimg.content=zero size=1\n")
    invoke(executable, work, ["c", "-n", "-F", "fat12", "-l", "path=4",
                              "-C", "content", "@short.mtree"])

    (work / "@odd").mkdir()
    (work / "@odd/inside").write_bytes(b"odd\n")
    invoke(executable, work, ["cf", "odd.img", "-F", "fat12", "./@odd"])
    assert listing(executable, work, "odd.img") == ['"/@odd"', '"/@odd/inside"']


def test_manifest_refusals(executable, root):
    """A manifest operand must name an existing regular file, never followed,
    and a bare @ is a syntax error. A manifest counts once against fds, and
    its content directory, given by -C, once more. -n checks a generated
    digest, which needs no payload, but not a host file's, which would."""
    work = root / "manifest-refused"
    generated_manifest(work, "gen.mtree")
    (work / "directory").mkdir()
    (work / "link.mtree").symlink_to("gen.mtree")
    os.mkfifo(work / "fifo.mtree")
    for operand, detail in [("@link.mtree", b"manifest is a symbolic link"),
                            ("@missing.mtree", b"cannot open manifest"),
                            ("@directory", b"manifest is not a regular file"),
                            ("@fifo.mtree", b"manifest is not a regular file")]:
        result = invoke(executable, work, ["cf", "refused.img", "-F", "fat12", operand],
                        expected=1)
        assert detail in result.stderr, (operand, result.stderr)
        error = failed(invoke(executable, work, ["cf", "refused.img", "-F", "fat12",
                                                 "--json", operand], expected=1))
        assert error["status"] == ("I/O failure" if operand == "@missing.mtree" else
                                   "unsupported capability"), (operand, error)
    result = invoke(executable, work, ["cf", "refused.img", "-F", "fat12", "@"], expected=2)
    assert b"@ requires a manifest name" in result.stderr
    result = invoke(executable, work, ["cf", "refused.img", "-F", "fat12", "-l", "fds=5",
                                       "-C", ".", "@gen.mtree"], expected=1)
    assert b"fds limit 5" in result.stderr, result.stderr
    assert not (work / "refused.img").exists()
    invoke(executable, work, ["cf", "fds.img", "-F", "fat12", "-l", "fds=5", "unrelated.txt"])
    invoke(executable, work, ["cf", "fds-manifest.img", "-F", "fat12", "-l", "fds=5",
                              "@gen.mtree"])

    wrong = "0" * 64
    (work / "wrong.mtree").write_text(
        f"./gen type=file fsimg.content=seeded fsimg.seed=7 size=100 sha256digest={wrong}\n")
    error = failed(invoke(executable, work, ["cn", "-F", "fat12", "--json", "@wrong.mtree"],
                          expected=1))
    assert error["message"].startswith("manifest line 1: "), error
    (work / "unrelated.mtree").write_text(
        f"./unrelated.txt type=file sha256digest={wrong}\n")
    plan = completed(invoke(executable, work, ["cn", "-F", "fat12", "--json",
                                               "-C", ".", "@unrelated.mtree"]))
    assert next(record for record in plan if record["type"] == "plan")["entries"] == "1"
    result = invoke(executable, work, ["cf", "wrong-host.img", "-F", "fat12",
                                       "-C", ".", "@unrelated.mtree"], expected=1)
    assert b"do not match the manifest's sha256digest" in result.stderr, result.stderr
    assert not (work / "wrong-host.img").exists()


def test_extraction_names(executable, root):
    """x names its fresh directory after the image, removing one compression,
    one container and one image suffix in that order, ASCII
    case-insensitively. An existing name of any kind fails untouched; an
    image with no such suffix needs -E."""
    images = root / "stems"
    parent = root / "stem-parent"
    images.mkdir()
    parent.mkdir()
    stems = {"boot.IMG.GZ": "boot", "disk.DMG": "disk", "x.vhd.gz": "x",
             "y.RAW": "y", "a.b.img": "a.b", "q.simg": "q",
             "rootfs.vhd.img": "rootfs.vhd", "s.bin": "s", "t.dd": "t",
             "u.img.img": "u.img", "v.gz": "v", "w.dmg.img.gz": "w.dmg"}
    for index, (name, stem) in enumerate(stems.items()):
        os.link(root / "human.img", images / name)
        existing = parent / stem
        if index % 3 == 0:
            existing.write_bytes(b"keep")
        elif index % 3 == 1:
            existing.mkdir()
        else:
            existing.symlink_to("absent")
        result = invoke(executable, root, ["xf", f"stems/{name}", "-C", "stem-parent"], expected=1)
        expected = f'cannot create extraction directory "stem-parent/{stem}": File exists'
        assert expected.encode() in result.stderr, (name, result.stderr)
    assert sorted(os.listdir(parent)) == sorted(stems.values())
    for index, stem in enumerate(stems.values()):
        existing = parent / stem
        if index % 3 == 0:
            assert existing.read_bytes() == b"keep"
        elif index % 3 == 1:
            assert not list(existing.iterdir())
        else:
            assert os.readlink(existing) == "absent"
    for name in ["noext", ".img", "..img"]:
        os.link(root / "human.img", images / name)
        result = invoke(executable, root, ["xf", f"stems/{name}", "-C", "stem-parent"], expected=2)
        expected = f'cannot name the extraction directory after the image; use -E "stems/{name}"'
        assert expected.encode() in result.stderr, (name, result.stderr)
    assert sorted(os.listdir(parent)) == sorted(stems.values())


def test_extract_here(executable, root):
    """-E extracts into the -C directory itself, which must be empty and
    private: owned by the user and not writable by group or others. A
    symbolic link is not followed, and it or any other existing object that
    is not a directory is refused as unsupported."""
    private_directory(root, "into-empty")
    invoke(executable, root, ["xEf", "human.img", "-C", "into-empty"])
    same_bytes(root / "source/nested/caf\u00e9.txt", root / "into-empty/nested/caf\u00e9.txt")
    private_directory(root, "into-readable", 0o755)
    invoke(executable, root, ["xEf", "human.img", "-C", "into-readable"])
    assert (root / "into-readable/HELLO.TXT").read_bytes() == b"hello\n"

    (private_directory(root, "into-dotted") / ".keep").write_bytes(b"")
    private_directory(root, "into-group", 0o770)
    private_directory(root, "into-other", 0o707)
    for name, detail in [
        ("into-dotted", "extraction destination is not empty"),
        ("into-group", "extraction destination is writable by group or others"),
        ("into-other", "extraction destination is writable by group or others"),
    ]:
        result = invoke(executable, root, ["xEf", "human.img", "-C", name], expected=1)
        assert f'{detail} "{name}"'.encode() in result.stderr, (name, result.stderr)
    assert os.listdir(root / "into-dotted") == [".keep"]
    assert not list((root / "into-group").iterdir()) and not list((root / "into-other").iterdir())
    private_directory(root, "into-target")
    (root / "into-link").symlink_to("into-target")
    result = invoke(executable, root, ["xEf", "human.img", "-C", "into-link"], expected=1)
    assert b'-C directory is a symbolic link "into-link"' in result.stderr, result.stderr
    for spelling in ["into-link/", "into-link/."]:
        result = invoke(executable, root, ["xEf", "human.img", "-C", spelling], expected=1)
        assert b"-C directory is a symbolic link" in result.stderr, (spelling, result.stderr)
    error = failed(invoke(executable, root, ["xEf", "human.img", "--json", "-C", "into-link"],
                          expected=1))
    assert error["status"] == "unsupported capability", error
    assert not list((root / "into-target").iterdir())
    # So is any other existing object that is not a directory, however it
    # is spelled; a path through one names nothing and fails to open.
    (root / "into-file").write_bytes(b"")
    os.mkfifo(root / "into-fifo")
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(os.path.relpath(root / "into-socket"))
    for name in ["into-file", "into-file/", "into-file/.", "into-fifo", "into-socket"]:
        error = failed(invoke(executable, root, ["xEf", "human.img", "--json", "-C", name],
                              expected=1))
        assert error["status"] == "unsupported capability", (name, error)
        assert error["message"] == "-C directory is not a directory", (name, error)
        assert error["path"] == name, (name, error)
    error = failed(invoke(executable, root, ["xEf", "human.img", "--json", "-C",
                                             "into-file/inner"], expected=1))
    assert error["status"] == "I/O failure", error
    listener.close()
    for name in ["into-file", "into-fifo", "into-socket"]:
        (root / name).unlink()
    result = invoke(executable, root, ["xEf", "human.img"], expected=1)
    assert b'extraction destination is not empty "."' in result.stderr


def test_change_directory_kinds(executable, root):
    """A -C naming an existing object that is not a directory is refused as
    unsupported, as with -E, for x and c alike. The -C directory is followed
    through a symbolic link, as tar's -C is, so a link to a file is that
    file; a path through a non-directory names nothing and fails to open."""
    work = root / "change-kinds"
    work.mkdir()
    (work / "file").write_bytes(b"x")
    (work / "to-file").symlink_to("file")
    shutil.copyfile(root / "human.img", work / "human.img")

    def refusals(directory):
        return [["xf", "human.img", "--json", "-C", directory],
                ["cf", "out.img", "-F", "fat12", "--json", "-C", directory, "."]]

    for directory in ["file", "to-file", "file/"]:
        for arguments in refusals(directory):
            error = failed(invoke(executable, work, arguments, expected=1))
            assert error["status"] == "unsupported capability", (arguments, error)
            assert error["message"] == "-C directory is not a directory", error
            assert error["path"] == directory, error
    for arguments in refusals("file/inner"):
        error = failed(invoke(executable, work, arguments, expected=1))
        assert error["message"] == "cannot open -C directory", (arguments, error)
        assert error["os_error"] == errno.ENOTDIR, error
    assert sorted(path.name for path in work.iterdir()) == ["file", "human.img", "to-file"]


def test_extraction_acls(executable, root):
    """A destination with a non-trivial ACL is refused; a fresh directory
    that inherits one from its parent is removed again and the error names
    the parent."""
    if sys.platform == "darwin":
        def grant(path, inherit):
            rights = "list,search" + (",file_inherit,directory_inherit" if inherit else "")
            return subprocess.run(["chmod", "+a", f"user:nobody allow {rights}", str(path)])
    elif shutil.which("setfacl"):
        def grant(path, inherit):
            return subprocess.run(["setfacl", *(["-d"] if inherit else []), "-m", "u:nobody:rx",
                                   str(path)], stderr=subprocess.DEVNULL)
    else:
        print("SKIP: extraction ACL checks (no chmod +a or setfacl)")
        return
    guarded = private_directory(root, "acl-guarded")
    inheriting = private_directory(root, "acl-parent")
    if grant(guarded, False).returncode != 0 or grant(inheriting, True).returncode != 0:
        print("SKIP: extraction ACL checks (the filesystem refuses ACLs)")
        return
    result = invoke(executable, root, ["xEf", "human.img", "-C", "acl-guarded"], expected=1)
    assert b'extraction destination carries a non-trivial access control list "acl-guarded"' \
        in result.stderr
    assert not list(guarded.iterdir())
    result = invoke(executable, root, ["xf", "human.img", "-C", "acl-parent"], expected=1)
    assert b'-C directory gives new directories a non-trivial access control list "acl-parent"' \
        in result.stderr
    assert not list(inheriting.iterdir())

    # Every private directory fsimg creates passes the same check: the
    # staging of an --mtree file beside a private destination, a created
    # image's staging, and scratch.
    destination = private_directory(root, "acl-destination")
    for arguments, noun in [
            (["xf", "human.img", "--json", "-C", "acl-destination",
              "--mtree=acl-parent/human.mtree"], "--mtree directory"),
            (["cf", "acl-parent/new.img", "-F", "fat12", "--json", "-C", "source", "."],
             "output directory"),
            (["xf", "human.img", "--json", "-C", "acl-destination", "-T", "acl-parent"],
             "scratch directory")]:
        error = failed(invoke(executable, root, arguments, expected=1))
        assert error["message"] == \
            f"{noun} gives new directories a non-trivial access control list", error
        assert error["path"] == "acl-parent" and error["output_state"] == "absent", error
        assert not list(inheriting.iterdir()), arguments
        assert not list(destination.iterdir()), arguments


def test_failed_creation_output(executable, root):
    source = root / "oversized-source"
    source.mkdir()
    # The sparse payload exceeds the FAT12 writer's capacity. Planning must
    # reject it before any image creation or payload copying. Reproducible
    # timestamp conversion emits loss examples before geometry rejection.
    with (source / "large.bin").open("wb") as payload:
        payload.truncate(256 * 1024 * 1024)
    message = b"fsimg: source tree is too large for FAT12\n"
    arguments = ["c", "-R", "-F", "fat12", "-f", "oversized.img"]
    operands = ["-C", "oversized-source", "."]
    rejected = invoke(executable, root, [*arguments, *operands], expected=1)
    assert rejected.stdout == b"" and rejected.stderr.endswith(message)
    lines = rejected.stderr.splitlines(keepends=True)
    assert lines[-1] == message
    assert all(line.startswith(b"fsimg: warning: OS sandbox") for line in lines[:-1])
    assert b"metadata" not in rejected.stderr and b"0 bytes" not in rejected.stderr
    assert not (root / "oversized.img").exists()
    assert not list(root.glob(".fsimg-*"))

    verbose = invoke(executable, root, [*arguments, "-vv", *operands], expected=1)
    assert b"metadata example:" in verbose.stderr and verbose.stderr.endswith(message)
    assert b"final count unavailable" not in verbose.stderr

    decoded = failed(invoke(executable, root, [*arguments, "--json", *operands], expected=1))
    assert decoded["code"] == -7 and decoded["output_state"] == "absent"
    assert decoded["message"] == "source tree is too large for FAT12"
    assert decoded["resource"] == "none" and decoded["limit"] == "0"
    constrained = invoke(executable, root, [
        *arguments, "-o", "cluster=512", *operands,
    ], expected=1)
    assert b"source tree is too large for FAT12 with 512-byte clusters" in constrained.stderr
    small = invoke(executable, root, [
        "c", "-n", "-R", "-F", "fat32", "-s", "8M", "-o", "cluster=512", *operands,
    ], expected=1)
    assert b"too small for FAT32 with 512-byte clusters" in small.stderr


def test_human_loss_bounds(executable, root):
    source = root / "loss-source"
    source.mkdir()
    for index in range(40):
        (source / f"f{index:02}").write_bytes(b"")
    # Host import exposes access/modification times everywhere and birth time
    # on macOS/FreeBSD. Reproducible FAT creation substitutes every such field.
    expected_losses = 80 if sys.platform.startswith("linux") else 120
    notice = f"{expected_losses} metadata fields".encode()
    arguments = ["c", "-n", "-R", "-F", "fat12"]
    operands = ["-C", "loss-source", "."]
    ordinary = invoke(executable, root, [*arguments, *operands])
    assert notice in ordinary.stderr
    assert b"metadata example:" not in ordinary.stderr
    verbose = invoke(executable, root, [*arguments, "-vv", *operands])
    assert notice in verbose.stderr
    assert verbose.stderr.count(b"metadata example:") == 32
    assert b"fields=0x" not in verbose.stderr


def test_format(executable, root, profile, payload):
    image = f"{profile}.img"
    decoded = completed(invoke(executable, root, [
        "c", "--json", "-R", "-F", profile, "-f", image, "-C", "source", ".",
    ]))
    plan = next(record for record in decoded if record["type"] == "plan")
    assert plan["format"] == profile and int(plan["payload_bytes"]) == len(payload) + 6
    assert decoded[-1]["published"] is True
    assert (root / image).stat().st_mode & 0o077 == 0

    information = completed(invoke(executable, root, ["i", "--json", "-T", "-", "-f", image]))
    info = information[1]
    assert information[0]["stack"] == profile and information[0]["volume_selected"]
    assert info["format"] == profile and info["label"] == "FSIMG"
    assert info["stack"] == profile and info["compression"] is None
    assert info["validation_scope"] == "header" and info["entry_count"] is None
    assert info["free_bytes"] is None

    listed = completed(invoke(executable, root, ["t", "--json", "-f", image]))
    assert listed[-1]["command"] == "t"
    entries = {record["path"]: record for record in listed if record["type"] == "entry"}
    assert set(entries) == {"/HELLO.TXT", "/nested", "/nested/caf\u00e9.txt"}
    assert all(record["complete"] for record in entries.values())
    assert isinstance(entries["/nested/caf\u00e9.txt"]["size_bytes"], str)
    modified = entries["/HELLO.TXT"]["modified"]
    assert modified["basis"] == ("utc" if profile == "exfat" else "wall-clock")
    assert invoke(executable, root, ["x", "-O", "-f", image, "/NESTED/CAF\u00c9.TXT"]).stdout == payload

    # Extraction creates ./<profile> from <profile>.img.
    destination = root / profile
    completed(invoke(executable, root, ["x", "--json", "-f", image]))
    same_bytes(root / "source/nested/caf\u00e9.txt", destination / "nested/caf\u00e9.txt")
    assert (destination / "HELLO.TXT").read_bytes() == b"hello\n"
    assert destination.stat().st_mode & 0o077 == 0

    verification = completed(invoke(executable, root, ["W", "--json", "-d", "-f", image]))
    assert verification[-1]["command"] == "W"
    report = next(record for record in verification if record["type"] == "verify")
    assert report["validation_scope"] == "initialized-payload" and report["files"] == "2"
    assert int(report["initialized_bytes_read"]) == len(payload) + 6

    second = f"{profile}-second.img"
    completed(invoke(executable, root, [
        "c", "--json", "-R", "-m", "16M", "-F", profile, "-f", second, "-C", "source", ".",
    ]))
    same_bytes(root / image, root / second)

    before = (root / image).stat()
    error = failed(invoke(executable, root, [
        "c", "--json", "-R", "-F", profile, "-f", image, "-C", "source", ".",
    ], expected=1))
    assert error["code"] == -12 and (root / image).stat().st_ino == before.st_ino
    assert failed(invoke(executable, root, [
        "x", "--json", "-f", image,
    ], expected=1))["code"] == -12


def test_failures(executable, root):
    for arguments in [
        ["i", "--json", "-f", "exfat.img", "extra"],
        ["x", "-O", "--json", "-f", "exfat.img", "/HELLO.TXT", "/nested"],
        ["c", "--json", "-F", "exfat", "-e", "315532800", "-n", "-C", "source", "."],
        ["i", "--json", "-f", "exfat.img", "-l", "work=1", "-l", "work=1"],
        ["i", "--json", "-f", "exfat.img", "-l", "seconds=18446744073709551615"],
        ["c", "--json", "-n", "-F", "qnx4", "-L", "A" * 49, "-C", "source", "."],
    ]:
        failed(invoke(executable, root, arguments, expected=2))
    # An empty standard input is an unrecognised image, not a hang on
    # whatever input the test runner happens to hold.
    assert failed(invoke(executable, root, ["i", "--json", "-f", "-"], expected=1,
                         stdin=subprocess.DEVNULL))["code"] == -4
    # The command carries QNX4's 48-byte label; exFAT's own limit refuses it.
    assert completed(invoke(executable, root, [
        "c", "--json", "-n", "-F", "qnx4", "-L", "A" * 48, "-C", "source", ".",
    ]))[-1]["validation_scope"] == "planned"
    failed(invoke(executable, root, [
        "c", "--json", "-n", "-F", "exfat", "-L", "A" * 48, "-C", "source", ".",
    ], expected=1))

    planned = completed(invoke(executable, root, [
        "c", "--json", "-n", "-R", "-F", "exfat", "-f", "unused.img", "-C", "source", ".",
    ]))
    assert planned[-1]["validation_scope"] == "planned" and not (root / "unused.img").exists()
    assert failed(invoke(executable, root, [
        "W", "--json", "-f", "exfat.img", "-l", "work=1",
    ], expected=1))["resource"] == "work"
    assert failed(invoke(executable, root, [
        "t", "--json", "-f", "exfat.img", "-l", "path=4", "/abc",
    ], expected=1))["resource"] == "path"
    # A range addresses the disk, which only the worker sees once it has
    # peeled any container, so the library's selection refuses it.
    past_end = failed(invoke(executable, root, [
        "i", "--json", "-f", "exfat.img", "--offset=1T",
    ], expected=1))
    assert past_end["code"] == -1
    assert past_end["message"] == "the range starts past the end of the disk"

    (root / "truncated.img").write_bytes(bytes(511))
    failed(invoke(executable, root, ["i", "--json", "-f", "truncated.img"], expected=1))
    os.mkfifo(root / "stream")
    failed(invoke(executable, root, ["i", "--json", "-f", "stream"], expected=1))

    (root / "unsafe-source").mkdir()
    (root / "sentinel").write_bytes(b"outside")
    (root / "unsafe-source/pivot").symlink_to("../sentinel")
    error = failed(invoke(executable, root, [
        "c", "--json", "-R", "-F", "exfat", "-f", "unsafe.img", "-C", "unsafe-source", ".",
    ], expected=1))
    assert error["output_state"] == "absent" and not (root / "unsafe.img").exists()
    assert (root / "sentinel").read_bytes() == b"outside"

    (root / "output-pivot").symlink_to("sentinel")
    for flags in [[], ["-E"]]:
        failed(invoke(executable, root, [
            "x", *flags, "--json", "-f", "exfat.img", "-C", "output-pivot",
        ], expected=1))
    assert (root / "sentinel").read_bytes() == b"outside"

    # The outer file contains a deliberately offset volume and unrelated tail.
    with (root / "sliced.img").open("wb") as output, (root / "exfat.img").open("rb") as source:
        output.write(bytes(4096))
        while data := source.read(65536):
            output.write(data)
        output.write(b"outside")
    volume_bytes = str((root / "exfat.img").stat().st_size)
    completed(invoke(executable, root, [
        "W", "--json", "-d", "--offset=4096", f"--length={volume_bytes}", "-f", "sliced.img",
    ]))


def test_output_failures(executable, root):
    read_fd, write_fd = os.pipe()
    os.close(read_fd)
    try:
        result = subprocess.run([
            str(executable), "x", "-O", "-f", "exfat.img", "/nested/caf\u00e9.txt",
        ], cwd=root, stdout=write_fd, stderr=subprocess.PIPE, timeout=15)
        assert result.returncode == 1 and b"output write failed: Broken pipe" in result.stderr
    finally:
        os.close(write_fd)

    # A blocked consumer cannot prevent the coordinator from receiving signals.
    read_fd, write_fd = os.pipe()
    try:
        os.set_blocking(write_fd, False)
        while True:
            try:
                os.write(write_fd, bytes(4096))
            except BlockingIOError:
                break
        os.set_blocking(write_fd, True)
        process = subprocess.Popen([
            str(executable), "--formats", "--json",
        ], cwd=root, stdout=write_fd, stderr=subprocess.PIPE)
        time.sleep(0.2)
        process.send_signal(signal.SIGINT)
        _, diagnostic = process.communicate(timeout=10)
        assert process.returncode == 1 and b"cancelled" in diagnostic
    finally:
        os.close(read_fd)
        os.close(write_fd)


def test_examples(executable, root):
    programs = executable.parent
    output = root / "example-output"
    scratch = root / "example-scratch"
    output.mkdir(mode=0o700)
    scratch.mkdir(mode=0o700)
    invoke(programs / "example-create-tree", root, ["source", str(output), "tree.img", str(scratch)])
    completed(invoke(executable, root, ["W", "--json", "-d", "-f", str(output / "tree.img")]))
    invoke(programs / "example-provider", root, [str(output), "provider.img", str(scratch)])
    image = output / "provider.img"
    sampled = invoke(programs / "example-read-slice", root, [
        str(image), "0", str(image.stat().st_size), str(scratch),
    ])
    assert b"sampled_bytes=29" in sampled.stdout
    read_fd, write_fd = os.pipe()
    os.close(read_fd)
    try:
        # Python ignores SIGPIPE. Preserve that disposition across exec so
        # the example must notice stdio failure rather than die by a signal.
        broken = subprocess.run([
            str(programs / "example-read-slice"), str(image), "0",
            str(image.stat().st_size), str(scratch),
        ], cwd=root, stdout=write_fd, stderr=subprocess.PIPE,
            restore_signals=False, timeout=10)
        assert broken.returncode == 1, ("example ignored stdout failure", broken.returncode)
    finally:
        os.close(write_fd)
    invoke(programs / "example-read-slice", root, [
        str(image), "0", str(image.stat().st_size + 1), str(scratch),
    ], expected=1)
    invoke(programs / "example-extract", root, [str(image), str(output), "extracted", str(scratch)])
    assert (output / "extracted/provider.txt").read_bytes() == b"hello from a source provider\n"
    invoke(programs / "example-extract", root, [
        str(image), str(output), "extracted", str(scratch),
    ], expected=1)
    invoke(programs / "example-provider", root, [
        str(output), "provider.img", str(scratch),
    ], expected=1)
    assert not list(scratch.iterdir())


def test_qnx6(executable, root, payload):
    """QNX6 creation with its typed settings: a big-endian volume with a
    chosen UUID, inode capacity and reserve stores the POSIX fields and a
    symbolic link, reads back and verifies, and is reproducible."""
    source = root / "qnx6-source"
    (source / "nested").mkdir(parents=True)
    (source / "nested/caf\u00e9.txt").write_bytes(payload)
    (source / "nested/caf\u00e9.txt").chmod(0o640)
    os.symlink("nested/caf\u00e9.txt", source / "link")
    uuid = "89abcdef-0123-4567-89ab-cdef01234567"
    arguments = ["c", "--json", "-R", "-p", "-S", "off", "-F", "qnx6",
                 "-o", "cluster=2K", "-o", "endian=big", "-o", "uuid=" + uuid.upper(),
                 "-o", "inodes=100", "-o", "reserve=0"]
    operands = ["-C", "qnx6-source", "."]
    decoded = completed(invoke(executable, root, [*arguments, "-f", "qnx6.img", *operands]))
    plan = next(record for record in decoded if record["type"] == "plan")
    assert plan["format"] == "qnx6" and plan["cluster_bytes"] == 2048
    image = (root / "qnx6.img").read_bytes()
    assert image[8192:8196] == bytes.fromhex("68191122")
    assert image[8192 + 32:8192 + 48] == bytes.fromhex(uuid.replace("-", ""))
    assert int.from_bytes(image[8192 + 52:8192 + 56], "big") == 112

    info = completed(invoke(executable, root, ["i", "--json", "-f", "qnx6.img"]))[1]
    assert info["format"] == "qnx6" and info["uuid"] == uuid
    listed = completed(invoke(executable, root, ["t", "--json", "-f", "qnx6.img"]))
    entries = {record["path"]: record for record in listed if record["type"] == "entry"}
    assert set(entries) == {"/.boot", "/link", "/nested", "/nested/caf\u00e9.txt"}
    assert entries["/link"]["kind"] == "symlink"
    assert entries["/nested/caf\u00e9.txt"]["mode"] == 0o640
    assert invoke(executable, root, [
        "x", "-O", "-f", "qnx6.img", "/nested/caf\u00e9.txt"]).stdout == payload
    assert invoke(executable, root, ["x", "-O", "-f", "qnx6.img", "/link"]).stdout == \
        "nested/caf\u00e9.txt".encode()
    verification = completed(invoke(executable, root, ["W", "--json", "-d", "-f", "qnx6.img"]))
    report = next(record for record in verification if record["type"] == "verify")
    assert report["validation_scope"] == "initialized-payload"

    completed(invoke(executable, root, [*arguments, "-m", "16M", "-f", "qnx6-second.img",
                                        *operands]))
    same_bytes(root / "qnx6.img", root / "qnx6-second.img")
    # Without -R and uuid= the volume gets a random version 4 UUID.
    completed(invoke(executable, root, ["c", "--json", "-S", "off", "-F", "qnx6",
                                        "-f", "qnx6-random.img", *operands]))
    random_uuid = completed(invoke(executable, root, [
        "i", "--json", "-f", "qnx6-random.img"]))[1]["uuid"]
    assert random_uuid[14] == "4" and random_uuid[19] in "89ab"
    error = failed(invoke(executable, root, [
        "c", "-n", "--json", "-F", "qnx6", "-L", "LABEL", *operands,
    ], expected=1))
    assert error["code"] == -6


def test_qnx4(executable, root, payload):
    """QNX4 creation through the command: a 48-byte label and an inodes=
    setting cross the worker job record into the image, which stores the
    POSIX fields and a symbolic link, reads back, verifies and is
    reproducible."""
    source = root / "qnx4-source"
    (source / "nested").mkdir(parents=True)
    (source / "nested/caf\u00e9.txt").write_bytes(payload)
    (source / "nested/caf\u00e9.txt").chmod(0o640)
    os.symlink("nested/caf\u00e9.txt", source / "link")
    label = "L" * 47 + "Q"
    arguments = ["c", "--json", "-R", "-p", "-S", "off", "-F", "qnx4",
                 "-L", label, "-o", "inodes=200"]
    operands = ["-C", "qnx4-source", "."]
    completed(invoke(executable, root, [*arguments, "-f", "qnx4.img", *operands]))
    image = (root / "qnx4.img").read_bytes()
    # The root block, block 2 at byte 512, holds .inodes in record 1, 200
    # slots of 64 bytes, and the label in 48 bytes at 0x1c0, unterminated.
    assert int.from_bytes(image[512 + 0x50:512 + 0x54], "little") == 200 * 64
    assert image[512 + 0x1c0:512 + 0x1f0] == label.encode()

    info = completed(invoke(executable, root, ["i", "--json", "-f", "qnx4.img"]))[1]
    assert info["format"] == "qnx4" and info["label"] == label
    listed = completed(invoke(executable, root, ["t", "--json", "-f", "qnx4.img"]))
    entries = {record["path"]: record for record in listed if record["type"] == "entry"}
    assert set(entries) == {"/.boot", "/.altboot", "/link", "/nested",
                            "/nested/caf\u00e9.txt"}
    assert entries["/link"]["kind"] == "symlink"
    assert entries["/nested/caf\u00e9.txt"]["mode"] == 0o640
    # The long listing shows the stored permission bits and link length.
    columns = long_listing(invoke(executable, root, ["tvf", "qnx4.img"]))
    assert columns['"/nested/caf\u00e9.txt"'][:3] == ("-", "rw-r-----", str(len(payload)))
    target_bytes = len("nested/caf\u00e9.txt".encode())
    assert columns['"/link"'][0] == "l" and columns['"/link"'][2] == str(target_bytes)
    assert columns['"/nested"'][0] == "d" and columns['"/nested"'][2] == "-"
    assert invoke(executable, root, [
        "x", "-O", "-f", "qnx4.img", "/nested/caf\u00e9.txt"]).stdout == payload
    verification = completed(invoke(executable, root, ["W", "--json", "-d", "-f", "qnx4.img"]))
    report = next(record for record in verification if record["type"] == "verify")
    assert report["validation_scope"] == "initialized-payload"
    completed(invoke(executable, root, [*arguments, "-f", "qnx4-second.img", *operands]))
    same_bytes(root / "qnx4.img", root / "qnx4-second.img")


ESP_TYPE = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
LINUX_TYPE = "0fc63daf-8483-4772-8e79-3d69d8477de4"
DISK_GUID = "12345678-9abc-4def-8123-456789abcdef"


def gpt_disk(sectors, partitions):
    """A GPT disk laid out as sgdisk does: protective MBR, primary header at
    LBA 1 with 128 entries from LBA 2, backup entries and header at the end.
    Each partition is (first_lba, last_lba, type, name, payload)."""
    disk = bytearray(sectors * 512)
    disk[446:462] = struct.pack("<B3sB3sII", 0, b"\0\x02\0", 0xEE, b"\xff\xff\xff",
                                1, min(sectors - 1, 0xFFFFFFFF))
    disk[510:512] = b"\x55\xaa"
    entries = bytearray(128 * 128)
    for index, (first, last, kind, name, payload) in enumerate(partitions):
        entries[index * 128:(index + 1) * 128] = (
            uuid.UUID(kind).bytes_le + uuid.UUID(int=index + 1).bytes_le +
            struct.pack("<QQQ", first, last, 0) + name.encode("utf-16-le").ljust(72, b"\0"))
        disk[first * 512:first * 512 + len(payload)] = payload
    entries_crc = zlib.crc32(entries)

    def header(current, alternate, entries_lba):
        fields = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0,
                             current, alternate, 34, sectors - 34,
                             uuid.UUID(DISK_GUID).bytes_le, entries_lba, 128, 128, entries_crc)
        return fields[:16] + struct.pack("<I", zlib.crc32(fields)) + fields[20:]

    disk[512:604] = header(1, sectors - 1, 2)
    disk[1024:1024 + len(entries)] = entries
    disk[(sectors - 33) * 512:(sectors - 1) * 512] = entries
    disk[(sectors - 1) * 512:(sectors - 1) * 512 + 92] = header(sectors - 1, 1, sectors - 33)
    return disk


def mbr_disk(sectors, partitions):
    """An MBR disk; each partition is (first_lba, sector_count, type, payload)
    and the first is active."""
    disk = bytearray(sectors * 512)
    disk[440:444] = struct.pack("<I", 0xA1B2C3D4)
    for index, (first, count, kind, payload) in enumerate(partitions):
        disk[446 + 16 * index:462 + 16 * index] = struct.pack(
            "<B3sB3sII", 0x80 if index == 0 else 0, b"\0\0\0", kind, b"\0\0\0", first, count)
        disk[first * 512:first * 512 + len(payload)] = payload
    disk[510:512] = b"\x55\xaa"
    return disk


def test_partitioned(executable, root):
    invoke(executable, root, ["c", "-R", "-F", "fat12", "-s", "1M", "-f", "part.img",
                              "-C", "source", "."])
    volume = (root / "part.img").read_bytes()
    names = listing(executable, root, "part.img")
    (root / "gpt.img").write_bytes(gpt_disk(8192, [
        (2048, 4095, ESP_TYPE, "EFI", volume), (4096, 6143, LINUX_TYPE, "data", b""),
    ]))

    info = invoke(executable, root, ["i", "-f", "gpt.img"]).stdout.decode()
    assert info.startswith(
        "Stack:        fat12/gpt\nCompression:  none\n"
        "Scheme:       GPT, 512-byte sectors\nDisk size:    4 MiB\n"
        f"Disk GUID:    {DISK_GUID}\n"
        'Partition 1:  esp, 1 MiB at 1 MiB, fat12, name "EFI", '
        "GUID 00000000-0000-0000-0000-000000000001, selected\n"
        'Partition 2:  linux, 1 MiB at 2 MiB, no filesystem, name "data", '
        "GUID 00000000-0000-0000-0000-000000000002\n"
        "Format:       FAT12\n"), info
    decoded = completed(invoke(executable, root, ["i", "--json", "-f", "gpt.img"]))
    assert [record["type"] for record in decoded] == \
        ["stack", "partition", "partition", "info", "summary"]
    stack, first, second, volume_info = decoded[:4]
    assert stack["stack"] == "fat12/gpt" and stack["scheme"] == "gpt"
    assert stack["disk_guid"] == DISK_GUID and stack["selected_partition"] == 1
    assert first["partition_type"] == "esp" and first["type_guid"] == ESP_TYPE
    assert first["name"] == "EFI" and first["format"] == "fat12" and first["selected"]
    assert first["offset_bytes"] == str(1 << 20) and first["length_bytes"] == str(1 << 20)
    assert second["filesystem_claim"] == "none" and second["format"] is None
    assert volume_info["stack"] == "fat12/gpt"

    for selection in [[], ["--partition=1"], ["--partition=name=EFI"],
                      ["--partition=type=esp"], ["--partition", f"type={ESP_TYPE}"],
                      ["-F", "fat12/gpt", "-o", "gpt:sector=512"],
                      ["--offset=1M", "--length=1M"]]:
        assert invoke(executable, root, ["tf", "gpt.img", *selection]).stdout.decode() \
            .splitlines() == names, selection
    for selection, detail in [
        (["--partition=3"], b"the selected partition slot is unused"),
        (["--partition=200"], b"the partition table has no slot of that number"),
        (["--partition=name=efi"], b"no partition has that name"),
        (["--partition=type=0x0c"], b"an MBR type byte cannot select a GPT partition"),
        (["-F", "gpt", "-o", "gpt:sector=4096"], b"GPT"),
        (["-F", "fat12/mbr"], b"no MBR partition table"),
    ]:
        result = invoke(executable, root, ["tf", "gpt.img", *selection], expected=1)
        assert detail in result.stderr, (selection, result.stderr)
    invoke(executable, root, ["tf", "gpt.img", "--partition=2"], expected=1)

    verified = invoke(executable, root, ["Wf", "gpt.img"]).stdout
    assert b'"gpt.img": GPT partition table (2 partitions) and partition 1 filesystem ' \
        b"structure verified" in verified, verified
    report = next(record for record in completed(invoke(
        executable, root, ["W", "--json", "-f", "gpt.img"])) if record["type"] == "verify")
    assert report["scheme"] == "gpt" and report["partitions_checked"] == 2
    assert report["partition"] == 1 and isinstance(stack["sector_bytes"], int)
    invoke(executable, root, ["xf", "gpt.img"])
    assert (root / "gpt/HELLO.TXT").read_bytes() == b"hello\n"

    # A partition name is untrusted text: terminal controls are escaped.
    (root / "hostile.img").write_bytes(gpt_disk(8192, [
        (2048, 4095, ESP_TYPE, "E\x1b]0;x\x07\x9b2J", volume),
    ]))
    info = invoke(executable, root, ["i", "-f", "hostile.img"]).stdout
    assert b"Partition 1:  esp" in info, info
    assert not any(control in info for control in [b"\x1b", b"\x07", b"\xc2\x9b"]), info

    # A damaged table fails every partition selection, never a raw range.
    damaged = bytearray((root / "gpt.img").read_bytes())
    damaged[512 + 16] ^= 1
    (root / "damaged-gpt.img").write_bytes(damaged)
    result = invoke(executable, root, ["tf", "damaged-gpt.img"], expected=1)
    assert b"GPT primary header CRC32 mismatch" in result.stderr, result.stderr
    assert invoke(executable, root, [
        "tf", "damaged-gpt.img", "--offset=1M", "--length=1M",
    ]).stdout.decode().splitlines() == names

    # Two filesystems leave i without a volume and every other mode failing.
    (root / "mbr.img").write_bytes(mbr_disk(8192, [
        (2048, 2048, 0x01, volume), (4096, 2048, 0x01, volume),
    ]))
    info = invoke(executable, root, ["i", "-f", "mbr.img"]).stdout.decode()
    assert info == (
        "Stack:        mbr\nCompression:  none\n"
        "Scheme:       MBR, 512-byte sectors\nDisk size:    4 MiB\n"
        "Disk ID:      0xa1b2c3d4\n"
        "Partition 1:  fat12, 1 MiB at 1 MiB, fat12, active\n"
        "Partition 2:  fat12, 1 MiB at 2 MiB, fat12\n"
        "Volume:       none selected: several partitions hold a filesystem: 1, 2; "
        "choose one with --partition\n"), info
    decoded = completed(invoke(executable, root, ["i", "--json", "-f", "mbr.img"]))
    assert [record["type"] for record in decoded] == \
        ["stack", "partition", "partition", "summary"]
    assert decoded[0]["volume_selected"] is False and decoded[0]["mbr_disk_id"] == 0xA1B2C3D4
    assert decoded[0]["selected_partition"] is None and decoded[1]["index"] == 1
    assert decoded[1]["mbr_type"] == 1 and decoded[1]["active"] is True
    result = invoke(executable, root, ["tf", "mbr.img"], expected=1)
    assert b"several partitions hold a filesystem: 1, 2" in result.stderr
    assert invoke(executable, root, ["tf", "mbr.img", "--partition=2"]).stdout.decode() \
        .splitlines() == names
    result = invoke(executable, root, ["tf", "mbr.img", "--partition=name=EFI"], expected=1)
    assert b"partition names exist only in GPT" in result.stderr
    info = invoke(executable, root, ["i", "-f", "mbr.img", "--partition=type=0x01"],
                  expected=1)
    assert b"several partitions have that type" in info.stderr


SEGMENT_ID = "0123abcd-4567-4890-8bcd-ef0123456789"


def dmg_image(disk, regions):
    """A DMG as hdiutil lays it out: the data fork, the XML plist, the koly
    trailer. Each (first, count) region in sectors gets a block table whose
    256-sector chunks are zero runs when blank, zlib runs when compression
    pays and raw runs otherwise; the master checksum covers the tables'."""
    fork, entries, master = bytearray(), [], 0
    for index, (first, count) in enumerate(regions):
        runs, crc = [], 0
        for sector in range(0, count, 256):
            sectors = min(256, count - sector)
            data = disk[(first + sector) * 512:(first + sector + sectors) * 512]
            crc = zlib.crc32(data, crc)
            packed = zlib.compress(data, 9)
            if not any(data):
                runs.append((0, sector, sectors, len(fork), 0))
            elif len(packed) < len(data):
                runs.append((0x80000005, sector, sectors, len(fork), len(packed)))
                fork += packed
            else:
                runs.append((1, sector, sectors, len(fork), len(data)))
                fork += data
        runs.append((0xFFFFFFFF, count, 0, len(fork), 0))
        mish = struct.pack(">4sIQQQII24sII128sI", b"mish", 1, first, count, 0, 0x208,
                           index, b"", 2, 32, struct.pack(">I", crc), len(runs))
        mish += b"".join(struct.pack(">IIQQQQ", kind, 0, sector, sectors, offset, length)
                         for kind, sector, sectors, offset, length in runs)
        master = zlib.crc32(struct.pack(">I", crc), master)
        entries.append(
            "\t\t\t<dict>\n\t\t\t\t<key>Attributes</key>\n\t\t\t\t<string>0x0050</string>\n"
            f"\t\t\t\t<key>Data</key>\n\t\t\t\t<data>{base64.b64encode(mish).decode()}</data>\n"
            f"\t\t\t\t<key>ID</key>\n\t\t\t\t<string>{index}</string>\n"
            "\t\t\t\t<key>Name</key>\n\t\t\t\t<string>region &amp; more</string>\n"
            "\t\t\t</dict>\n")
    xml = ('<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0">\n<dict>\n'
           "\t<key>resource-fork</key>\n\t<dict>\n\t\t<key>blkx</key>\n\t\t<array>\n" +
           "".join(entries) + "\t\t</array>\n\t</dict>\n</dict>\n</plist>\n").encode()
    koly = struct.pack(">4sIIIQQQQQII16sII128sQQ120sII128sIQ12s", b"koly", 4, 512, 1, 0, 0,
                       len(fork), 0, 0, 1, 1, uuid.UUID(SEGMENT_ID).bytes, 2, 32,
                       struct.pack(">I", zlib.crc32(fork)), len(fork), len(xml), b"", 2, 32,
                       struct.pack(">I", master), 1, len(disk) // 512, b"")
    return bytes(fork) + xml + koly


def vhd_image(disk):
    """A fixed VHD: the disk and a footer whose checksum is the one's
    complement of its byte sum."""
    footer = bytearray(struct.pack(">8sIIQI4sI4sQQHBBII16sB427s", b"conectix", 2, 0x10000,
                                   0xFFFFFFFFFFFFFFFF, 0, b"test", 0x10000, b"Wi2k", len(disk),
                                   len(disk), 120, 16, 63, 2, 0, uuid.UUID(DISK_GUID).bytes,
                                   0, b""))
    footer[64:68] = struct.pack(">I", ~sum(footer) & 0xFFFFFFFF)
    return bytes(disk) + bytes(footer)


def simg_image(disk):
    """An Android sparse image of 4 KiB blocks: a DONT_CARE chunk for each
    blank 64 KiB unit, a RAW chunk otherwise, and a closing CRC32 chunk."""
    chunks = []
    for offset in range(0, len(disk), 65536):
        unit = disk[offset:offset + 65536]
        if any(unit):
            chunks.append(struct.pack("<HHII", 0xCAC1, 0, len(unit) // 4096, 12 + len(unit)) + unit)
        else:
            chunks.append(struct.pack("<HHII", 0xCAC3, 0, len(unit) // 4096, 12))
    chunks.append(struct.pack("<HHIII", 0xCAC4, 0, 0, 16, zlib.crc32(disk)))
    header = struct.pack("<IHHHHIIII", 0xED26FF3A, 1, 0, 28, 12, 4096, len(disk) // 4096,
                         len(chunks), 0)
    return header + b"".join(chunks)


def test_containers(executable, root):
    """t, x, i and W read through a DMG, a fixed VHD and a sparse image, the
    partition table inside included; a forced container slot runs only its
    own probe; a range addresses the container's disk, longer than the
    file; and a damaged chunk fails the read instead of passing raw bytes."""
    # Incompressible bytes outside every partition give the DMG raw runs.
    disk = bytearray((root / "gpt.img").read_bytes())
    disk[6144 * 512:6400 * 512] = random.Random(7).randbytes(256 * 512)
    disk = bytes(disk)
    names = listing(executable, root, "gpt.img")
    (root / "gpt.dmg").write_bytes(dmg_image(disk, [(0, 2048), (2048, 2048), (4096, 4096)]))
    (root / "gpt.simg").write_bytes(simg_image(disk))
    (root / "part.vhd").write_bytes(vhd_image((root / "part.img").read_bytes()))
    assert (root / "gpt.dmg").stat().st_size < len(disk) // 2

    for image, stack in [("gpt.dmg", "fat12/gpt/dmg"), ("gpt.simg", "fat12/gpt/simg"),
                         ("part.vhd", "fat12/vhd")]:
        assert listing(executable, root, image) == names, image
        info = invoke(executable, root, ["i", "-f", image]).stdout.decode()
        assert info.startswith(f"Stack:        {stack}\nCompression:  none\nContainer:    "), info
        verified = invoke(executable, root, ["W", "-d", "-f", image]).stdout
        assert f'"{image}": {stack.rsplit("/", 1)[1]} container,'.encode() in verified, verified
        assert b"initialized file data verified" in verified, verified
        assert invoke(executable, root, ["tf", image, "-F", stack]).stdout.decode() \
            .splitlines() == names
    info = invoke(executable, root, ["i", "-f", "gpt.dmg"]).stdout.decode()
    assert "Container:    dmg, 4 MiB disk in " in info and \
        f"chunks (raw,zero,zlib), ID {SEGMENT_ID}\n" in info, info
    assert "Scheme:       GPT, 512-byte sectors\nDisk size:    4 MiB\n" in info, info
    stack = next(record for record in completed(invoke(
        executable, root, ["i", "--json", "-f", "gpt.dmg"])) if record["type"] == "stack")
    assert stack["stack"] == "fat12/gpt/dmg" and stack["container"] == "dmg"
    assert stack["container_codecs"] == "raw,zero,zlib" and stack["container_guid"] == SEGMENT_ID
    assert int(stack["container_chunks"]) > 3 and stack["disk_bytes"] == str(len(disk))
    report = next(record for record in completed(invoke(
        executable, root, ["W", "-d", "--json", "-f", "gpt.dmg"])) if record["type"] == "verify")
    assert report["container"] == "dmg" and report["scheme"] == "gpt"
    plain = next(record for record in completed(invoke(
        executable, root, ["i", "--json", "-f", "gpt.img"])) if record["type"] == "stack")
    assert plain["container"] is None and plain["container_chunks"] is None

    (root / "from-dmg").mkdir()
    invoke(executable, root, ["xf", "gpt.dmg", "-C", "from-dmg"])
    assert (root / "from-dmg/gpt/HELLO.TXT").read_bytes() == b"hello\n"
    # The range lies inside the 4 MiB disk though the file is far shorter.
    assert invoke(executable, root, [
        "tf", "gpt.dmg", "--offset=1M", "--length=1M",
    ]).stdout.decode().splitlines() == names
    result = invoke(executable, root, ["tf", "gpt.dmg", "--offset=5M"], expected=1)
    assert b"the range starts past the end of the disk" in result.stderr, result.stderr
    result = invoke(executable, root, ["tf", "gpt.dmg", "-F", "fat12/-/vhd"], expected=1)
    assert b"no VHD footer" in result.stderr, result.stderr

    # A flipped byte in the first zlib stream fails the read that decodes
    # it; one in raw bytes no read needs fails W on the data fork checksum.
    image = (root / "gpt.dmg").read_bytes()
    damaged = bytearray(image)
    damaged[damaged.index(b"\x78\xda") + 40] ^= 0xFF
    (root / "damaged.dmg").write_bytes(damaged)
    result = invoke(executable, root, ["tf", "damaged.dmg"], expected=1)
    assert b"compressed chunk is not a valid zlib stream" in result.stderr, result.stderr
    damaged = bytearray(image)
    damaged[image.index(disk[6144 * 512:6144 * 512 + 64])] ^= 0xFF
    (root / "damaged.dmg").write_bytes(damaged)
    assert listing(executable, root, "damaged.dmg") == names
    result = invoke(executable, root, ["W", "-f", "damaged.dmg"], expected=1)
    assert b"DMG data fork checksum does not match" in result.stderr, result.stderr


def test_compressed_and_streams(executable, root):
    """t, x, i and W read standard input through the coordinator's relay,
    raw, DMG and gzip alike, and gzip files in place, the relayed stream
    counting against fds; a terminal on stdin is refused; -z on a raw file
    is an absent signature; a raw stream that also passes for gzip is
    ambiguous until -z or -F resolves it; -T - leaves no spool for a gzip
    file; and a spool limit stops the decode."""
    names = listing(executable, root, "gpt.img")
    disk = (root / "gpt.img").read_bytes()
    (root / "gpt.img.gz").write_bytes(gzip.compress(disk, mtime=0))
    for image in ["gpt.img", "gpt.dmg", "gpt.img.gz"]:
        with (root / image).open("rb") as stream:
            listed = invoke(executable, root, ["tf", "-"], stdin=stream)
        assert listed.stdout.decode().splitlines() == names, image
    # The worker charges the relayed stream like any descriptor it holds.
    with (root / "gpt.img").open("rb") as stream:
        error = failed(invoke(executable, root, ["tf", "-", "--json", "-l", "fds=2"],
                              expected=1, stdin=stream))
    assert error["resource"] == "fds" and error["limit"] == "2", error
    with (root / "gpt.img").open("rb") as stream:
        invoke(executable, root, ["tf", "-", "-l", "fds=3"], stdin=stream)
    assert listing(executable, root, "gpt.img.gz") == names
    info = invoke(executable, root, ["i", "-f", "gpt.img.gz"]).stdout.decode()
    assert info.startswith("Stack:        fat12/gpt\nCompression:  gzip\n"), info
    with (root / "gpt.img.gz").open("rb") as stream:
        stack = next(record for record in completed(invoke(
            executable, root, ["i", "--json", "-f", "-"], stdin=stream))
            if record["type"] == "stack")
    assert stack["compression"] == "gzip" and stack["stack"] == "fat12/gpt"
    invoke(executable, root, ["Wf", "gpt.img.gz"])
    private_directory(root, "from-stdin")
    with (root / "gpt.img.gz").open("rb") as stream:
        invoke(executable, root, ["xf", "-", "-E", "-C", "from-stdin"], stdin=stream)
    assert (root / "from-stdin/HELLO.TXT").read_bytes() == b"hello\n"

    leader, follower = pty.openpty()
    try:
        result = invoke(executable, root, ["tf", "-"], expected=1, stdin=follower)
    finally:
        os.close(leader)
        os.close(follower)
    assert b"refusing to read an image from a terminal" in result.stderr, result.stderr
    # A standard input the relay cannot read fails as that, not as the
    # verdict the worker reaches on the bytes it got before the early end:
    # a closed descriptor, which the coordinator holds with an unreadable
    # placeholder so no descriptor of its own takes its place, and a
    # directory.
    # The AddressSanitizer runtime opens a socket before main(), which takes
    # descriptor 0 when it is closed, so the closed case is unobservable
    # there: the coordinator correctly relays that socket as the input.
    if "ASAN_OPTIONS" in os.environ:
        print("SKIP: closed standard input (the sanitizer runtime takes "
              "descriptor 0 before main)")
    else:
        result = invoke(executable, root, ["tf", "-"], expected=1,
                        preexec_fn=lambda: os.close(0))
        assert b"standard input could not be read" in result.stderr, result.stderr
    directory = os.open(root, os.O_RDONLY)
    try:
        result = invoke(executable, root, ["tf", "-"], expected=1, stdin=directory)
    finally:
        os.close(directory)
    assert b"standard input could not be read" in result.stderr, result.stderr
    # A non-blocking standard input with no data yet is waited for.
    reader, writer = os.pipe()
    os.set_blocking(reader, False)
    process = subprocess.Popen([str(executable), "tf", "-"], cwd=root,
                               stdin=reader, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    os.close(reader)
    time.sleep(0.5)
    with os.fdopen(writer, "wb") as stream:
        stream.write((root / "gpt.img.gz").read_bytes())
    output, errors = process.communicate(timeout=30)
    assert process.returncode == 0, errors
    assert output.decode().splitlines() == names
    result = invoke(executable, root, ["t", "-z", "-f", "gpt.img"], expected=1)
    assert b"no gzip signature" in result.stderr, result.stderr

    # gzip's identity in the protective MBR's boot code.
    (root / "ambiguous.img").write_bytes(b"\x1f\x8b\x08\x00" + disk[4:])
    for arguments, expected, detail in [
        ([], 1, b"passes for both a compressed and an uncompressed image"),
        (["-F", "fat12"], 0, b""),
        (["-z"], 1, b"DEFLATE data is invalid"),
    ]:
        with (root / "ambiguous.img").open("rb") as stream:
            result = invoke(executable, root, ["t", *arguments, "-f", "-"],
                            expected=expected, stdin=stream)
        assert detail in result.stderr, (arguments, result.stderr)
        if expected == 0:
            assert result.stdout.decode().splitlines() == names
    result = invoke(executable, root, ["tf", "gpt.img.gz", "-T", "-"], expected=1)
    assert b"-T - disables" in result.stderr, result.stderr
    result = invoke(executable, root, ["tf", "gpt.img.gz", "-l", "spool=1M"],
                    expected=1)
    assert b"spool limit 1048576" in result.stderr, result.stderr


BASIC_TYPE = "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"


def test_partitioned_create(executable, root):
    """c writes a disk around its volume: the volume at 1 MiB is the bare
    volume byte for byte, the tables around it carry the derived or given
    identifiers and settings, the plan reports each layer, and an invalid
    setting publishes nothing."""
    common = ["-R", "-e", "1700000000", "-C", "source", "."]
    invoke(executable, root, ["c", "-F", "fat32", "-f", "bare.img", *common])
    invoke(executable, root, ["c", "-F", "fat32/gpt", "-f", "made-gpt.img", *common])
    bare = (root / "bare.img").read_bytes()
    disk = (root / "made-gpt.img").read_bytes()
    assert disk[1 << 20:(1 << 20) + len(bare)] == bare
    # Epoch 1700000000 is 0x6553f100: its eight little-endian bytes, the
    # role (0x81 disk, 0x82 partition) and the slot, version 4.
    disk_guid = "00f15365-0000-4000-8100-000000000000"
    partition_guid = "00f15365-0000-4000-8201-000000000000"
    info = invoke(executable, root, ["i", "-f", "made-gpt.img"]).stdout.decode()
    assert "Stack:        fat32/gpt\nCompression:  none\nScheme:       GPT, 512-byte sectors\n" \
        in info, info
    assert f"Disk GUID:    {disk_guid}\n" in info, info
    assert re.search(r'Partition 1:  basic, [^\n]* at 1 MiB, fat32, name "Microsoft basic data", '
                     f"GUID {partition_guid}, selected\n", info), info
    assert disk[450] == 0xEE and disk[510:512] == b"\x55\xaa" and disk[512:520] == b"EFI PART"
    assert disk[512 + 56:512 + 72] == uuid.UUID(disk_guid).bytes_le
    assert disk[1024:1040] == uuid.UUID(BASIC_TYPE).bytes_le
    assert disk[1040:1056] == uuid.UUID(partition_guid).bytes_le
    invoke(executable, root, ["cW", "-F", "fat32/gpt", "-f", "made-gpt-2.img", *common])
    same_bytes(root / "made-gpt.img", root / "made-gpt-2.img")
    assert listing(executable, root, "made-gpt.img") == listing(executable, root, "bare.img")

    # The plan reports the disk, the volume's place on it and the volume.
    decoded = completed(invoke(executable, root, [
        "c", "-n", "--json", "-F", "fat16/mbr", "-o", "mbr:disk=64M", *common]))
    assert [record["type"] for record in decoded] == ["stack", "partition", "plan", "summary"]
    stack, partition, plan = decoded[:3]
    assert stack["disk_bytes"] == str(64 << 20) and stack["mbr_disk_id"] == 0x6553F100
    assert partition["offset_bytes"] == str(1 << 20) and partition["mbr_type"] == 6
    assert plan["volume_bytes"] == partition["length_bytes"] == str(63 << 20)

    # Settings: type, name and disk length, the MBR status and identifier,
    # and 4096-byte GPT sectors.
    invoke(executable, root, ["c", "-F", "fat12/gpt", "-f", "esp.img", "-o", "gpt:type=esp",
                              "-o", "gpt:name=EFI", "-o", "gpt:disk=8M", *common])
    assert (root / "esp.img").stat().st_size == 8 << 20
    info = invoke(executable, root, ["i", "-f", "esp.img"]).stdout.decode()
    assert 'Partition 1:  esp, ' in info and 'name "EFI"' in info, info
    invoke(executable, root, ["c", "-F", "fat16/mbr", "-f", "made-mbr.img", "-o", "mbr:active=yes",
                              "-o", "mbr:id=0x0badcafe", *common])
    disk = (root / "made-mbr.img").read_bytes()
    assert disk[440:444] == struct.pack("<I", 0x0BADCAFE) and disk[446] == 0x80 and disk[450] == 0x06
    info = invoke(executable, root, ["i", "-f", "made-mbr.img"]).stdout.decode()
    assert "Disk ID:      0x0badcafe\n" in info and "fat16, active, selected\n" in info, info
    invoke(executable, root, ["c", "-F", "exfat/gpt", "-f", "4kn.img", "-o", "gpt:sector=4096",
                              *common])
    assert (root / "4kn.img").read_bytes()[4096:4104] == b"EFI PART"
    assert b"GPT, 4096-byte sectors" in invoke(executable, root, ["i", "-f", "4kn.img"]).stdout

    # Without -R each identifier is random, version 4.
    for name in ["random-1.img", "random-2.img"]:
        invoke(executable, root, ["c", "-F", "fat12/gpt", "-f", name, "-C", "source", "."])
    guids = [records(invoke(executable, root, ["i", "--json", "-f", name]))[0]["disk_guid"]
             for name in ["random-1.img", "random-2.img"]]
    assert guids[0] != guids[1] and all(uuid.UUID(guid).version == 4 for guid in guids)

    for arguments, detail in [
        (["-F", "qnx4/gpt"], b'the partition type has no GPT GUID "gpt:type"'),
        (["-F", "fat32/gpt", "-o", "gpt:name=" + "x" * 37],
         b'the GPT name exceeds 36 UTF-16 units "gpt:name"'),
        (["-F", "fat12/gpt", "-o", "gpt:disk=1M"],
         b'the disk is too small for a partition "gpt:disk"'),
        (["-F", "fat12/mbr", "-o", "mbr:type=0x85"],
         b'extended and GPT protective types cannot be written "mbr:type"'),
        (["-F", "fat12/mbr", "-o", "mbr:type=0xee"],
         b'extended and GPT protective types cannot be written "mbr:type"'),
        (["-F", "fat12/mbr", "-o", "mbr:type=apfs"],
         b'the partition type has no MBR byte "mbr:type"'),
        (["-F", "fat12/mbr", "-o", "mbr:disk=3T"],
         b'an MBR disk holds at most 2^32 sectors "mbr:disk"'),
    ]:
        result = invoke(executable, root, ["c", "-f", "refused.img", *arguments, *common],
                        expected=1)
        assert detail in result.stderr, (arguments, result.stderr)
        assert not (root / "refused.img").exists()
    for arguments, detail in [
        (["-F", "fat32/mbr", "-o", "gpt:type=esp"],
         b"setting names a layer the stack does not contain"),
        (["-F", "fat32/gpt", "-o", "gpt:guid=00000000-0000-0000-0000-000000000000"],
         b"GUID setting must be RFC 4122 text other than nil"),
        (["-F", "fat32/mbr", "-o", "mbr:id=12"],
         b"setting requires 0x and at most eight hex digits"),
        (["-F", "fat32/mbr", "-o", "mbr:active=1"], b"setting must be yes or no"),
        (["-F", "fat32/gpt", "-o", "gpt:color=red"], b"unknown layer setting"),
    ]:
        result = invoke(executable, root, ["c", "-f", "refused.img", *arguments, *common],
                        expected=2)
        assert detail in result.stderr, (arguments, result.stderr)
    result = invoke(executable, root, ["tf", "made-gpt.img", "-F", "fat32/gpt",
                                       "-o", "gpt:type=esp"], expected=2)
    assert b"setting applies only to creation" in result.stderr, result.stderr


def test_container_create(executable, root):
    """c writes each container; c -f - streams the same bytes to stdout and
    refuses a terminal; -a accepts a container suffix naming the stack's
    container; cW verifies the container before publishing; without -R
    the container identifier is random and differs between runs."""
    for container in ["dmg", "vhd", "simg"]:
        image = f"made.{container}"
        invoke(executable, root, ["cf", image, "-R", "-a", "-F", f"fat12/gpt/{container}",
                                  "-C", "source", "."])
        assert sorted(listing(executable, root, image)) == ['"/HELLO.TXT"', '"/nested"',
                                                    '"/nested/caf\u00e9.txt"'], image
        streamed = invoke(executable, root, ["cf", "-", "-R", "-F", f"fat12/gpt/{container}",
                                             "-C", "source", "."])
        assert streamed.stdout == (root / image).read_bytes(), container
        assert b"output is incomplete" not in streamed.stderr
        verified = invoke(executable, root, ["W", "-d", "-f", image]).stdout
        assert f"{container} container,".encode() in verified, verified
    plan = records(invoke(executable, root, ["c", "-n", "--json", "-R", "-F", "fat12/dmg",
                                              "-C", "source", "."]))
    planned = next(record for record in plan if record["type"] == "plan")
    assert planned["codec"].startswith("zlib ") and int(planned["output_bytes"]) > 0
    invoke(executable, root, ["cWf", "checked.dmg", "-F", "fat12/gpt/dmg", "-C", "source", "."])
    invoke(executable, root, ["cf", "random.dmg", "-F", "fat12/dmg", "-C", "source", "."])
    first = (root / "checked.dmg").read_bytes()[-512 + 64:-512 + 80]
    assert first != (root / "random.dmg").read_bytes()[-512 + 64:-512 + 80]
    # A terminal on stdout never receives an image, and nothing is left.
    primary, secondary = os.openpty()
    try:
        result = subprocess.run(
            [str(executable), "cf", "-", "-F", "fat12/dmg", "-C", "source", "."],
            cwd=root, stdout=secondary, stderr=subprocess.PIPE, timeout=30)
    finally:
        os.close(secondary)
        os.close(primary)
    assert result.returncode == 1 and b"refusing to write an image to a terminal" in result.stderr
    for name in ["made.dmg", "made.vhd", "made.simg", "checked.dmg", "random.dmg"]:
        (root / name).unlink()


def test_compressed_create(executable, root):
    """c -z, and -a with a .gz name, write one gzip member around any stack:
    gzip -t accepts it, its header is fsimg's (no name, the -R epoch as
    MTIME, XFL by level, OS 3), it decodes to the image written without
    gzip, and it reads back as that image. -f - streams the same bytes, cW
    verifies the member before publishing, -n reports gzip, its bound and
    zlib, and gzip:level outside 1 to 9 is refused before anything is
    written."""
    stacks = [("fat12", "gz-bare.img"), ("fat12/gpt", "gz-disk.img"),
              ("fat12/gpt/dmg", "gz-disk.dmg")]
    for stack, name in stacks:
        invoke(executable, root, ["cf", name, "-R", "-F", stack, "-C", "source", "."])
        invoke(executable, root, ["cf", name + ".gz", "-R", "-a", "-F", stack,
                                  "-C", "source", "."])
        packed = (root / (name + ".gz")).read_bytes()
        assert packed[:10] == b"\x1f\x8b\x08\x00" + struct.pack("<I", 315532800) + \
            b"\x00\x03", packed[:10]
        assert gzip.decompress(packed) == (root / name).read_bytes(), stack
        streamed = invoke(executable, root, ["cf", "-", "-z", "-R", "-F", stack,
                                             "-C", "source", "."]).stdout
        assert streamed == packed, stack
        listed = invoke(executable, root, ["tf", name + ".gz"]).stdout
        assert b"/HELLO.TXT" in listed, listed
        (root / name).unlink()
        (root / (name + ".gz")).unlink()
    for level, xfl in [("1", 4), ("9", 2)]:
        invoke(executable, root, ["cf", "level.gz", "-z", "-R", "-F", "fat12",
                                  "-o", f"gzip:level={level}", "-C", "source", "."])
        assert (root / "level.gz").read_bytes()[8] == xfl
        (root / "level.gz").unlink()
    result = invoke(executable, root, ["cf", "level.gz", "-z", "-F", "fat12",
                                       "-o", "gzip:level=10", "-C", "source", "."],
                    expected=1)
    assert b"gzip:level" in result.stderr and not (root / "level.gz").exists()
    invoke(executable, root, ["cWf", "checked.img.gz", "-a", "-F", "fat12/gpt",
                              "-C", "source", "."])
    assert gzip.decompress((root / "checked.img.gz").read_bytes())[510:512] == b"\x55\xaa"
    (root / "checked.img.gz").unlink()
    plan = records(invoke(executable, root, ["c", "-n", "-z", "--json", "-R", "-F",
                                              "fat12/dmg", "-C", "source", "."]))
    stack = next(record for record in plan if record["type"] == "stack")
    planned = next(record for record in plan if record["type"] == "plan")
    assert stack["compression"] == "gzip" and planned["codec"].startswith("zlib ")
    assert int(planned["output_bytes"]) > int(planned["volume_bytes"])

    # An encoded image's worker holds the raw intermediate beside its
    # staging directory, so the coordinator counts both against fds and
    # refuses, naming no source path, a limit the worker would exceed. The
    # operand is one directory of files, which the count covers; a deeper
    # tree holds one more descriptor per level while it is scanned or copied.
    for stack, name in [("fat12", "fds.img.gz"), ("fat12/gpt/dmg", "fds.dmg")]:
        compress = ["-z"] if name.endswith(".gz") else []
        error = failed(invoke(executable, root, [
            "cf", name, *compress, "--json", "-F", stack, "-l", "fds=5",
            "-C", "source", "nested"], expected=1))
        assert error["path"] == "" and error["resource"] == "fds", error
        assert (error["limit"], error["observed"]) == ("5", "6"), error
        invoke(executable, root, ["cf", name, *compress, "-F", stack, "-l", "fds=6",
                                  "-C", "source", "nested"])
        (root / name).unlink()
    # Planning writes no raw intermediate and stages nothing.
    invoke(executable, root, ["c", "-n", "-z", "-F", "fat12/gpt/dmg", "-l", "fds=4",
                              "-C", "source", "nested"])

    # The worker charges every descriptor it holds from the moment it holds
    # it, so scanning nested empty directories counts the raw intermediate
    # and every root not yet scanned: the compressed image needs one
    # descriptor more than the plain one, whatever the operand order.
    (root / "levels/deep/a/b/c").mkdir(parents=True)
    (root / "levels/file").write_bytes(b"x")

    def fewest(name, *arguments):
        for count in range(4, 16):
            result = invoke(executable, root, ["cf", name, "-F", "fat12", "-l",
                                               f"fds={count}", *arguments], expected=None)
            if result.returncode == 0:
                (root / name).unlink()
                return count
            assert failed(result)["resource"] == "fds", result
        raise AssertionError(arguments)

    plain = fewest("levels.img", "--json", "-C", "levels", "deep")
    assert fewest("levels.img.gz", "--json", "-z", "-C", "levels", "deep") == plain + 1
    assert fewest("levels.img", "--json", "-C", "levels", "deep", "file") == plain + 1
    assert fewest("levels.img", "--json", "-C", "levels", "file", "deep") == plain + 1


def test_mtree_export(executable, root):
    """x --mtree publishes a manifest of the extracted selection only after a
    complete extraction: a failed one leaves no file, and an existing name is
    refused before any destination exists. With -k the skipped link and
    FIFO are recorded with their metadata, and each file with the digest of
    the bytes written; the tree and its manifest re-create the image byte
    for byte; two extractions write the same manifest; it is published
    with mode 0600, rejected with -O, and holds one descriptor, which the
    worker receives and the extractor writes through."""
    work = root / "mtree-export"
    work.mkdir()
    (work / "source.mtree").write_text(
        "/set uid=0 gid=0 mode=0644 time=1500000000\n"
        "./sp\\040ace type=dir mode=0750 uid=7 gid=8\n"
        "./sp\\040ace/f\\043 type=file fsimg.content=seeded fsimg.seed=3 "
        "size=70000 fsimg.holes=100:5000\n"
        "./link type=link link=sp\\040ace\n"
        "./fifo type=fifo mode=0620\n")
    invoke(executable, work, ["cf", "q.img", "-F", "qnx6", "-R", "@source.mtree"])

    error = failed(invoke(executable, work, ["xf", "q.img", "--json",
                                             "--mtree=failed.mtree"], expected=1))
    assert error["status"] == "unsupported capability", error
    assert error["output_state"] == "incomplete", error
    assert not (work / "failed.mtree").exists()
    shutil.rmtree(work / "q")

    invoke(executable, work, ["xkf", "q.img", "--mtree=q.mtree"])
    text = (work / "q.mtree").read_text()
    assert text.startswith("#mtree v2.0\n. type=dir\n"), text
    assert (work / "q.mtree").stat().st_mode & 0o777 == 0o600
    written = (work / "q/sp ace/f#").read_bytes()
    assert len(written) == 70000 and written[100:5100] == bytes(5000)
    assert (f"./sp\\040ace/f\\043 type=file mode=0644 uid=0 gid=0 time=315532800.000000000 "
            f"size=70000 sha256digest={hashlib.sha256(written).hexdigest()}\n") in text, text
    assert "./sp\\040ace type=dir mode=0750 uid=7 gid=8 " in text, text
    assert "./link type=link mode=0644 uid=0 gid=0 time=315532800.000000000 " \
        "link=sp\\040ace\n" in text, text
    assert "./fifo type=fifo mode=0620 uid=0 gid=0 time=315532800.000000000\n" in text, text

    other = private_directory(work, "other")
    result = invoke(executable, work, ["xkf", "q.img", "-C", "other", "--mtree=q.mtree"],
                    expected=1)
    assert b"cannot create --mtree file" in result.stderr, result.stderr
    assert not list(other.iterdir())

    invoke(executable, work, ["xkf", "q.img", "-C", "other", "--mtree=again.mtree"])
    assert (work / "again.mtree").read_bytes() == (work / "q.mtree").read_bytes()
    invoke(executable, work, ["cf", "rebuilt.img", "-F", "qnx6", "-R", "-C", "q",
                              "@../q.mtree"])
    same_bytes(work / "q.img", work / "rebuilt.img")

    result = invoke(executable, work, ["xOf", "q.img", "--mtree=o.mtree", "/link"],
                    expected=2)
    assert b"-O rejects" in result.stderr, result.stderr
    result = invoke(executable, work, ["xkf", "q.img", "--mtree="], expected=2)
    assert b"--mtree requires a file name" in result.stderr, result.stderr

    # Names refused before anything is extracted: a dangling symbolic link
    # is never followed, the destination's own new name is taken, and a
    # file inside an -E destination would make it non-empty.
    (work / "dangling.mtree").symlink_to("nowhere")
    names = private_directory(work, "names")
    for arguments, message in [
            (["-C", "names", "--mtree=dangling.mtree"], "cannot create --mtree file"),
            (["-C", "names", "--mtree=names/q"], "cannot create --mtree file"),
            (["-E", "-C", "names", "--mtree=names/inside.mtree"],
             "extraction destination is not empty")]:
        error = failed(invoke(executable, work, ["xkf", "q.img", "--json", *arguments],
                              expected=1))
        assert error["message"] == message, (arguments, error)
        assert error["output_state"] != "incomplete", (arguments, error)
        assert not list(names.iterdir()), arguments
        assert not (work / "nowhere").exists()

    # The manifest has its own bound, -l manifest; a manifest that would
    # outgrow it fails the extraction before the file grows, and nothing is
    # published or left staged.
    private_directory(work, "limited")
    private_directory(work, "limited-human")
    error = failed(invoke(executable, work, [
        "xkf", "q.img", "--json", "-C", "limited", "-l", "manifest=64",
        "--mtree=limited.mtree"], expected=1))
    assert error["resource"] == "manifest" and error["limit"] == "64", error
    assert error["output_state"] == "incomplete", error
    assert not (work / "limited.mtree").exists()
    assert not list(work.glob(".fsimg-*"))
    result = invoke(executable, work, ["xkf", "q.img", "-C", "limited-human", "-l",
                                       "manifest=64", "--mtree=limited.mtree"], expected=1)
    assert b"(manifest limit 64, observed " in result.stderr, result.stderr

    def fewest(*arguments):
        for count in range(3, 16):
            target = private_directory(work, f"fds-{count}-{len(arguments)}")
            result = invoke(executable, work, ["xkf", "q.img", "--json", "-C", target.name,
                                               "-l", f"fds={count}", *arguments],
                            expected=None)
            if result.returncode == 0:
                return count
            assert failed(result)["resource"] == "fds", result
            assert not (work / "fds.mtree").exists()
        raise AssertionError(arguments)

    plain = fewest()
    assert fewest("--mtree=fds.mtree") == plain + 1
    assert not list(work.glob(".fsimg-*"))


def test_mtree_failures(executable, root):
    """However x --mtree fails, the failure is reported, no manifest is
    published and its staging is gone: a host write of the manifest refused
    by the file size limit, which the extracted files fit and the manifest
    outgrows, and a work limit reached while the image is still being
    read, before anything is written."""
    work = root / "mtree-failures"
    work.mkdir()
    name = "n" * 120
    (work / "many.mtree").write_text("".join(
        f"./{name}{index:03} type=file fsimg.content=seeded fsimg.seed={index} size=1\n"
        for index in range(400)))
    invoke(executable, work, ["cf", "many.img", "-F", "qnx6", "-R", "@many.mtree"])

    def limit_file_size():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (32768, 32768))

    private_directory(work, "sized")
    error = failed(invoke(executable, work, [
        "xf", "many.img", "--json", "-C", "sized", "--mtree=sized.mtree"],
        expected=1, preexec_fn=limit_file_size))
    assert error["os_error"] == errno.EFBIG, error
    assert error["output_state"] == "incomplete", error
    assert error["retained_manifest"] == "", error

    private_directory(work, "worked")
    error = failed(invoke(executable, work, [
        "xf", "many.img", "--json", "-C", "worked", "-l", "work=100",
        "--mtree=worked.mtree"], expected=1))
    assert error["resource"] == "work", error
    assert error["retained_manifest"] == "", error
    assert not (work / "sized.mtree").exists() and not (work / "worked.mtree").exists()
    assert not list(work.glob(".fsimg-*"))


def test_mtree_reporting(executable, root):
    """The summary of x --mtree says the manifest was published."""
    work = root / "mtree-reporting"
    work.mkdir()
    shutil.copyfile(root / "human.img", work / "human.img")
    summary = completed(invoke(executable, work, ["xf", "human.img", "--json",
                                                  "--mtree=json.mtree"]))[-1]
    assert summary["published"] is True, summary
    assert (work / "json.mtree").exists()


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="cli-fixture-", dir=Path.cwd()) as temporary:
        root = Path(temporary)
        (root / "source/nested").mkdir(parents=True)
        (root / "source/HELLO.TXT").write_bytes(b"hello\n")
        payload = bytes(index % 251 for index in range(131101))
        (root / "source/nested/caf\u00e9.txt").write_bytes(payload)
        test_human_help(executable, root)
        test_human_commands(executable, root)
        test_bundled_modes(executable, root)
        test_option_syntax(executable, root)
        test_extraction_names(executable, root)
        test_extract_here(executable, root)
        test_extraction_acls(executable, root)
        test_change_directory_kinds(executable, root)
        test_failed_creation_output(executable, root)
        test_human_loss_bounds(executable, root)
        test_formats(executable, root)
        for profile in ["fat12", "fat16", "fat32", "exfat"]:
            test_format(executable, root, profile, payload)
        test_create_operands(executable, root)
        test_posix_metadata(executable, root)
        test_qnx6(executable, root, payload)
        test_qnx4(executable, root, payload)
        test_failures(executable, root)
        test_output_failures(executable, root)
        test_examples(executable, root)
        test_merged_operands(executable, root)
        test_manifest_generated(executable, root)
        test_manifest_host(executable, root)
        test_manifest_refusals(executable, root)
        test_mtree_export(executable, root)
        test_mtree_reporting(executable, root)
        test_mtree_failures(executable, root)
        test_partitioned(executable, root)
        test_containers(executable, root)
        test_compressed_and_streams(executable, root)
        test_partitioned_create(executable, root)
        test_container_create(executable, root)
        test_compressed_create(executable, root)
        assert not list(root.glob(".fsimg-*"))
    print("CLI commands, independent JSON decoding, publication and failure checks pass")


if __name__ == "__main__":
    main()
