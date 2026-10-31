#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
#
# usage: MAKE=make sh sanitize.sh asan|ubsan|lsan|msan|fuzz
#
# Run the complete test suite under one sanitizer. Run it from the top of a
# configured build directory, with MAKE naming the make program that drives
# that build; CC and CXX select the sanitizer compiler (default clang). The
# result lives in sanitizer-MODE/ there:
#
#   src/    a pristine, read-only source copy made by this build's
#           `make distdir`, so it works whether the source tree is configured
#           in place or not, and a file missing from the distribution fails
#           the suite. Diagnostics name files here; edit the originals.
#   build/  the instrumented build, configured with this build's own
#           configure arguments so it finds the same development oracles
#
# Each run starts from scratch and writes nothing outside sanitizer-MODE/.
set -eu
unset CDPATH
mode=${1:-}
make=${MAKE:-make}
if test ! -f config.status; then
    echo 'sanitize.sh: run from the top of a configured build directory' >&2
    exit 2
fi

# Select the instrumentation and the runtime options for the mode.
case "$mode" in
asan|fuzz)
    sanitizers=address,undefined
    if test "$mode" = fuzz; then sanitizers=fuzzer-no-link,address,undefined; fi
    flags="-fsanitize=$sanitizers -fno-omit-frame-pointer -fsanitize-address-use-after-scope"
    ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
    if test "$(uname -s)" = Linux; then
        ASAN_OPTIONS=$ASAN_OPTIONS:detect_leaks=1:check_initialization_order=1
    fi
    export ASAN_OPTIONS UBSAN_OPTIONS ;;
ubsan) flags='-fsanitize=undefined -fno-sanitize-recover=all' ;;
lsan|msan)
    if test "$(uname -s)" != Linux; then
        echo "SKIP: $mode requires a supported Linux toolchain (host: $(uname -s))"
        exit 0
    fi
    case "$mode" in
    lsan) flags='-fsanitize=leak' ;;
    msan)
        flags='-fsanitize=memory -fsanitize-memory-track-origins=2 -fPIE'
        if test -z "${MSAN_ZLIB:-}" ||
            test ! -f "$MSAN_ZLIB/include/zlib.h" ||
            test ! -f "$MSAN_ZLIB/lib/libz.a"; then
            echo 'sanitize.sh: MSan requires MSAN_ZLIB pointing to an instrumented zlib prefix (docs/build.md)' >&2
            exit 2
        fi
        msan_zlib=$(cd "$MSAN_ZLIB" && pwd)
        zlib_cflags="-I$msan_zlib/include"
        # The named archive stays a dependency of libtool's convenience
        # library; an absolute archive would become a nested member.
        zlib_libs="-L$msan_zlib/lib -l:libz.a" ;;
    esac ;;
*)
    echo 'usage: MAKE=make sh sanitize.sh asan|ubsan|lsan|msan|fuzz' >&2
    exit 2 ;;
esac
link_flags=$flags
if test "$mode" = msan; then link_flags="$link_flags -pie"; fi
sanitizer_cc=${CC:-clang}
sanitizer_cxx=${CXX:-clang++}
sanitizer_cflags="-O1 -g $flags"
work_dir="$(pwd)/sanitizer-$mode"

# Copy the distribution afresh, so no stale source or object survives.
rm -rf "$work_dir"
mkdir "$work_dir" "$work_dir/build"
"$make" distdir distdir="$work_dir/src"
find "$work_dir/src" -type f -exec chmod a-w {} +

# Replay this build's configure arguments (oracle paths such as FATCAT and
# feature options); config.status --config prints them quoted for the shell.
# Arguments given after them take precedence, so the copy is the source and
# no cache file is shared whatever the outer build was configured with.
eval "set -- $(./config.status --config)"
if test "$mode" = msan; then
    set -- "$@" "ZLIB_CFLAGS=$zlib_cflags" "ZLIB_LIBS=$zlib_libs"
fi
cd "$work_dir/build"
../src/configure "$@" --srcdir=../src --cache-file=/dev/null \
    CC="$sanitizer_cc" CXX="$sanitizer_cxx" CFLAGS="$sanitizer_cflags" \
    CXXFLAGS="$sanitizer_cflags" LDFLAGS="$link_flags" --disable-shared

# Repeat the compiler and flags: assignments on the outer make command line
# (make check-asan CFLAGS=-O0) reach this make through MAKEFLAGS and would
# override the configured ones, silently building without instrumentation.
set --
if test "$mode" = msan; then
    set -- "ZLIB_CFLAGS=$zlib_cflags" "ZLIB_LIBS=$zlib_libs"
fi
"$make" check "$@" CC="$sanitizer_cc" CXX="$sanitizer_cxx" \
    CFLAGS="$sanitizer_cflags" CXXFLAGS="$sanitizer_cflags" LDFLAGS="$link_flags"
