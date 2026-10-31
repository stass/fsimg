# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
"""Measure the complete CLI process group on a generated disk-backed tree."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    executable = Path(sys.argv[1]).resolve()
    files = int(sys.argv[2]) if len(sys.argv) == 3 else 100000
    assert 0 < files <= 100000
    root = Path(tempfile.mkdtemp(prefix="qualification-cli-", dir=Path.cwd()))
    program = root / "fsimg-rss"
    shutil.copy2(executable, program)
    source = root / "source"
    source.mkdir(mode=0o700)
    scratch = root / "scratch"
    scratch.mkdir(mode=0o700)
    image = root / "image.img"
    # Extraction creates a directory named after the image beside it.
    destination = root / "image"

    # Grouping bounds host directory width independently of total entries.
    for first in range(0, files, 1000):
        directory = source / f"d{first // 1000:08d}"
        directory.mkdir(mode=0o700)
        for ordinal in range(first, min(files, first + 1000)):
            with (directory / f"f{ordinal:08d}").open("xb"):
                pass
    marker = bytes(range(256)) + b"!"
    (source / "MARKER.BIN").write_bytes(marker)
    sandbox = "auto" if sys.platform == "darwin" else "required"
    common = ["-S", sandbox, "-T", str(scratch), "-l", "seconds=600"]
    commands = [
        ["c", "--json", "-R", "-F", "exfat", "-f", str(image), *common, "-C", str(source), "."],
        ["i", "--json", "-f", str(image), *common],
        ["t", "--json", "-f", str(image), *common],
        ["x", "-O", "-f", str(image), *common, "/MARKER.BIN"],
        ["W", "--json", "-d", "-f", str(image), *common],
        ["x", "--json", "-f", str(image), *common, "-C", str(root)],
    ]
    try:
        for index, command in enumerate(commands):
            diagnostic_path = root / f"{index}-{command[0]}.log"
            with diagnostic_path.open("wb") as diagnostics:
                result = subprocess.run(
                    [str(program), "16777216", *command],
                    stdout=subprocess.DEVNULL, stderr=diagnostics, timeout=900,
                )
            measurements = 0
            with diagnostic_path.open("rb") as diagnostics:
                for line in diagnostics:
                    if line.startswith(b"CLI RSS "):
                        print(line.decode("ascii").strip(), flush=True)
                        measurements += 1
            result.check_returncode()
            assert measurements == 1
        assert (destination / "MARKER.BIN").read_bytes() == marker
        for first in range(0, files, 1000):
            directory = destination / f"d{first // 1000:08d}"
            for ordinal in range(first, min(files, first + 1000)):
                path = directory / f"f{ordinal:08d}"
                assert path.is_file() and path.stat().st_size == 0
        assert not list(scratch.iterdir())
    except BaseException:
        print(f"CLI qualification fixtures retained: {root}", file=sys.stderr)
        raise
    shutil.rmtree(root)
    print(f"CLI disk-backed source files={files + 1}; all six operations satisfy RSS allowance")


if __name__ == "__main__":
    main()
