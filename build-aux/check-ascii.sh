#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
set -eu
LC_ALL=C
export LC_ALL
# Inspect maintained source/build files, also in unpacked release archives.
# An in-tree build writes specification test results and caches under test/.
find "$1/include" "$1/lib" "$1/compat" "$1/cmd" "$1/test" "$1/.github" \
    "$1/fuzz" "$1/tools" "$1/examples" "$1/docs" "$1/build-aux" \
    \( -path "$1/test/spec-results" -o -path "$1/test/spec-cache" \) -prune -o \
    -type f \( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.sh' \
    -o -name '*.yml' -o -name '*.am' -o -name '*.ac' -o -name '*.py' \
    -o -name '*.sh.in' -o -name '*.1' -o -name '*.3' -o -name '*.rb' \
    -o -name '*.json' -o -name '*.patch' -o -name Dockerfile \
    -o -name Gemfile -o -name Gemfile.lock \
    -o -path "$1/test/spec/bin/*" \) \
    -exec awk '/[^\001-\177]/ { print FILENAME ":" FNR ": non-ASCII byte"; bad=1 } END { if (bad) exit 1 }' {} +
awk '/[^\001-\177]/ { print FILENAME ":" FNR ": non-ASCII byte"; bad=1 } END { if (bad) exit 1 }' \
    "$1/configure.ac" "$1/Makefile.am" "$1/libfsimg.pc.in" \
    "$1/test/baseline.txt"
