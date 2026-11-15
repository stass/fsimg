#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
set -eu

# Run apfsck, the Linux APFS checker, on each IMAGE that is a bare APFS
# container, from macOS or Linux, through Docker. The image is the one
# build-aux/linux-check.sh builds for this checkout, and the checker is built
# inside it by build-aux/build-apfs-oracles.sh at its pinned revision, which
# needs the network. The images reach the container as one sparse tar on
# standard input, so no copy of them is made on the host: a byte copy of a
# sparse image would write its holes. A GPT disk or UDIF file is reported as
# skipped, since apfsck reads only a bare container.
#
#   tools/apfs/apfsck-linux.sh DIR/outputs/*/image.*
#
# Exit status 0 when every bare container passes with no output.
usage='usage: apfsck-linux.sh IMAGE...'
test "$#" -ge 1 || { echo "$usage" >&2; exit 2; }
command -v docker >/dev/null || { echo "apfsck-linux: docker is required" >&2; exit 1; }

top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/apfsck-linux.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir "$work/image" "$work/list"
cp "$top/build-aux/linux-check/Dockerfile" "$top/build-aux/build-cdrtools.sh" \
    "$top/build-aux/build-hfsfuse.sh" "$top/build-aux/build-msan-zlib.sh" \
    "$work/image/"
cp "$top/test/spec/Gemfile" "$top/test/spec/Gemfile.lock" "$work/image/"
docker build -q --iidfile "$work/iid" "$work/image" >/dev/null

# Each image enters the tar under its index, beside a file naming it, so
# that any path, however spelled, arrives intact.
index=0
for image in "$@"; do
    test -f "$image" || { echo "apfsck-linux: $image is not a file" >&2; exit 2; }
    index=$((index + 1))
    ln -s "$(cd "$(dirname "$image")" && pwd)/$(basename "$image")" "$work/list/$index.img"
    printf '%s\n' "$image" >"$work/list/$index.name"
done

# The archive's producer is outside the container, and a POSIX pipeline
# reports only its last command, so the container checks that every image
# it was promised arrived whole: a failed or short transfer leaves an image
# or its name missing, or fails the extraction, and the run fails.
(cd "$work/list" && tar -chSf - .) | docker run -i --rm --user 1000:1000 \
    -e "EXPECTED=$index" \
    -v "$top/build-aux/build-apfs-oracles.sh:/s.sh:ro" "$(cat "$work/iid")" sh -c '
        set -eu
        sh /s.sh apfsprogs /tmp/o >/tmp/build.log 2>&1 ||
            { tail -20 /tmp/build.log; exit 1; }
        /tmp/o/bin/apfsck -v
        mkdir /tmp/i && cd /tmp/i && tar -xSf -
        for n in $(seq 1 "$EXPECTED"); do
            test -f "$n.img" && test -f "$n.name" ||
                { echo "apfsck-linux: image $n did not arrive"; exit 1; }
        done
        status=0
        for n in $(seq 1 "$EXPECTED"); do
            image=$n.img
            name=$(cat "$n.name")
            if test "$(dd if="$image" bs=1 skip=32 count=4 2>/dev/null)" != NXSB; then
                echo "$name: skipped: not a bare container"
            elif /tmp/o/bin/apfsck "$image" >/tmp/out 2>&1 && ! test -s /tmp/out; then
                echo "$name: pass"
            else
                status=1
                echo "$name: FAIL"
                sed -n 1,20p /tmp/out
            fi
        done
        exit $status'
