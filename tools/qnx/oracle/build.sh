#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# build.sh [OUTDIR]: build the codec oracle against Homebrew's liblzo2 and
# libucl (brew install lzo ucl) into OUTDIR, by default
# ~/.local/share/fsimg-oracles/ifs-codecs. The binary combines GPL
# libraries, so it stays outside the source tree.
set -eu
src=$(CDPATH= cd -P -- "$(dirname -- "$0")" && pwd)
out=${1:-$HOME/.local/share/fsimg-oracles/ifs-codecs}
lzo=$(brew --prefix lzo)
ucl=$(brew --prefix ucl)
mkdir -p "$out"
cc -O2 -Wall -I"$lzo/include" -I"$ucl/include" "$src/oracle.c" \
    "$lzo/lib/liblzo2.a" "$ucl/lib/libucl.a" -o "$out/oracle"
echo "$out/oracle"
