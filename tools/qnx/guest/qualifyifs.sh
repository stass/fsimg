#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# qualifyifs.sh INDEX-BYTES: the native qualification cycle for ImageFS
# images fsimg wrote, run in the QNX 6.5.0 guest on the disk that
# tools/qnx/decode/ifsqual.py pack built and capture.sh -i loaded onto
# /dev/hd0. The disk starts with an index of INDEX-BYTES bytes: "case NAME"
# lines, and "member NAME SECTOR LENGTH CKSUM" lines for each member stored
# at that 512-byte sector. For each case the cycle is:
#   1. dumpifs -v and dumpifs -m of the fsimg image;
#   2. dumpifs -x of it into an empty directory on a QNX4 scratch volume,
#      after creating the image directories there (dumpifs -x creates none),
#      then ls -ldn and cksum of every path;
#   3. mkifs -nn of the same tree from the case buildfile, whose members are
#      the file contents, then steps 1 and 2 on that native image;
#   4. no mount: 6.5.0 has no mount_ifs, and procnto mounts only the image
#      it boots (E11).
# Comparison happens on the host with ifsqual.py compare. Upload and run:
#   put.sh tools/qnx/guest/qualifyifs.sh /tmp/qualifyifs.sh
#   capture.sh -i DISK NAME "sh /tmp/qualifyifs.sh INDEX-BYTES"
# The scratch volume is a 2 MiB QNX4 file on the RAM /tmp (whose names stop
# at 48 bytes; the scratch volume takes 505); everything is removed at the
# end, as are the images in /dev/shmem.
set -u
if test $# -ne 1; then
    echo "usage: qualifyifs.sh INDEX-BYTES" >&2
    exit 2
fi
W=/tmp/s10q
V=/tmp/s10vol.img
S=/s10scratch
I=/dev/shmem/s10q-fsimg.ifs
N=/dev/shmem/s10q-native.ifs
cleanup() {
    cd /
    umount $S 2>/dev/null
    rmdir $S 2>/dev/null
    rm -rf $W $V
    rm -f $I $N
}
cleanup
trap cleanup 0
mkdir -p $W $S || exit 1
dd if=/dev/hd0 bs=512 count=$((($1 + 511) / 512)) 2>/dev/null | head -c $1 > $W/index
echo "== index"
cksum $W/index
dd if=/dev/zero of=$V bs=512 count=4096 2>/dev/null
dinit -hq -S 2097152 $V > /dev/null || { echo "dinit failed"; exit 1; }
mount -t qnx4 $V $S || { echo "scratch mount failed"; exit 1; }
echo "== date"
date -u

# get MEMBER DEST: copy one member off the disk.
get() {
    set -- "$1" "$2" $(awk -v m="$1" "\$1 == \"member\" && \$2 == m { print \$3, \$4; exit }" $W/index)
    if test $# -ne 4; then
        echo "no member $1"
        return 1
    fi
    dd if=/dev/hd0 bs=512 skip=$3 count=$((($4 + 511) / 512)) 2>/dev/null | head -c $4 > "$2"
}

# extract LABEL IMAGE CASE: step 2 on IMAGE.
extract() {
    rm -rf -l 1000 $S/x
    mkdir $S/x || { echo "scratch directory left behind"; exit 1; }
    get "$3.dirs" $W/dirs
    cd $S/x
    while IFS= read -r d; do
        mkdir -p "./$d" 2>/dev/null
    done < $W/dirs
    echo "--- extract $1"
    dumpifs -x "$2" 2>&1
    echo "status $?"
    echo "--- tree $1"
    find . -print | while IFS= read -r f; do
        ls -ldn "$f" 2>&1
        if test -f "$f" && ! test -L "$f"; then cksum "$f" 2>&1; fi
    done
    cd /
    rm -rf -l 1000 $S/x
}

# list LABEL IMAGE: step 1 on IMAGE.
list() {
    echo "--- dumpifs-v $1"
    dumpifs -v "$2" 2>&1
    echo "status $?"
    echo "--- dumpifs-m $1"
    dumpifs -m "$2" 2>&1
    echo "status $?"
}

for c in $(awk "\$1 == \"case\" { print \$2 }" $W/index); do
    echo "=== case $c"
    get "$c.ifs" $I || continue
    echo "--- cksum fsimg"
    cksum < $I
    list fsimg $I
    extract fsimg $I "$c"
    mkdir -p $W/c
    for m in $(awk -v p="$c." "\$1 == \"member\" && index(\$2, p) == 1 { print \$2 }" $W/index); do
        case ${m#"$c".} in
        *[!0-9]*) ;;
        *) get "$m" $W/c/${m#"$c".} ;;
        esac
    done
    get "$c.bld" $W/build
    echo "--- mkifs"
    rm -f $N
    mkifs -nn $W/build $N 2>&1
    echo "status $?"
    if test -f $N; then
        echo "--- cksum native"
        cksum < $N
        list native $N
        extract native $N "$c"
    fi
    rm -rf $W/c $W/build $W/dirs $I $N
done
echo "=== members"
for m in $(awk "\$1 == \"member\" { print \$2 }" $W/index); do
    get "$m" $W/m
    echo "$m $(cksum < $W/m)"
done
echo "=== end"
