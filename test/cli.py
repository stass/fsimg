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
    assert set(formats) == {"fat12", "fat16", "fat32", "exfat"}
    assert all(record["capabilities"] == 7 for record in formats.values())
    assert formats["exfat"]["write_cluster_bytes"][-1] == 32 * 1024 * 1024
    assert isinstance(formats["exfat"]["max_file_bytes"], str)
    capabilities = next(record for record in decoded if record["type"] == "capabilities")
    assert capabilities["sandbox_runtime"] in {"unavailable", "not-probed"}
    assert capabilities["source_device_open_exclusion"] is False
    assert b"Usage: fsimg" in invoke(executable, root, ["-h"]).stdout
    assert invoke(executable, root, ["-V"]).stdout.startswith(b"fsimg ")


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
        assert result.returncode == 1 and b"I/O failure" in result.stderr
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


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="cli-fixture-", dir=Path.cwd()) as temporary:
        root = Path(temporary)
        (root / "source/nested").mkdir(parents=True)
        (root / "source/HELLO.TXT").write_bytes(b"hello\n")
        payload = bytes(index % 251 for index in range(131101))
        (root / "source/nested/caf\u00e9.txt").write_bytes(payload)
        test_formats(executable, root)
        for profile in ["fat12", "fat16", "fat32", "exfat"]:
            test_format(executable, root, profile, payload)
        test_failures(executable, root)
        test_output_failures(executable, root)
        test_examples(executable, root)
        assert not list(root.glob(".fsimg-*"))
    print("CLI commands, independent JSON decoding, publication and failure checks pass")


if __name__ == "__main__":
    main()
