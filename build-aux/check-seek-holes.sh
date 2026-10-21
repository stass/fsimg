#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
set -eu
LC_ALL=C
export LC_ALL
# The host's hole report is asked in one place, the compatibility layer:
# SEEK_HOLE and SEEK_DATA may appear in no C source or header but
# compat/host.c, so no other path can grow a host hole query.
status=0
for directory in include lib compat cmd fuzz examples; do
	[ -d "$1/$directory" ] || continue
	for file in $(find "$1/$directory" -type f \( -name '*.c' -o \
	    -name '*.h' -o -name '*.cc' \) ! -path "$1/compat/host.c" \
	    -exec grep -l -E 'SEEK_(HOLE|DATA)' {} +); do
		printf '%s: SEEK_HOLE or SEEK_DATA outside compat/host.c\n' \
		    "$file" >&2
		status=1
	done
done
exit $status
