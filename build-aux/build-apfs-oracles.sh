#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
set -eu

usage='usage: build-apfs-oracles.sh apfsprogs|libfsapfs|apfs-fuse [new-directory]'
if test "$#" -lt 1 || test "$#" -gt 2; then
    echo "$usage" >&2
    exit 2
fi
tool=$1

# The APFS development oracles. Each is an external program that checks or
# reads images fsimg produces; none is linked into, copied into or required
# by fsimg. The directory is created exclusively so an existing build is
# never overwritten, and defaults to a per-tool directory beside the other
# oracles. Everything fetched is pinned: a git revision, or a release archive
# with its SHA-256 where the upstream git tree is not self-contained.
apfsprogs_revision=b9c4cb404f88ffab6c4f260bbab29a8fb4784907
libfsapfs_release=20261030
libfsapfs_sha256=c727d23197af3f2c9e39618545c4719366633a666725b83be2a3214a48a11df5
apfs_fuse_revision=66b86bd525e8cb90f9012543be89b1f092b75cf3
apfs_fuse_lzfse_revision=e634ca58b4821d9f3d560cdc6df5dec02ffc93fd

make=${MAKE:-make}
jobs=$(getconf _NPROCESSORS_ONLN)

fail() {
    echo "build-apfs-oracles: $*" >&2
    exit 1
}

need() {
    for need_tool in "$@"; do
        command -v "$need_tool" >/dev/null ||
            fail "required development tool is missing: $need_tool"
    done
}

# The pinned archive hash is compared with this one; with no hash tool
# nothing is printed and the comparison fails closed.
sha256_of() {
    if command -v sha256sum >/dev/null; then
        sha256sum "$1" | sed 's/ .*//'
    elif command -v sha256 >/dev/null; then
        sha256 -q "$1"
    else
        shasum -a 256 "$1" | sed 's/ .*//'
    fi
}

# fetch_revision DIRECTORY URL REVISION: fetch one commit at depth 1 into
# DIRECTORY/source and check it is the pinned one.
fetch_revision() {
    git init -q "$1/source"
    git -C "$1/source" fetch -q --depth 1 "$2" "$3"
    git -C "$1/source" checkout -q --detach FETCH_HEAD
    test "$(git -C "$1/source" rev-parse HEAD)" = "$3" ||
        fail "fetched revision is not $3"
}

# apfsck and mkapfs from linux-apfs/apfsprogs (GPL-2.0). The sources
# include <linux/types.h> and the other Linux uapi headers, so they build
# on Linux only; macOS and FreeBSD hosts use the build-aux/linux-check.sh
# environment.
build_apfsprogs() {
    test "$(uname -s)" = Linux ||
        fail 'apfsprogs includes <linux/types.h> and builds on Linux only'
    fetch_revision "$destination" \
        https://github.com/linux-apfs/apfsprogs.git "$apfsprogs_revision"
    "$make" -C "$destination/source/apfsck" -j"$jobs"
    "$make" -C "$destination/source/mkapfs" -j"$jobs"
    mkdir "$destination/bin"
    cp "$destination/source/apfsck/apfsck" "$destination/source/mkapfs/mkapfs" \
        "$destination/bin/"
    echo "apfsprogs oracle: $destination/bin; upstream revision: $apfsprogs_revision"
}

# fsapfsinfo from the libfsapfs experimental release (LGPL-3.0-or-later).
# The release archive carries the generated configure script and the libyal
# support libraries at the versions the release was tested with; a git
# checkout instead fetches those from their moving heads (synclibs.sh).
# The build needs no OpenSSL, zlib, FUSE or gettext. It uses the local copies
# of the libyal libraries unless configure finds installed ones.
build_libfsapfs() {
    archive=libfsapfs-experimental-$libfsapfs_release.tar.gz
    curl -fsSL --proto '=https' --proto-redir '=https' -o "$destination/$archive" \
        "https://github.com/libyal/libfsapfs/releases/download/$libfsapfs_release/$archive"
    test "$(sha256_of "$destination/$archive")" = "$libfsapfs_sha256" ||
        fail "$archive does not match its pinned SHA-256"
    mkdir "$destination/source"
    tar -xzf "$destination/$archive" -C "$destination/source" --strip-components 1
    cd "$destination/source"
    ./configure --prefix="$destination" --disable-shared \
        --disable-nls --without-libfuse --without-openssl --without-zlib \
        --disable-python \
        > "$destination/configure.log" 2>&1 || {
        tail -n 30 "$destination/configure.log" >&2
        exit 1
    }
    "$make" -j"$jobs"
    mkdir "$destination/bin"
    cp fsapfstools/fsapfsinfo "$destination/bin/"
    echo "libfsapfs oracle: $destination/bin; release: $libfsapfs_release sha256: $libfsapfs_sha256"
}

# apfs-dump, apfs-dump-quick and apfsutil from sgan81/apfs-fuse
# (GPL-2.0-or-later). They link the system zlib and bzip2.
# The FUSE mount tool is not built, so no FUSE library is needed. The
# upstream CMakeLists asks for CMake 3.0, which CMake 4 refuses, so the
# policy minimum is passed on the command line and no source is patched.
# The lzfse submodule is fetched at the revision the checkout records.
build_apfs_fuse() {
    fetch_revision "$destination" \
        https://github.com/sgan81/apfs-fuse.git "$apfs_fuse_revision"
    git -C "$destination/source" submodule update --init --depth 1
    test "$(git -C "$destination/source/3rdparty/lzfse" rev-parse HEAD)" = \
        "$apfs_fuse_lzfse_revision" || fail 'lzfse submodule is not pinned'
    build=$destination/build
    cmake -S "$destination/source" -B "$build" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > "$destination/configure.log" 2>&1 || {
        tail -n 30 "$destination/configure.log" >&2
        exit 1
    }
    cmake --build "$build" -j"$jobs" --target apfs-dump apfs-dump-quick apfsutil
    mkdir "$destination/bin"
    for built in apfs-dump apfs-dump-quick apfsutil; do
        cp "$build/$built" "$destination/bin/"
    done
    echo "apfs-fuse oracle: $destination/bin; upstream revision: $apfs_fuse_revision"
}

# Check the host before creating anything, so a refused run leaves no
# directory behind.
case $tool in
apfsprogs)
    # The sources include <linux/types.h> and the other Linux uapi headers.
    test "$(uname -s)" = Linux ||
        fail 'apfsprogs builds on Linux only; use the build-aux/linux-check.sh image'
    need git cc "$make"
    ;;
libfsapfs) need curl tar cc "$make" ;;
apfs-fuse) need git cc c++ cmake "$make" ;;
*)
    echo "$usage" >&2
    exit 2
    ;;
esac

umask 022
if test "$#" = 2; then
    target=$2
else
    mkdir -p "$HOME/.local/share/fsimg-oracles"
    target=$HOME/.local/share/fsimg-oracles/$tool
fi
case $target in
*'
'*) fail 'the directory name contains a newline' ;;
esac
mkdir -- "$target"
destination=$(CDPATH= cd -- "$target" && pwd)
unset MAKEFLAGS MFLAGS

# A failed build leaves its directory behind for inspection; remove it
# before running again.
case $tool in
apfsprogs) build_apfsprogs ;;
libfsapfs) build_libfsapfs ;;
apfs-fuse) build_apfs_fuse ;;
esac
