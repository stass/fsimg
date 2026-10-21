#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
#
# usage: MAKE=make sh fuzz.sh
#
# Run from the top of a configured build directory holding test-fat,
# test-exfat, test-qnx6, test-qnx4, test-protocol, test-layers and
# test/spec.sh, which write the seeds, with MAKE naming the make program
# that drives that build (sanitize.sh inherits it). FUZZ_CC and FUZZ_CXX
# select the compiler. The harnesses build against the instrumented
# sanitizer-fuzz/ tree (see sanitize.sh) and keep their corpora here
# between runs.
set -eu
fuzz_seconds=${FUZZ_SECONDS:-0}
case "$fuzz_seconds" in
''|*[!0-9]*) echo 'FUZZ_SECONDS must be decimal seconds (0..999999)' >&2; exit 2 ;;
esac
if test "${#fuzz_seconds}" -gt 6; then
    echo 'FUZZ_SECONDS exceeds 999999 seconds' >&2
    exit 2
fi
fuzz_runs=1000
if test "$fuzz_seconds" -gt 0; then
    fuzz_runs=-1
fi

# A positive wall-clock allowance selects a sustained campaign. Otherwise the
# finite mutation count provides the smoke budget; neither mode is unlimited.
run_fuzzer() {
    fuzz_program=$1
    fuzz_corpus=$2
    fuzz_max_length=$3
    fuzz_timeout=$4
    "$fuzz_program" -runs="$fuzz_runs" -max_total_time="$fuzz_seconds" \
        -max_len="$fuzz_max_length" -timeout="$fuzz_timeout" "$fuzz_corpus"
}

fuzz_cc=$(command -v "${FUZZ_CC:-clang}")
fuzz_cxx=${FUZZ_CXX:-"$(dirname "$fuzz_cc")/clang++"}
# Instrument every library object as well as the harness. A separate build
# prevents an ordinary ASan archive from silently providing no parser feedback.
CC="$fuzz_cc" CXX="$fuzz_cxx" sh "$(dirname "$0")/sanitize.sh" fuzz
# The harnesses compile from the same source copy as the library and live
# beside it, so make clean removes them with the sanitizer build.
work_dir=sanitizer-fuzz
source_dir=$work_dir/src
build_dir=$work_dir/build
fuzz_library=$build_dir/.libs/libfsimg.a
if ! nm "$fuzz_library" | grep -q __sanitizer_cov_; then
    echo 'FAIL: fuzz library lacks sanitizer coverage instrumentation' >&2
    exit 1
fi
"$fuzz_cc" -std=c11 -O1 -g -fno-omit-frame-pointer \
    -fsanitize=fuzzer,address,undefined -fsanitize-address-use-after-scope \
    -I"$source_dir/include" -I"$source_dir/compat" \
    "$source_dir/fuzz/fat.c" "$fuzz_library" -o "$work_dir/fuzz-fat"
mkdir -p fuzz-corpus
for variant in 12 16 32; do
    if test ! -f "fuzz-corpus/fat$variant.seed"; then
        ./test-fat --seed-variant "fuzz-corpus/fat$variant.seed" "$variant"
    fi
done
# The specification harness adds a seed per targeted or promoted FAT case
# when Ruby 3.2 and its locked gems are available; test/spec.sh exits 77
# otherwise.
spec_status=0
sh test/spec.sh --export-seeds="$PWD/fuzz-corpus" || spec_status=$?
if test "$spec_status" -ne 0 && test "$spec_status" -ne 77; then
    echo "FAIL: test/spec.sh --export-seeds exited $spec_status" >&2
    exit 1
fi
ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
if test "$(uname -s)" = Linux; then
    ASAN_OPTIONS=$ASAN_OPTIONS:detect_leaks=1:check_initialization_order=1
fi
export ASAN_OPTIONS UBSAN_OPTIONS
run_fuzzer "$work_dir/fuzz-fat" fuzz-corpus 3145728 10

# The same bounded dispatch/content harness reaches exFAT from both an
# independently encoded fragmented fixture and a compact writer fixture.
exfat_corpus=exfat-fuzz-corpus
mkdir -p "$exfat_corpus"
if test ! -f "$exfat_corpus/fragmented.seed"; then
    ./test-exfat --seed "$exfat_corpus/fragmented.seed"
fi
if test ! -f "$exfat_corpus/compact.seed"; then
    ./test-exfat --emit "$exfat_corpus/compact.seed"
fi
run_fuzzer "$work_dir/fuzz-fat" "$exfat_corpus" 3145728 10

# QNX6 locates its second header from the slice length, so its harness
# reads exactly the input and forces the decoder. It reseals header CRCs
# with the driver's own codec, so it sees the driver's private header.
"$fuzz_cc" -std=c11 -O1 -g -fno-omit-frame-pointer \
    -fsanitize=fuzzer,address,undefined -fsanitize-address-use-after-scope \
    -I"$source_dir/include" -I"$source_dir/compat" \
    -I"$source_dir/lib/core" -I"$source_dir/lib/meta" -I"$source_dir/lib/io" \
    -I"$source_dir/lib/read" -I"$source_dir/lib/formats" \
    -I"$source_dir/lib/formats/qnx6" \
    "$source_dir/fuzz/qnx6.c" "$fuzz_library" -o "$work_dir/fuzz-qnx6"
qnx6_corpus=qnx6-fuzz-corpus
mkdir -p "$qnx6_corpus"
if test ! -f "$qnx6_corpus/sample.seed"; then
    ./test-qnx6 --seed "$qnx6_corpus/sample.seed"
fi
run_fuzzer "$work_dir/fuzz-qnx6" "$qnx6_corpus" 1048576 10

# QNX4 derives its block count from the slice length, so its harness also
# reads exactly the input and forces the decoder. The seeds are generated
# volumes of the reader's acceptance fixtures.
"$fuzz_cc" -std=c11 -O1 -g -fno-omit-frame-pointer \
    -fsanitize=fuzzer,address,undefined -fsanitize-address-use-after-scope \
    -I"$source_dir/include" -I"$source_dir/compat" \
    "$source_dir/fuzz/qnx4.c" "$fuzz_library" -o "$work_dir/fuzz-qnx4"
qnx4_corpus=qnx4-fuzz-corpus
mkdir -p "$qnx4_corpus"
if test ! -f "$qnx4_corpus/sample.seed"; then
    ./test-qnx4 --seed "$qnx4_corpus/sample.seed"
fi
if test ! -f "$qnx4_corpus/chain.seed"; then
    ./test-qnx4 --seed-chain "$qnx4_corpus/chain.seed"
fi
run_fuzzer "$work_dir/fuzz-qnx4" "$qnx4_corpus" 1048576 10

"$fuzz_cc" -std=c11 -O1 -g -fno-omit-frame-pointer \
    -fsanitize=fuzzer,address,undefined -fsanitize-address-use-after-scope \
    -I"$build_dir" -I"$source_dir/include" -I"$source_dir/compat" \
    -I"$source_dir/lib/core" -I"$source_dir/lib/layers" -I"$source_dir/cmd/fsimg" \
    "$source_dir/fuzz/protocol.c" "$source_dir/cmd/fsimg/protocol.c" \
    "$source_dir/cmd/fsimg/job.c" "$fuzz_library" -o "$work_dir/fuzz-protocol"
protocol_corpus=protocol-fuzz-corpus
mkdir -p "$protocol_corpus"
if test ! -f "$protocol_corpus/frames.seed"; then
    ./test-protocol --seed "$protocol_corpus/frames.seed"
fi
run_fuzzer "$work_dir/fuzz-protocol" "$protocol_corpus" 65536 5

# The image harness reads each input as a whole disk and drives the scheme
# step, partition enumeration, selection and layer verification under
# several forced stacks. The seeds are the GPT and MBR disks of the layer
# tests.
"$fuzz_cc" -std=c11 -O1 -g -fno-omit-frame-pointer \
    -fsanitize=fuzzer,address,undefined -fsanitize-address-use-after-scope \
    -I"$source_dir/include" -I"$source_dir/compat" \
    "$source_dir/fuzz/image.c" "$fuzz_library" -o "$work_dir/fuzz-image"
image_corpus=image-fuzz-corpus
mkdir -p "$image_corpus"
if test ! -f "$image_corpus/gpt.seed"; then
    ./test-layers --seed-gpt "$image_corpus/gpt.seed"
fi
if test ! -f "$image_corpus/mbr.seed"; then
    ./test-layers --seed-mbr "$image_corpus/mbr.seed"
fi
run_fuzzer "$work_dir/fuzz-image" "$image_corpus" 1048576 10
