#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
set -eu

if test "$#" != 1; then
    echo 'usage: build-erofs-utils.sh new-directory' >&2
    exit 2
fi

erofs_make=${MAKE:-make}
tag=v1.9.4
revision=f36cadb5c563995ab3aa8572a60ed6b721b9557d
for tool in git cc autoconf automake libtoolize pkg-config "$erofs_make"; do
    if [ "$tool" = libtoolize ] && command -v glibtoolize >/dev/null; then
        continue
    fi
    if ! command -v "$tool" >/dev/null; then
        echo "build-erofs-utils: required development tool is missing: $tool" >&2
        exit 1
    fi
done

# Keep the oracle outside the project; it is a development reader and
# writer, never linked into or copied into fsimg. The directory is created
# exclusively so an existing checkout is never overwritten. The build has
# LZ4, liblzma, zlib and libzstd and no FUSE; the S3, OCI and
# netlink options that configure would otherwise probe for are disabled.
# Homebrew libraries are found through PKG_CONFIG_PATH when brew is present.
umask 022
mkdir "$1"
destination=$(CDPATH= cd -- "$1" && pwd)
if command -v brew >/dev/null; then
    brew_prefix=$(brew --prefix)
    PKG_CONFIG_PATH=$brew_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
    export PKG_CONFIG_PATH
fi

git init -q "$destination/source"
git -C "$destination/source" fetch -q --depth 1 \
    https://github.com/erofs/erofs-utils.git "refs/tags/$tag"
git -C "$destination/source" checkout -q --detach FETCH_HEAD
test "$(git -C "$destination/source" rev-parse 'HEAD^{commit}')" = "$revision"

unset MAKEFLAGS MFLAGS
cd "$destination/source"
./autogen.sh
./configure --prefix="$destination" --with-libzstd \
    --without-libcurl --without-openssl --without-libxml2 --without-json-c \
    --without-libnl3 --disable-fuse > "$destination/configure.log" 2>&1 || {
    tail -n 30 "$destination/configure.log" >&2
    exit 1
}
"$erofs_make" -j"$(getconf _NPROCESSORS_ONLN)"
# Configure silently drops a codec whose development package is missing, so
# the build must report every codec the oracle is meant to have.
codecs=$(mkfs/mkfs.erofs -V | sed -n 's/^available compressors: //p')
test "$codecs" = 'lz4, lz4hc, lzma, deflate, zstd' || {
    echo "build-erofs-utils: unexpected codec set: $codecs" >&2
    exit 1
}
mkdir "$destination/bin"
for tool in mkfs/mkfs.erofs fsck/fsck.erofs dump/dump.erofs; do
    cp "$tool" "$destination/bin/"
done
echo "erofs-utils oracle: $destination/bin; upstream $tag revision: $revision"
