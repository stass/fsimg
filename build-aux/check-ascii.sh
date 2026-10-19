#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
set -eu
LC_ALL=C
export LC_ALL
# Inspect maintained source/build files, also in unpacked release archives.
find "$1/include" "$1/lib" "$1/compat" "$1/cmd" "$1/test" "$1/.github" \
    "$1/fuzz" "$1/tools" "$1/examples" "$1/docs" \
    -type f \( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.sh' \
    -o -name '*.yml' -o -name '*.am' -o -name '*.ac' -o -name '*.py' \
    -o -name '*.sh.in' -o -name '*.1' -o -name '*.3' \) \
    -exec awk '/[^\001-\177]/ { print FILENAME ":" FNR ": non-ASCII byte"; bad=1 } END { if (bad) exit 1 }' {} +
awk '/[^\001-\177]/ { print FILENAME ":" FNR ": non-ASCII byte"; bad=1 } END { if (bad) exit 1 }' \
    "$1/configure.ac" "$1/Makefile.am" "$1/libfsimg.pc.in" "$1/build-aux/check-ascii.sh" \
    "$1/build-aux/sanitize.sh" "$1/build-aux/fuzz.sh" \
    "$1/build-aux/build-fatcat.sh" "$1/build-aux/fatcat-freebsd.patch" \
    "$1/build-aux/check-install.sh" "$1/build-aux/check-installed.py" \
    "$1/build-aux/opencode-review.sh" "$1/test/baseline.txt"
