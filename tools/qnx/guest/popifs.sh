#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# popifs.sh GROUP: build the IFS experiment images of one group in the QNX
# 6.5.0 guest and write each to /dev/hd0 at a 512 KiB slot.
#
# Each case prints a "CASE slot name rc bytes cksum" line, its buildfile and
# mkifs output, then `dumpifs -v`. Slot N starts at byte N * 524288 of hd0;
# the host cuts each image out of the capture by that offset and its length.
# Groups: basic (E1, E2, E4, E5, E9), names (E3), data (E6, E7, dups),
# extra (E3 limits, E7 on larger data).  Work happens in /tmp (a 4 MiB RAM disk), so cases stay small.

group=$1
W=/tmp/ifsw
slot=0
rm -rf $W
mkdir -p $W/src
cd $W

emit() {
    # emit NAME [MKIFS-OPTIONS]: build $W/b into $W/o and store it.
    name=$1
    shift
    rm -f o
    mkifs "$@" b o > e 2>&1
    rc=$?
    if test -f o; then
        bytes=$(ls -l o | awk "{print \$5}")
        sum=$(cksum o | awk "{print \$1}")
        dd if=o of=/dev/hd0 bs=512 seek=$((slot * 1024)) conv=notrunc 2>/dev/null
    else
        bytes=-; sum=-
    fi
    echo "CASE $slot $name rc=$rc bytes=$bytes cksum=$sum"
    echo "--- buildfile"
    cat b
    echo "--- mkifs"
    cat e
    if test -f o; then
        echo "--- dumpifs"
        dumpifs -v o 2>&1
    fi
    echo "--- end"
    slot=$((slot + 1))
}

put() {
    # put PATH CONTENT: a host file under src
    mkdir -p "$(dirname "$W/src/$1")"
    printf "%s" "$2" > "$W/src/$1"
}

case $group in
basic)
    put a.txt hello
    put d1/d2/b.txt nested
    printf "a.txt=%s/src/a.txt\n" $W > b
    emit e1-one
    emit e1-one-n -n
    emit e1-one-n2 -n
    emit e1-one-nn -nn
    emit e1-one-nn2 -nn
    : > b
    emit e1-empty -nn
    printf "a.txt=%s/src/a.txt\nd1/d2/b.txt=%s/src/d1/d2/b.txt\n" $W $W > b
    emit e1-nested -nn
    printf "[type=dir]d1\n[type=dir]d1/d2\na.txt=%s/src/a.txt\nd1/d2/b.txt=%s/src/d1/d2/b.txt\n" $W $W > b
    emit e1-nested-explicit -nn
    printf "[type=dir]d1\n[type=dir]d1/d2\n" > b
    emit e1-dirs-only -nn
    printf "z.txt=%s/src/a.txt\nb.txt=%s/src/a.txt\nm/x=%s/src/a.txt\na=%s/src/a.txt\n" $W $W $W $W > b
    emit e1-order -nn
    for m in /foo / "" /a/very/long/mount/point/for/the/header/padding/rules/0123456789; do
        printf "[mount=%s]\na.txt=%s/src/a.txt\n" "$m" $W > b
        emit "e2-mount-$(echo "x$m" | tr / _)" -nn
    done
    printf "[prefix=pfx/sub]\na.txt=%s/src/a.txt\n" $W > b
    emit e2-prefix -nn
    printf "[prefix=/abs]\na.txt=%s/src/a.txt\n" $W > b
    emit e2-prefix-abs -nn
    printf "[type=link]rel=a.txt\n[type=link]abs=/proc/boot/a.txt\n[type=link]dangling=no/such/target\n[type=link]d1/up=../a.txt\na.txt=%s/src/a.txt\n" $W > b
    emit e4-links -nn
    t=$(printf "%01024d" 0 | tr 0 t)
    printf "[type=link]long=%s\n" "$t" > b
    emit e4-link-1k -nn
    t=$(printf "%04000d" 0 | tr 0 t)
    printf "[type=link]long=%s\n" "$t" > b
    emit e4-link-4000 -nn
    printf "[type=fifo]p\n[type=dir]d\n[type=dir perms=0700 uid=7 gid=8]e\n" > b
    emit e4-special -nn
    printf "[type=chr]c\n" > b
    emit e4-chr -nn
    for u in 0 65535 4294967295; do
        printf "[uid=%s gid=%s]a.txt=%s/src/a.txt\n" $u $u $W > b
        emit "e5-id-$u" -nn
    done
    for p in 0000 0644 0755 4755 7777; do
        printf "[perms=%s]a.txt=%s/src/a.txt\n" $p $W > b
        emit "e5-perms-$p" -nn
    done
    touch -t 197001010000 src/a.txt
    printf "a.txt=%s/src/a.txt\n" $W > b
    emit e5-mtime-epoch
    touch -t 203801190314 src/a.txt
    emit e5-mtime-2038
    touch -t 210601010000 src/a.txt 2>/dev/null
    emit e5-mtime-2106
    printf "[+bigendian]\na.txt=%s/src/a.txt\n[type=link]l=a.txt\n[type=fifo]p\nd1/d2/b.txt=%s/src/d1/d2/b.txt\n" $W $W > b
    emit e9-bigendian -nn
    ;;
names)
    put a.txt hello
    for n in 1 254 255 256 300 1000; do
        nm=$(printf "%0${n}d" 0 | tr 0 n)
        printf "%s=%s/src/a.txt\n" "$nm" $W > b
        emit "e3-comp-$n" -nn
    done
    p=x
    i=1
    while test $i -lt 32; do p=$p/x; i=$((i + 1)); done
    printf "%s=%s/src/a.txt\n" "$p" $W > b
    emit e3-depth-32 -nn
    i=1
    q=d
    while test $i -lt 200; do q=$q/d; i=$((i + 1)); done
    printf "%s=%s/src/a.txt\n" "$q" $W > b
    emit e3-depth-200 -nn
    c=$(printf "%0200d" 0 | tr 0 c)
    p=$c/$c/$c/$c/$c/$c/$c/$c/$c/$c
    printf "%s=%s/src/a.txt\n" "$p" $W > b
    emit e3-path-2010 -nn
    printf "%s/%s/%s=%s/src/a.txt\n" "$p" "$p" "$p" $W > b
    emit e3-path-6032 -nn
    printf "caf\303\251=%s/src/a.txt\nlat\351=%s/src/a.txt\nsp ace=%s/src/a.txt\n" $W $W $W > b
    emit e3-nonascii -nn
    printf "[type=dir]top\ntop/a=%s/src/a.txt\nimp/b=%s/src/a.txt\n" $W $W > b
    emit e3-parents -nn
    printf "./dot=%s/src/a.txt\nx/../y=%s/src/a.txt\n//dbl=%s/src/a.txt\nemp//ty=%s/src/a.txt\n" $W $W $W $W > b
    emit e3-odd-paths -nn
    ;;
data)
    for s in 0 1 3 4 65535 65536 65537; do
        dd if=/dev/zero bs=1 count=$s 2>/dev/null | tr "\000" q > src/f$s
    done
    printf "" > b
    for s in 0 1 3 4 65535 65536 65537; do
        printf "f%s=%s/src/f%s\n" $s $W $s >> b
    done
    emit e6-sizes -nn
    printf "one=%s/src/f65537\ntwo=%s/src/f65537\n" $W $W > b
    emit e6-identical -nn
    printf "[+page_align]a=%s/src/f3\n[-page_align]b=%s/src/f3\n" $W $W > b
    emit e6-page-align -nn
    printf "a=%s/src/f3\nb=%s/src/f1\n" $W $W > b
    emit e6-pad -nn
    printf "same=%s/src/f3\nsame=%s/src/f1\n" $W $W > b
    emit e6-dup-file -nn
    printf "[type=link]same=f1\nsame=%s/src/f3\nf1=%s/src/f1\n" $W $W $W > b
    emit e6-dup-link-file -nn
    printf "same=%s/src/f3\n[type=link]same=f1\nf1=%s/src/f1\n" $W $W $W > b
    emit e6-dup-file-link -nn
    printf "d=%s/src/f3\nd/x=%s/src/f1\n" $W $W > b
    emit e6-file-parent -nn
    printf "[compress=1]\na=%s/src/f65537\n" $W > b
    emit e7-compress-1 -nn
    ;;
extra)
    put a.txt hello
    for n in 1010 1020 1022 1023 1024 1025 1030; do
        nm=$(printf "%0${n}d" 0 | tr 0 n)
        printf "%s=%s/src/a.txt\n" "$nm" $W > b
        emit "e3-path-$n" -nn
    done
    c=$(printf "%0200d" 0 | tr 0 c)
    printf "%s/%s/%s/%s/%s=%s/src/a.txt\n" $c $c $c $c $c $W > b
    emit e3-path-1004-deep -nn
    printf "caf\303\251=%s/src/a.txt\nlat\351=%s/src/a.txt\n" $W $W > b
    emit e3-nonascii -nn
    printf "[type=link]l=caf\303\251\n" > b
    emit e4-link-nonascii -nn
    for n in 1000 1023 1024; do
        t=$(printf "%0${n}d" 0 | tr 0 t)
        printf "[type=link]l=%s\n" "$t" > b
        emit "e4-link-$n" -nn
    done
    i=0
    : > src/mid
    while test $i -lt 3; do cat /bin/mkifs /bin/dumpifs /boot/sys/startup-bios >> src/mid; i=$((i + 1)); done
    ls -l src/mid
    for c in 2 3; do
        printf "[compress=%s]\nmid=%s/src/mid\nf=%s/src/a.txt\n" $c $W $W > b
        emit "e7-compress-$c" -nn
    done
    printf "[+compress]\nmid=%s/src/mid\n" $W > b
    emit e7-compress-plus -nn
    printf "[+bigendian compress=2]\nmid=%s/src/mid\n" $W > b
    emit e7-compress-2-be -nn
    rm -f src/mid
    ;;
*)
    echo "unknown group $group" >&2
    exit 2 ;;
esac
echo "SLOTS $slot"
