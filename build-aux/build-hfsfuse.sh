#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
set -eu

if test "$#" != 1; then
    echo 'usage: build-hfsfuse.sh new-directory' >&2
    exit 2
fi

hfsfuse_make=${MAKE:-make}
revision=eed780cbe0445893de6f4aa8d74461da00756984
for tool in git cc "$hfsfuse_make"; do
    if ! command -v "$tool" >/dev/null; then
        echo "build-hfsfuse: required development tool is missing: $tool" >&2
        exit 1
    fi
done

# Keep the oracle outside the project and retain its upstream license/source.
# Creating the directory exclusively prevents an existing checkout being
# overwritten; it stays readable so a container's unprivileged user can run it.
# Only the hfsdump reader is built: the upstream tree carries its libraries
# (libhfs, utf8proc, LZVN, LZFSE), so the oracle needs no FUSE library. The
# upstream makefile needs GNU make, which is MAKE=gmake on FreeBSD.
umask 022
mkdir "$1"
destination=$(CDPATH= cd -- "$1" && pwd)
git init -q "$destination/source"
git -C "$destination/source" fetch -q --depth 1 \
    https://github.com/0x09/hfsfuse.git "$revision"
git -C "$destination/source" checkout -q --detach FETCH_HEAD
test "$(git -C "$destination/source" rev-parse HEAD)" = "$revision"

unset MAKEFLAGS MFLAGS
"$hfsfuse_make" -C "$destination/source" hfsdump
mkdir "$destination/bin"
cp "$destination/source/hfsdump" "$destination/bin/hfsdump"
echo "hfsdump oracle: $destination/bin/hfsdump; upstream revision: $revision"
