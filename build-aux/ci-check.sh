#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
#
# ci-check.sh
#
# Run the checks a native CI job runs, in a configured and built build
# directory. The macOS, Linux and FreeBSD CI jobs and verification before a
# change lands all run this one list, so what is checked locally cannot
# drift from what CI checks. MAKE names the make program (default make;
# MAKE=gmake on a FreeBSD host); pass -j through MAKEFLAGS. Targets a host
# cannot run, such as LeakSanitizer and MemorySanitizer outside Linux,
# report their own skip; on Linux, check-msan needs MSAN_ZLIB
# (docs/build.md). The long qualification runs (check-extended) stay out.

set -eu

make=${MAKE:-make}

if [ ! -f config.status ]; then
	echo "ci-check.sh: run in a configured build directory" >&2
	exit 2
fi

"$make" check
# The CLI and specification tests must run here, not skip.
grep -q '^:test-result: PASS' test/cli.sh.trs
grep -q '^:test-result: PASS' test/spec.sh.trs
"$make" check-install
# The suite skips the sandbox test where a profile is unsupported; on hosts
# with a sandbox backend it must run and pass.
case $(uname -s) in
Linux | FreeBSD) ./test-sandbox ;;
esac
"$make" check-asan
"$make" check-ubsan
"$make" check-lsan
"$make" check-msan
"$make" check-scale
"$make" check-cli-rss
"$make" distcheck
