#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
#
# Build an external zlib source tree for MemorySanitizer checks only.
# Usage: build-msan-zlib.sh SOURCE PREFIX
# SOURCE is unpacked by the development host's package manager. Nothing
# from it enters fsimg's source tree or ordinary library build.
set -eu
unset CDPATH

if test "$#" -ne 2 || test ! -f "$1/configure"; then
	echo "usage: $0 zlib-source prefix" >&2
	exit 2
fi
source=$(cd "$1" && pwd)
mkdir -p "$2"
prefix=$(cd "$2" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM
cp -R "$source/." "$work/"
cd "$work"
compiler=${CC:-clang}
flags='-O1 -g -fsanitize=memory -fsanitize-memory-track-origins=2 -fPIC'
CC="$compiler" CFLAGS="$flags" ./configure --static --prefix="$prefix"
# Pin these at make too: inherited command-line variables cannot remove
# the instrumentation selected by configure.
"${MAKE:-make}" CC="$compiler" CFLAGS="$flags"
"${MAKE:-make}" install CC="$compiler" CFLAGS="$flags"
