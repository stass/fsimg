#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# Guest-side population recipe for the second populated captures (pop2-*):
# long names of 28 bytes in two-byte UTF-8 and of 30 bytes in three-byte
# CJK UTF-8; a file written, appended to and chmodded three seconds apart to
# order its timestamps; and files and a directory deleted after a remount, to
# observe freed directory slots, inodes and blocks. Upload and run it like
# pop.sh.
mkdir -p /mnt/t && mount -t qnx6 /dev/hd0 /mnt/t || exit 1
cd /mnt/t
e=$(printf "\303\251")
n=$e$e$e$e$e$e$e$e$e$e$e$e$e$e
echo -n u > "$n"
echo -n v > "$(printf "\344\270\255\344\270\255\344\270\255\344\270\255\344\270\255\344\270\255\344\270\255\344\270\255\344\270\255\344\270\255")"
echo -n t1 > times; sleep 3
echo -n t2 >> times; sleep 3
chmod 600 times; sleep 3
cat times >/dev/null
echo -n a > gone1; echo -n b > gone2; mkdir gonedir
cd /; umount /mnt/t
mount -t qnx6 /dev/hd0 /mnt/t || exit 1
rm /mnt/t/gone1; rmdir /mnt/t/gonedir
ls -la /mnt/t; date
umount /mnt/t
