#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
#
# Runs a command over a committed revision in an Ubuntu 24.04 container that
# mirrors the CI Linux job, from a macOS (or any Docker) host:
#
#   build-aux/linux-check.sh [-r REV] [COMMAND]
#
# REV defaults to HEAD. COMMAND is one shell string, run in the configured
# out-of-tree build directory; it defaults to "make -j8 check" plus CI's check
# that the specification suite passed rather than skipped. When the build or
# COMMAND fails, the relevant logs' tails are printed, since the container and
# its logs are discarded on exit.
#
# The revision supplies the source and the spec gem lockfile; this checkout's
# Dockerfile supplies the toolchain, so any revision that has
# test/spec/Gemfile is checked with the same toolchain. The source arrives
# with git archive, so nothing built on Linux lands in the working tree and
# uncommitted changes are not checked. Building the image fetches packages
# and the revision's gems over the network; the run itself has no network
# and runs as an unprivileged user. The container's kernel must provide
# Landlock ABI 3 for the sandbox profiles to run (docs/build.md).
set -eu

usage() {
	echo "usage: $0 [-r rev] [command]" >&2
	exit 2
}

default_command="make -j8 check && grep -q '^:test-result: PASS' test/spec.sh.trs"
rev=HEAD
while getopts r: opt; do
	case $opt in
	r) rev=$OPTARG ;;
	*) usage ;;
	esac
done
shift $((OPTIND - 1))
test $# -le 1 || usage
check_command=${1:-$default_command}
top=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
if ! commit=$(git -C "$top" rev-parse -q --verify --end-of-options \
    "$rev^{commit}"); then
	echo "$0: $rev does not name a commit" >&2
	exit 2
fi

# The program run inside the container: $1 is COMMAND, and "sh" fills $0.
# stdin carries the revision's archive.
container_program='
set -eu
mkdir src
cd src
tar -xf -
autoreconf -fi >/dev/null
mkdir build
cd build
if ! ../configure CC=clang CXX=clang++ >/dev/null; then
	tail -40 config.log || :
	exit 1
fi
make -j8 >/dev/null
status=0
sh -c "$1" || status=$?
if test "$status" -ne 0; then
	for trs in $(grep -rlE "^:test-result: (FAIL|ERROR|XPASS)" \
	    --include="*.trs" .); do
		echo "######## ${trs%.trs}.log"
		tail -60 "${trs%.trs}.log" || :
	done
	if test -f test/spec.sh.trs &&
	    ! grep -q "^:test-result: PASS" test/spec.sh.trs; then
		echo "######## test/spec.sh did not pass:"
		tail -20 test/spec.sh.log || :
	fi
fi
exit "$status"'

# Everything this run creates lives in a private directory: the image's build
# context (image/, which nothing else enters), the image and container IDs
# docker writes, and the source archive. Cleanup removes the container by the
# ID docker wrote, so it can never touch a container another run owns.
rundir=$(mktemp -d)
cleanup() {
	if test -s "$rundir/cid"; then
		docker rm -f "$(cat "$rundir/cid")" >/dev/null 2>&1 || :
	fi
	rm -rf "$rundir"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 131' QUIT
trap 'exit 143' TERM

# The image installs the revision's gems, so the run needs no network. Its
# ID, not a shared tag, names it: another checkout may rebuild meanwhile.
mkdir "$rundir/image"
cp "$top/build-aux/linux-check/Dockerfile" "$rundir/image/"
cp "$top/build-aux/build-msan-zlib.sh" "$rundir/image/"
git -C "$top" show "$commit:test/spec/Gemfile" >"$rundir/image/Gemfile"
git -C "$top" show "$commit:test/spec/Gemfile.lock" \
    >"$rundir/image/Gemfile.lock"
docker build --iidfile "$rundir/iid" "$rundir/image" >&2

# The archive goes to a file first so that a failure to produce it stops here
# rather than surfacing as a tar error inside the container.
git -C "$top" archive --format=tar -o "$rundir/source.tar" "$commit"

# Creating the container in the foreground records its ID before anything
# runs: a shell defers a trapped signal until its foreground command returns,
# so cleanup always knows what to remove. The started container is waited on
# in the background instead, so an interrupt runs cleanup at once, and
# removing the container stops the check. The label finds a container left
# behind if this script is killed outright:
#   docker ps -a --filter label=fsimg-linux-check
docker create -i --rm --network none --cidfile "$rundir/cid" \
    --label fsimg-linux-check "$(cat "$rundir/iid")" \
    sh -c "$container_program" sh "$check_command" >/dev/null
docker start -ai "$(cat "$rundir/cid")" <"$rundir/source.tar" &
wait $!
