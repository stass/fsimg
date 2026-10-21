# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
"""Validate installed metadata without selecting a different installed package."""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def matches_path_flag(flag, marker, expected):
    if not flag.startswith(marker):
        return False
    path = Path(flag[len(marker):])
    # Installed flags must work from every consumer's working directory.
    return path.is_absolute() and path.resolve() == expected.resolve()


def validate_metadata(pkg_config, prefix, version):
    environment = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "LC_ALL": "C",
        "PKG_CONFIG_PATH": "",
        "PKG_CONFIG_LIBDIR": str(prefix / "lib/pkgconfig"),
    }

    def query(option):
        return subprocess.check_output(
            [pkg_config, option, "libfsimg"], env=environment, text=True,
        ).strip()

    if query("--modversion") != version:
        raise RuntimeError("installed package version differs from the command")
    cflags = shlex.split(query("--cflags"))
    libraries = shlex.split(query("--libs"))
    if len(cflags) != 1 or not matches_path_flag(cflags[0], "-I", prefix / "include"):
        raise RuntimeError(f"unexpected installed Cflags: {cflags!r}")
    if len(libraries) != 2 or \
            not matches_path_flag(libraries[0], "-L", prefix / "lib") or \
            libraries[1] != "-lfsimg":
        raise RuntimeError(f"unexpected installed Libs: {libraries!r}")


def self_test(pkg_config):
    with tempfile.TemporaryDirectory(prefix="fsimg-pc-") as temporary:
        prefix = Path(temporary) / "prefix with spaces"
        directory = prefix / "lib/pkgconfig"
        directory.mkdir(parents=True)
        metadata = directory / "libfsimg.pc"
        good = (Path(__file__).resolve().parent.parent / "libfsimg.pc.in").read_text()
        # Equivalent path spellings arise when TMPDIR already ends in '/'.
        spelled_prefix = str(prefix.parent) + "//" + prefix.name
        replacements = {"@prefix@": spelled_prefix, "@exec_prefix@": "${prefix}",
                        "@libdir@": "${exec_prefix}/lib", "@includedir@": "${prefix}/include",
                        "@PACKAGE_VERSION@": "1.2.3",
                        "@ZLIB_LIBS@": "-lz"}
        for placeholder, value in replacements.items():
            good = good.replace(placeholder, value)
        metadata.write_text(good)
        validate_metadata(pkg_config, prefix, "1.2.3")
        for old, replacement in [
            ('Cflags: -I"${includedir}"', 'Cflags: -I/another/include'),
            ('Libs: -L"${libdir}" -lfsimg', 'Libs: -L/another/lib -lfsimg'),
            ('Cflags: -I"${includedir}"',
             'Cflags: -I"' + os.path.relpath(prefix / "include") + '"'),
            ('Libs: -L"${libdir}" -lfsimg',
             'Libs: -L"' + os.path.relpath(prefix / "lib") + '" -lfsimg'),
            ('-lfsimg\n', '-lfsimg -lcrypto\n'),
        ]:
            assert old in good
            metadata.write_text(good.replace(old, replacement))
            try:
                validate_metadata(pkg_config, prefix, "1.2.3")
            except RuntimeError:
                pass
            else:
                raise AssertionError("malformed installed metadata was accepted")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--self-test":
        self_test(sys.argv[2])
        print("installed-metadata rejection checks pass")
        return
    pkg_config, prefix_text = sys.argv[1:]
    prefix = Path(prefix_text)
    required = ["include/fsimg.h", "lib/libfsimg.a", "share/man/man1/fsimg.1",
                "share/man/man3/libfsimg.3", "share/fsimg/examples/provider.c"]
    for relative in required:
        if not (prefix / relative).is_file():
            raise RuntimeError(f"installed artifact is missing: {relative}")
    shared = "lib/libfsimg.0.dylib" if sys.platform == "darwin" else "lib/libfsimg.so.0"
    if not (prefix / shared).is_file():
        raise RuntimeError("shared library is missing from the qualification build")
    version = subprocess.check_output([str(prefix / "bin/fsimg"), "-V"], text=True).strip()
    if not version.startswith("fsimg "):
        raise RuntimeError("installed command returned an invalid version")
    validate_metadata(pkg_config, prefix, version.removeprefix("fsimg "))
    print("installed package paths, version, Cflags and Libs pass")


if __name__ == "__main__":
    main()
