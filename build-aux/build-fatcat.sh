#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
set -eu

if test "$#" != 1; then
    echo 'usage: build-fatcat.sh new-directory' >&2
    exit 2
fi

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
destination=$1
revision=76000303082e9e86f1aca8edb030c5fb8a63ae55
for tool in git cmake patch; do
    if ! command -v "$tool" >/dev/null; then
        echo "build-fatcat: required development tool is missing: $tool" >&2
        exit 1
    fi
done

# Keep the oracle outside the project and retain its upstream license/source.
# An exclusive private directory prevents an existing checkout being overwritten.
umask 077
mkdir "$destination"
destination=$(CDPATH= cd -- "$destination" && pwd)
git init -q "$destination/source"
git -C "$destination/source" fetch --depth 1 \
    https://github.com/gregwar/fatcat.git "$revision"
git -C "$destination/source" checkout -q --detach FETCH_HEAD
test "$(git -C "$destination/source" rev-parse HEAD)" = "$revision"

if test "$(uname -s)" = FreeBSD; then
    # Select upstream's existing native getopt and 64-bit offset interfaces.
    # No filesystem parsing or content-decoding code changes in this patch.
    patch -d "$destination/source" -p1 < "$source_dir/build-aux/fatcat-freebsd.patch"
fi

cmake -S "$destination/source" -B "$destination/build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=14 \
    -DCMAKE_CXX_STANDARD_REQUIRED=ON
cmake --build "$destination/build" --parallel "${JOBS:-2}"
echo "fatcat oracle: $destination/build/fatcat; upstream revision: $revision"
