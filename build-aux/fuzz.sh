#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
set -eu
source_dir=$1
fuzz_cc=$(command -v "${FUZZ_CC:-clang}")
fuzz_cxx=${FUZZ_CXX:-"$(dirname "$fuzz_cc")/clang++"}
# Instrument every library object as well as the harness. A separate build
# prevents an ordinary ASan archive from silently providing no parser feedback.
CC="$fuzz_cc" CXX="$fuzz_cxx" sh "$source_dir/build-aux/sanitize.sh" fuzz "$source_dir"
if ! nm sanitizer-fuzz/.libs/libfsimg.a | grep -q __sanitizer_cov_; then
    echo 'FAIL: fuzz library lacks sanitizer coverage instrumentation' >&2
    exit 1
fi
"$fuzz_cc" -std=c11 -O1 -g -fno-omit-frame-pointer \
    -fsanitize=fuzzer,address,undefined -fsanitize-address-use-after-scope \
    -I"$source_dir/include" -I"$source_dir/compat" \
    "$source_dir/fuzz/fat.c" sanitizer-fuzz/.libs/libfsimg.a -o fuzz-fat
mkdir -p fuzz-corpus
for variant in 12 16 32; do
    if test ! -f "fuzz-corpus/fat$variant.seed"; then
        ./test-fat --seed-variant "fuzz-corpus/fat$variant.seed" "$variant"
    fi
done
ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
if test "$(uname -s)" = Linux; then
    ASAN_OPTIONS=$ASAN_OPTIONS:detect_leaks=1:check_initialization_order=1
fi
export ASAN_OPTIONS UBSAN_OPTIONS
./fuzz-fat -runs=1000 -max_len=3145728 -timeout=10 fuzz-corpus
