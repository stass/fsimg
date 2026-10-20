# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
"""Exercise the installed command contract through independent JSON decoding."""

import json
import os
from pathlib import Path
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


def test_formats(executable, root):
    decoded = completed(invoke(executable, root, ["formats", "-j"]))
    formats = {record["name"]: record for record in decoded if record["type"] == "format"}
    assert set(formats) == {"fat12", "fat16", "fat32", "exfat", "qnx6", "qnx4"}
    assert formats.pop("qnx4")["capabilities"] == 5
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
    QNX6 format stores every kind and the POSIX fields."""
    decoded = completed(invoke(executable, root, ["formats", "-j"]))
    formats = {record["name"]: record for record in decoded if record["type"] == "format"}
    qnx6 = formats.pop("qnx6")
    assert qnx6["entry_kinds"] == 15 and qnx6["metadata_fields"] == 0x77
    assert qnx6["max_label_bytes"] == 0 and qnx6["write_settings"] == 15
    qnx4 = formats.pop("qnx4")
    assert qnx4["entry_kinds"] == 15 and qnx4["metadata_fields"] == 0x77
    assert qnx4["max_label_bytes"] == 48 and qnx4["write_settings"] == 0
    assert all(record["write_settings"] == 0 for record in formats.values())
    assert all(record["entry_kinds"] == 3 for record in formats.values())
    assert all(record["metadata_fields"] == 15 for record in formats.values())
    assert formats["fat16"]["max_label_bytes"] == 11
    assert formats["exfat"]["max_label_bytes"] == 22

    plans = []
    for flags in [[], ["-p"]]:
        decoded = completed(invoke(executable, root, [
            "create", "-n", "-j", "-R", *flags, "-t", "fat16", "source",
        ]))
        plans.append(next(record for record in decoded if record["type"] == "plan"))
    # Three entries each lose an owner and a mode.
    assert int(plans[1]["metadata_losses"]) == int(plans[0]["metadata_losses"]) + 6
    completed(invoke(executable, root, [
        "create", "-j", "-R", "-p", "-t", "fat16", "-o", "posix.img", "source",
    ]))
    same_bytes(root / "fat16.img", root / "posix.img")

    listed = completed(invoke(executable, root, ["ls", "-j", "-r", "-i", "posix.img"]))
    entries = {record["path"]: record for record in listed if record["type"] == "entry"}
    assert entries["/nested"]["kind"] == "directory"
    assert entries["/HELLO.TXT"]["kind"] == "file"
    assert all(record["uid"] is None and record["mode"] is None and
               record["changed"] is None for record in entries.values())
    info = completed(invoke(executable, root, ["info", "-j", "-i", "posix.img"]))[0]
    assert info["uuid"] is None
    assert b"UUID" not in invoke(executable, root, ["info", "-i", "posix.img"]).stdout

    linked = root / "linked-source"
    linked.mkdir()
    (linked / "target").write_bytes(b"data")
    os.symlink("target", linked / "link")
    # Unconfined, the scan imports the link and planning rejects its kind.
    error = failed(invoke(executable, root, [
        "create", "-n", "-j", "-S", "off", "-t", "fat16", "linked-source",
    ], expected=1))
    assert error["code"] == -6 and error["path"] == "/link"
    assert "entry kind" in error["message"]
    human = invoke(executable, root, [
        "create", "-n", "-S", "off", "-t", "fat16", "linked-source",
    ], expected=1)
    assert b"cannot store this entry kind" in human.stderr
    # A confined worker may be denied link inspection; either way the
    # failure is unsupported and names the link.
    error = failed(invoke(executable, root, [
        "create", "-n", "-j", "-t", "fat16", "linked-source",
    ], expected=1))
    assert error["code"] == -6 and error["path"] == "/link"
    assert ("entry kind" in error["message"] or
            "link inspection is not permitted" in error["message"])

    skipped = completed(invoke(executable, root, [
        "extract", "-j", "-k", "-i", "posix.img", "-C", "posix-extracted",
    ]))
    assert skipped[-1]["skipped_entries"] == "0"
    invoke(executable, root, ["ls", "-k", "-i", "posix.img"], expected=2)


def test_human_help(executable, root):
    empty = invoke(executable, root, [], expected=2)
    assert empty.stdout == b"" and empty.stderr.startswith(b"usage: fsimg")
    assert b"commands:" in empty.stderr and b"errno" not in empty.stderr
    help_output = invoke(executable, root, ["-h"])
    assert help_output.stdout == empty.stderr and help_output.stderr == b""
    for command in ["create", "info", "ls", "cat", "extract", "verify", "formats"]:
        help_output = invoke(executable, root, [command, "-h"])
        assert help_output.stdout.startswith(f"usage: fsimg {command} ".encode())
        assert b"options:" in help_output.stdout and b"example" in help_output.stdout
        assert b"commands:" not in help_output.stdout and help_output.stderr == b""
    missing = invoke(executable, root, ["info"], expected=2)
    assert missing.stderr == b"fsimg: info requires -i image\nusage: fsimg info [options] -i image\n"
    for arguments, detail in [
        (["info", "-Q"], b"unknown option -Q"),
        (["info", "-i"], b"option -i requires an argument"),
        (["no-such-command"], b'unknown command "no-such-command"'),
        ([""], b'unknown command ""'),
    ]:
        result = invoke(executable, root, arguments, expected=2)
        assert detail in result.stderr and b"usage:" in result.stderr
        assert b"errno" not in result.stderr and result.stdout == b""
    formats = invoke(executable, root, ["formats"])
    assert formats.stdout == (b"Format  Read  Write  Verify\n"
                              b"fat12   yes   yes    yes\n"
                              b"fat16   yes   yes    yes\n"
                              b"fat32   yes   yes    yes\n"
                              b"exfat   yes   yes    yes\n"
                              b"qnx6    yes   yes    yes\n"
                              b"qnx4    yes   no     yes\n")
    assert formats.stderr == b""
    verbose = invoke(executable, root, ["formats", "-v"])
    assert verbose.stdout == formats.stdout and b"Sandbox backend:" in verbose.stderr
    hostile_command = invoke(executable, root, ["\x1b\n" * 12000], expected=2)
    assert b"unknown command" in hostile_command.stderr and b"truncated" in hostile_command.stderr
    assert b"usage:" in hostile_command.stderr and b"\x1b" not in hostile_command.stderr
    assert len(hostile_command.stderr) < 4096


def test_human_commands(executable, root):
    image = "human.img"
    created = invoke(executable, root, ["create", "-t", "fat12", "-o", image, "source"])
    assert created.stdout == b""
    assert created.stderr.count(b"metadata fields") == 1
    assert b"fields=0x" not in created.stderr and b"use -v" in created.stderr
    if b"sandbox unavailable" in created.stderr:
        assert created.stderr.count(b"sandbox unavailable") == 1
    listed = invoke(executable, root, ["ls", "-r", "-i", image])
    assert listed.stdout.decode().splitlines() == ['"/HELLO.TXT"', '"/nested"', '"/nested/caf\u00e9.txt"']
    info = invoke(executable, root, ["info", "-i", image])
    assert b"Format:       FAT12\n" in info.stdout
    assert b"Validation:   header only\n" in info.stdout
    assert b"Free space:   unknown (not scanned)\n" in info.stdout
    assert b"complete:" not in info.stdout and b"profile=" not in info.stdout
    plan = invoke(executable, root, ["create", "-n", "-R", "-t", "fat12", "source"])
    assert b"Volume size:" in plan.stdout and b"Metadata:" in plan.stdout
    assert b"source profile" not in plan.stdout and b"complete:" not in plan.stdout
    verbose = invoke(executable, root, ["create", "-v", "-t", "fat12", "-o", "human-verbose.img", "source"])
    assert verbose.stdout == b"" and b"Volume size:" in verbose.stderr
    assert verbose.stderr.count(b"source profile requires") == 1
    assert b"metadata example:" in verbose.stderr and b"modification time" in verbose.stderr
    assert b"fields=0x" not in verbose.stderr
    verified = invoke(executable, root, ["verify", "-i", image])
    assert verified.stdout == b'"human.img": filesystem structure verified (2 files, 2 directories)\n'
    payload = invoke(executable, root, ["verify", "-d", "-i", image])
    assert b"filesystem structure and initialized file data verified" in payload.stdout
    extracted = invoke(executable, root, ["extract", "-i", image, "-C", "human-output"])
    assert extracted.stdout == b"" and extracted.stderr.count(b"metadata fields") == 1
    same_bytes(root / "source/nested/caf\u00e9.txt", root / "human-output/nested/caf\u00e9.txt")
    raw = invoke(executable, root, ["cat", "-i", image, "/HELLO.TXT"])
    assert raw.stdout == b"hello\n"
    missing = invoke(executable, root, ["info", "-i", "missing\n\x1b.img"], expected=1)
    assert missing.stdout == b"" and b'cannot open image "missing\\u000a\\u001b.img"' in missing.stderr
    assert b"No such file or directory" in missing.stderr and b"errno=" not in missing.stderr
    assert b"\x1b" not in missing.stderr and len(missing.stderr.splitlines()) == 1
    geometry = invoke(executable, root, ["create", "-R", "-t", "fat32", "-s", "8M", "-o", "small.img", "source"], expected=1)
    assert b"too small for FAT32" in geometry.stderr and b"8388608 bytes" in geometry.stderr
    assert b"errno" not in geometry.stderr and not (root / "small.img").exists()
    for settings, explanation in [
        (["-t", "fat12", "-s", "1G"], b"too large for FAT12"),
        (["-t", "fat12", "-s", "1M", "-f", "1M"], b"cannot fit the source tree and requested free space"),
        (["-t", "fat12", "-f", "1G"], b"source tree and requested free space are too large for FAT12"),
        (["-t", "fat32", "-O", "cluster=32K", "-f", "4T"], b"required image exceeds FAT32 size limits"),
        (["-t", "fat32", "-f", "4T"], b"requested size and allocation settings conflict"),
    ]:
        rejected = invoke(executable, root, ["create", "-n", "-R", *settings, "source"], expected=1)
        assert explanation in rejected.stderr, (settings, rejected.stderr)
        assert b"image size 0" not in rejected.stderr
        assert b"free-space reserve 0" not in rejected.stderr


def test_failed_creation_output(executable, root):
    source = root / "oversized-source"
    source.mkdir()
    # The sparse payload exceeds the FAT12 writer's capacity. Planning must
    # reject it before any image creation or payload copying. Reproducible
    # timestamp conversion emits loss examples before geometry rejection.
    with (source / "large.bin").open("wb") as payload:
        payload.truncate(256 * 1024 * 1024)
    message = b"fsimg: source tree is too large for FAT12\n"
    arguments = ["create", "-R", "-t", "fat12", "-o", "oversized.img"]
    rejected = invoke(executable, root, [*arguments, "oversized-source"], expected=1)
    assert rejected.stdout == b"" and rejected.stderr.endswith(message)
    lines = rejected.stderr.splitlines(keepends=True)
    assert lines[-1] == message
    assert all(line.startswith(b"fsimg: warning: OS sandbox") for line in lines[:-1])
    assert b"metadata" not in rejected.stderr and b"0 bytes" not in rejected.stderr
    assert not (root / "oversized.img").exists()
    assert not list(root.glob(".fsimg-*"))

    verbose = invoke(executable, root, [*arguments, "-v", "oversized-source"], expected=1)
    assert b"metadata example:" in verbose.stderr and verbose.stderr.endswith(message)
    assert b"final count unavailable" not in verbose.stderr

    decoded = failed(invoke(executable, root, [*arguments, "-j", "oversized-source"], expected=1))
    assert decoded["code"] == -7 and decoded["output_state"] == "absent"
    assert decoded["message"] == "source tree is too large for FAT12"
    assert decoded["resource"] == "none" and decoded["limit"] == "0"
    constrained = invoke(executable, root, [
        *arguments, "-O", "cluster=512", "oversized-source",
    ], expected=1)
    assert b"source tree is too large for FAT12 with 512-byte clusters" in constrained.stderr
    small = invoke(executable, root, [
        "create", "-n", "-R", "-t", "fat32", "-s", "8M", "-O", "cluster=512", "oversized-source",
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
    ordinary = invoke(executable, root, ["create", "-n", "-R", "-t", "fat12", "loss-source"])
    assert notice in ordinary.stderr
    assert b"metadata example:" not in ordinary.stderr
    verbose = invoke(executable, root, ["create", "-n", "-v", "-R", "-t", "fat12", "loss-source"])
    assert notice in verbose.stderr
    assert verbose.stderr.count(b"metadata example:") == 32
    assert b"fields=0x" not in verbose.stderr


def test_format(executable, root, profile, payload):
    image = f"{profile}.img"
    decoded = completed(invoke(executable, root, [
        "create", "-j", "-R", "-t", profile, "-o", image, "source",
    ]))
    plan = next(record for record in decoded if record["type"] == "plan")
    assert plan["format"] == profile and int(plan["payload_bytes"]) == len(payload) + 6
    assert decoded[-1]["published"] is True
    assert (root / image).stat().st_mode & 0o077 == 0

    information = completed(invoke(executable, root, ["info", "-j", "-T", "-", "-i", image]))
    info = information[0]
    assert info["format"] == profile and info["label"] == "FSIMG"
    assert info["validation_scope"] == "header" and info["entry_count"] is None
    assert info["free_bytes"] is None

    listed = completed(invoke(executable, root, ["ls", "-j", "-r", "-i", image]))
    entries = {record["path"]: record for record in listed if record["type"] == "entry"}
    assert set(entries) == {"/HELLO.TXT", "/nested", "/nested/caf\u00e9.txt"}
    assert all(record["complete"] for record in entries.values())
    assert isinstance(entries["/nested/caf\u00e9.txt"]["size_bytes"], str)
    modified = entries["/HELLO.TXT"]["modified"]
    assert modified["basis"] == ("utc" if profile == "exfat" else "wall-clock")
    assert invoke(executable, root, ["cat", "-i", image, "/NESTED/CAF\u00c9.TXT"]).stdout == payload

    destination = f"extracted-{profile}"
    completed(invoke(executable, root, ["extract", "-j", "-i", image, "-C", destination]))
    same_bytes(root / "source/nested/caf\u00e9.txt", root / destination / "nested/caf\u00e9.txt")
    assert (root / destination / "HELLO.TXT").read_bytes() == b"hello\n"
    assert (root / destination).stat().st_mode & 0o077 == 0

    verification = completed(invoke(executable, root, ["verify", "-j", "-d", "-i", image]))
    report = next(record for record in verification if record["type"] == "verify")
    assert report["validation_scope"] == "initialized-payload" and report["files"] == "2"
    assert int(report["initialized_bytes_read"]) == len(payload) + 6

    second = f"{profile}-second.img"
    completed(invoke(executable, root, [
        "create", "-j", "-R", "-m", "16M", "-t", profile, "-o", second, "source",
    ]))
    same_bytes(root / image, root / second)

    before = (root / image).stat()
    error = failed(invoke(executable, root, [
        "create", "-j", "-R", "-t", profile, "-o", image, "source",
    ], expected=1))
    assert error["code"] == -12 and (root / image).stat().st_ino == before.st_ino
    assert failed(invoke(executable, root, [
        "extract", "-j", "-i", image, "-C", destination,
    ], expected=1))["code"] == -12


def test_failures(executable, root):
    for arguments in [
        ["info", "-j", "-i", "-"],
        ["cat", "-j", "-i", "exfat.img", "/HELLO.TXT"],
        ["create", "-j", "-t", "exfat", "-e", "315532800", "-n", "source"],
        ["info", "-j", "-i", "exfat.img", "-l", "work=1", "-l", "work=1"],
        ["info", "-j", "-i", "exfat.img", "-l", "seconds=18446744073709551615"],
        ["create", "-j", "-n", "-t", "exfat", "-L", "A" * 48, "source"],
    ]:
        failed(invoke(executable, root, arguments, expected=2))

    planned = completed(invoke(executable, root, [
        "create", "-j", "-n", "-R", "-t", "exfat", "-o", "unused.img", "source",
    ]))
    assert planned[-1]["validation_scope"] == "planned" and not (root / "unused.img").exists()
    assert failed(invoke(executable, root, [
        "verify", "-j", "-i", "exfat.img", "-l", "work=1",
    ], expected=1))["resource"] == "work"
    assert failed(invoke(executable, root, [
        "ls", "-j", "-i", "exfat.img", "-l", "path=4", "/abc",
    ], expected=1))["resource"] == "path"
    assert failed(invoke(executable, root, [
        "info", "-j", "-i", "exfat.img", "-b", "1T",
    ], expected=1))["code"] == -7

    (root / "truncated.img").write_bytes(bytes(511))
    failed(invoke(executable, root, ["info", "-j", "-i", "truncated.img"], expected=1))
    os.mkfifo(root / "stream")
    failed(invoke(executable, root, ["info", "-j", "-i", "stream"], expected=1))

    (root / "unsafe-source").mkdir()
    (root / "sentinel").write_bytes(b"outside")
    (root / "unsafe-source/pivot").symlink_to("../sentinel")
    error = failed(invoke(executable, root, [
        "create", "-j", "-R", "-t", "exfat", "-o", "unsafe.img", "unsafe-source",
    ], expected=1))
    assert error["output_state"] == "absent" and not (root / "unsafe.img").exists()
    assert (root / "sentinel").read_bytes() == b"outside"

    (root / "output-pivot").symlink_to("sentinel")
    failed(invoke(executable, root, [
        "extract", "-j", "-i", "exfat.img", "-C", "output-pivot",
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
        "verify", "-j", "-d", "-b", "4096", "-z", volume_bytes, "-i", "sliced.img",
    ]))


def test_output_failures(executable, root):
    read_fd, write_fd = os.pipe()
    os.close(read_fd)
    try:
        result = subprocess.run([
            str(executable), "cat", "-i", "exfat.img", "/nested/caf\u00e9.txt",
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
            str(executable), "formats", "-j",
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
    completed(invoke(executable, root, ["verify", "-j", "-d", "-i", str(output / "tree.img")]))
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
    arguments = ["create", "-j", "-R", "-p", "-S", "off", "-t", "qnx6",
                 "-O", "cluster=2K", "-O", "endian=big", "-O", "uuid=" + uuid.upper(),
                 "-O", "inodes=100", "-O", "reserve=0"]
    decoded = completed(invoke(executable, root, [*arguments, "-o", "qnx6.img",
                                                  "qnx6-source"]))
    plan = next(record for record in decoded if record["type"] == "plan")
    assert plan["format"] == "qnx6" and plan["cluster_bytes"] == 2048
    image = (root / "qnx6.img").read_bytes()
    assert image[8192:8196] == bytes.fromhex("68191122")
    assert image[8192 + 32:8192 + 48] == bytes.fromhex(uuid.replace("-", ""))
    assert int.from_bytes(image[8192 + 52:8192 + 56], "big") == 112

    info = completed(invoke(executable, root, ["info", "-j", "-i", "qnx6.img"]))[0]
    assert info["format"] == "qnx6" and info["uuid"] == uuid
    listed = completed(invoke(executable, root, ["ls", "-j", "-r", "-i", "qnx6.img"]))
    entries = {record["path"]: record for record in listed if record["type"] == "entry"}
    assert set(entries) == {"/.boot", "/link", "/nested", "/nested/caf\u00e9.txt"}
    assert entries["/link"]["kind"] == "symlink"
    assert entries["/nested/caf\u00e9.txt"]["mode"] == 0o640
    assert invoke(executable, root, ["cat", "-i", "qnx6.img", "/nested/caf\u00e9.txt"]).stdout == payload
    assert invoke(executable, root, ["cat", "-i", "qnx6.img", "/link"]).stdout == \
        "nested/caf\u00e9.txt".encode()
    verification = completed(invoke(executable, root, ["verify", "-j", "-d", "-i", "qnx6.img"]))
    report = next(record for record in verification if record["type"] == "verify")
    assert report["validation_scope"] == "initialized-payload"

    completed(invoke(executable, root, [*arguments, "-m", "16M", "-o", "qnx6-second.img",
                                        "qnx6-source"]))
    same_bytes(root / "qnx6.img", root / "qnx6-second.img")
    # Without -R and uuid= the volume gets a random version 4 UUID.
    completed(invoke(executable, root, ["create", "-j", "-S", "off", "-t", "qnx6",
                                        "-o", "qnx6-random.img", "qnx6-source"]))
    random_uuid = completed(invoke(executable, root, [
        "info", "-j", "-i", "qnx6-random.img"]))[0]["uuid"]
    assert random_uuid[14] == "4" and random_uuid[19] in "89ab"
    error = failed(invoke(executable, root, [
        "create", "-n", "-j", "-t", "qnx6", "-L", "LABEL", "qnx6-source",
    ], expected=1))
    assert error["code"] == -6


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
        test_failed_creation_output(executable, root)
        test_human_loss_bounds(executable, root)
        test_formats(executable, root)
        for profile in ["fat12", "fat16", "fat32", "exfat"]:
            test_format(executable, root, profile, payload)
        test_posix_metadata(executable, root)
        test_qnx6(executable, root, payload)
        test_failures(executable, root)
        test_output_failures(executable, root)
        test_examples(executable, root)
        assert not list(root.glob(".fsimg-*"))
    print("CLI commands, independent JSON decoding, publication and failure checks pass")


if __name__ == "__main__":
    main()
