#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# Guest-side population recipe for the first populated capture (B=1024 LE).
# Every step is native QNX 6.5.0. Upload it and capture with, for example:
#   put.sh ../guest/pop.sh /tmp/pop.sh
#   capture.sh pop1-b1024-le 'mkqnx6fs -q -b 1024 -e little /dev/hd0; sh -x /tmp/pop.sh'
mkdir -p /mnt/t && mount -t qnx6 /dev/hd0 /mnt/t || exit 1
cd /mnt/t
n27=aaaaaaaaaaaaaaaaaaaaaaaaaaa
n28=${n27}b
n510=$(printf "%0510d" 0 | tr 0 c)
echo -n x > "$n27"; echo -n y > "$n28"; echo -n z > "$n510"
touch "$(printf "\303\251")" "$(printf "a\377")" "$(printf "\200x")"
: > empty
head -c 16384 /dev/zero | tr "\000" "D" > direct16
head -c 16385 /dev/zero | tr "\000" "I" > indirect17
head -c 300000 /dev/zero | tr "\000" "L" > large
mkdir d1 d1/d2; echo -n sub > d1/d2/f
ln direct16 hardlink; ln -s direct16 symlink; mkfifo fifo
chmod 4751 direct16; chown 123:456 indirect17
cd /; umount /mnt/t
