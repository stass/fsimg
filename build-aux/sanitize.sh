#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
set -eu
mode=$1
source_dir=$2
case "$mode" in
asan) flags='-fsanitize=address,undefined -fno-omit-frame-pointer -fsanitize-address-use-after-scope'
    ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1
    if test "$(uname -s)" = Linux; then
        ASAN_OPTIONS=$ASAN_OPTIONS:detect_leaks=1:check_initialization_order=1
    fi
    export ASAN_OPTIONS ;;
ubsan) flags='-fsanitize=undefined -fno-sanitize-recover=all' ;;
lsan|msan)
    if test "$(uname -s)" != Linux; then
        echo "SKIP: $mode requires a supported Linux toolchain (host: $(uname -s))"
        exit 0
    fi
    case "$mode" in
    lsan) flags='-fsanitize=leak' ;;
msan) flags='-fsanitize=memory -fsanitize-memory-track-origins=2 -fPIE' ;;
    esac ;;
*) exit 1 ;;
esac
mkdir -p "sanitizer-$mode"
cd "sanitizer-$mode"
link_flags=$flags
if test "$mode" = msan; then link_flags="$link_flags -pie"; fi
CC="${CC:-clang}" CXX="${CXX:-clang++}" CFLAGS="-O1 -g $flags" \
    CXXFLAGS="-O1 -g $flags" LDFLAGS="$link_flags" \
    "$source_dir/configure" --disable-shared
make check
