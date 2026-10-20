# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
"""Exercise the installed command contract through independent JSON decoding."""

import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def invoke(executable, root, arguments, expected=0, **kwargs):
    result = subprocess.run(
        [str(executable), *arguments], cwd=root, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=30, **kwargs,
    )
    assert result.returncode == expected, (arguments, result.returncode, result.stderr)
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
    formats = {record["name"]: record for record in decoded if record["type"] == "format"}
    assert set(formats) == {"fat12", "fat16", "fat32", "exfat", "qnx6", "qnx4"}
    assert all(record["capabilities"] == 7 for record in formats.values())
    assert formats["exfat"]["write_cluster_bytes"][-1] == 32 * 1024 * 1024
    assert isinstance(formats["exfat"]["max_file_bytes"], str)
    capabilities = next(record for record in decoded if record["type"] == "capabilities")
    assert capabilities["sandbox_runtime"] in {"unavailable", "not-probed"}
    assert capabilities["source_device_open_exclusion"] is False
    assert b"usage: fsimg" in invoke(executable, root, ["-h"]).stdout
    assert invoke(executable, root, ["-V"]).stdout.startswith(b"fsimg ")


def test_posix_metadata(executable, root):
    """FAT and exFAT store no POSIX fields or links: -p imports become counted
    losses that leave image bytes unchanged, and a link fails planning. The
    QNX6 and QNX4 formats store every kind and the POSIX fields."""
    decoded = completed(invoke(executable, root, ["--formats", "--json"]))
    formats = {record["name"]: record for record in decoded if record["type"] == "format"}
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
    info = completed(invoke(executable, root, ["i", "--json", "-f", "posix.img"]))[0]
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
    assert formats.stdout == (b"Format  Read  Write  Verify\n"
                              b"fat12   yes   yes    yes\n"
                              b"fat16   yes   yes    yes\n"
                              b"fat32   yes   yes    yes\n"
                              b"exfat   yes   yes    yes\n"
                              b"qnx6    yes   yes    yes\n"
                              b"qnx4    yes   yes    yes\n")
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
    assert information[0]["format"] == "fat32" and information[0]["stack"] == "fat32"
    assert information[0]["compression"] is None and information[-1]["command"] == "i"
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
         b"-O rejects --json, -C, -E and -k"),
        (["x", "-O", "-E", "-f", "human.img", "/HELLO.TXT"], b"-O rejects --json, -C, -E and -k"),
        (["x", "-O", "-k", "-f", "human.img", "/HELLO.TXT"], b"-O rejects --json, -C, -E and -k"),
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
    ], expected=2))["message"] == "-O rejects --json, -C, -E and -k"
    for arguments, detail in [
        # Options precede operands: after the first member, -v is a member.
        (["t", "-f", "human.img", "/nested", "-v"], b"selecting several members is not supported"),
        (["t", "-z", "-f", "human.img"], b"compressed images are not supported"),
        (["t", "--partition=1", "-f", "human.img"], b"partition selection is not supported"),
        (["t", "-f", "-"], b"reading an image from standard input is not supported"),
        (["t", "-F", "fat32/gpt", "-f", "human.img"], b"partition schemes are not supported"),
        (["cf", "syntax.img", "-F", "fat32/dmg", "-C", "source", "."],
         b"image containers are not supported"),
        (["cf", "syntax.img", "-a", "-F", "fat32", "-C", "source", "."],
         b"compressed images are not supported"),
        (["cf", "syntax.img", "-M", "-F", "fat32", "-C", "source", "."],
         b"-M minimization is not supported"),
        (["cf", "syntax.img", "-F", "apfs", "-C", "source", "."],
         b"the named filesystem is reserved and not supported"),
        (["cf", "-", "-F", "fat32", "-C", "source", "."],
         b"writing an image to standard output is not supported"),
    ]:
        result = invoke(executable, root, arguments, expected=1)
        assert detail in result.stderr and result.stdout == b"", (arguments, result.stderr)
    assert not (root / "syntax.img").exists() and not (root / "-").exists()
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
    symbolic link is not followed."""
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
    assert not list((root / "into-target").iterdir())
    result = invoke(executable, root, ["xEf", "human.img"], expected=1)
    assert b'extraction destination is not empty "."' in result.stderr


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
    info = information[0]
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
    failed(invoke(executable, root, ["i", "--json", "-f", "-"], expected=1))
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
    assert failed(invoke(executable, root, [
        "i", "--json", "-f", "exfat.img", "--offset=1T",
    ], expected=1))["code"] == -7

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

    info = completed(invoke(executable, root, ["i", "--json", "-f", "qnx6.img"]))[0]
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
        "i", "--json", "-f", "qnx6-random.img"]))[0]["uuid"]
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

    info = completed(invoke(executable, root, ["i", "--json", "-f", "qnx4.img"]))[0]
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
        assert not list(root.glob(".fsimg-*"))
    print("CLI commands, independent JSON decoding, publication and failure checks pass")


if __name__ == "__main__":
    main()
