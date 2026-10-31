#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# Guest-side QNX4 population recipes for the native evidence corpus
# (docs/qnx4-research.md). Each case formats /dev/hd0 with dinit, mounts it on
# /q4, populates it with native utilities and unmounts. Upload and capture:
#   put.sh tools/qnx/guest/pop4.sh /tmp/pop4.sh
#   capture.sh q4-pop-131072-default "sh /tmp/pop4.sh pop"
# Cases: pop (E3), grow (E4), frag (E5), inodes (E6), long (E7), over (E10).
case=${1:?case}
dinit -hq /dev/hd0 || exit 1
mkdir -p /q4 && mount -t qnx4 /dev/hd0 /q4 || exit 1
cd /q4 || exit 1
rep() { printf "%0${1}d" 0 | tr 0 "$2"; }
fill() { head -c "$2" /dev/zero | tr "\000" "$3" > "$1"; }
case $case in
pop)
    # Names by length: 1, 15, 16, 17, 47, 48, 49, 504, 505 and a refused 506.
    for n in 1 15 16 17 47 48 49 504 505 506; do
        echo -n "$n" > "$(rep $n n)" || echo "refused $n"
    done
    # Sizes.
    : > s0; fill s1 1 a; fill s511 511 b; fill s512 512 c; fill s513 513 d
    fill s1024 1024 e
    # Nested directories and symbolic links.
    mkdir -p d1/d2/d3 && echo -n deep > d1/d2/d3/f
    ln -s s1 rel; ln -s /q4/s1 abs; ln -s missing dangling; ln -s d1 dirlink
    mkfifo fifo
    mknod blk b 1 2 || echo "mknod refused"
    # Modes and owners.
    : > m0000; chmod 0000 m0000
    : > m0644; chmod 0644 m0644
    : > m0755; chmod 0755 m0755
    : > m4755; chmod 4755 m4755
    : > o0; chown 0:0 o0
    : > o65534; chown 65534:65534 o65534
    : > o65535; chown 65535:65535 o65535 || echo "chown 65535 refused"
    : > t0; touch -t 197001010000.00 t0
    : > tmax; touch -t 203801190314.07 tmax
    # Hard links across directories, three names, then back to one.
    echo -n hl > hl1; ln hl1 d1/hl2; ln hl1 d1/d2/hl3
    echo -n hk > hk1; ln hk1 d1/hk2; rm d1/hk2
    # Renames: short to long, long to short.
    echo -n rn > rs; mv rs "$(rep 20 r)"
    echo -n rl > "$(rep 30 l)"; mv "$(rep 30 l)" rl
    ;;
grow)
    # More than 30 entries in the root and in a subdirectory, with long names.
    i=0
    while test $i -lt 40; do
        echo -n $i > "r$i"; echo -n $i > "$(rep 20 x)$i"; i=$((i + 1))
    done
    mkdir sub; i=0
    while test $i -lt 40; do
        echo -n $i > "sub/s$i"; echo -n $i > "sub/$(rep 20 y)$i"
        i=$((i + 1))
    done
    ;;
frag)
    # Interleaved one-block appends to a file and a spacer fragment the file
    # into one extent per append: 2, 60, 61, 62, 121 and 122 extents.
    head -c 512 /dev/zero | tr "\000" x > /tmp/blk
    for n in 2 60 61 62 121 122; do
        i=0
        while test $i -lt $n; do
            cat /tmp/blk >> "x$n"; cat /tmp/blk >> "pad$n"; i=$((i + 1))
        done
    done
    ;;
inodes)
    # More long-named files than the default .inodes holds.
    i=0
    while test $i -lt 200; do echo -n $i > "$(rep 20 i)$i"; i=$((i + 1)); done
    ;;
long)
    # Long names, a shared 48-byte prefix, rename and delete.
    p=$(rep 48 p)
    echo -n a > "${p}a"; echo -n b > "${p}b"
    echo -n c > "$(rep 49 c)"; echo -n d > "$(rep 505 d)"
    mv "$(rep 49 c)" "$(rep 60 e)"; rm "${p}b"
    echo -n f > "$(rep 70 f)"
    ;;
over)
    # Appends, truncation and blocks beyond the end of a file.
    fill a1 1000 a; fill a2 100 b
    i=0
    while test $i -lt 20; do echo -n z >> a1; echo -n y >> a2; i=$((i + 1)); done
    fill big 20000 c; head -c 100 big > t; cat t > big; rm t
    ;;
*) echo "unknown case $case"; exit 2 ;;
esac
ls -laR /q4
cd / && umount /q4
