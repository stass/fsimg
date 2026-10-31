#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# Guest-side native qualification of a volume fsimg wrote, the fixture tree
# that test-qnx6-write --create builds: check it, mount it, read every file,
# then create, grow, link, rename and delete natively, sync, unmount, check,
# remount, read again, unmount and check again. Any failing step stops the
# script with its status. Upload it and capture with, for example:
#   test-qnx6-write --create DIR little 1024 67108864
#   put.sh ../guest/qualify.sh /tmp/qualify.sh
#   capture.sh -i DIR/writer.img writer-b1024-le-qualify "sh /tmp/qualify.sh"
set -e
d=/dev/hd0
m=/mnt/t
mkdir -p $m
echo "== chkqnx6fs before mount"
chkqnx6fs -vv $d
echo "== mount and read"
mount -t qnx6 $d $m
ls -laR $m
find $m -type f | sort | while read f; do cksum "$f"; done
ls -l $m/link $m/fifo
echo "== create, grow, link, rename, delete"
echo created > $m/new.txt
mkdir $m/newdir
echo nested > $m/newdir/inner.txt
echo longer > $m/newdir/a-native-name-longer-than-twenty-seven-bytes.txt
dd if=/dev/zero bs=1024 count=3000 2>/dev/null | tr "\000" "x" >> $m/big.bin
echo appended >> $m/hello.txt
ln $m/new.txt $m/new-hardlink.txt
ln -s new.txt $m/new-symlink
mv $m/empty $m/renamed-empty
mv $m/dir/abcdefghijklmnopqrstuvwxyz01 $m/dir/renamed-to-a-name-longer-than-27-bytes
rm $m/zero.bin $m/dir/sub/deep.bin
rmdir $m/dir/sub
sync
ls -laR $m
umount $m
echo "== chkqnx6fs after native writes"
chkqnx6fs -vv $d
echo "== remount and read"
mount -t qnx6 $d $m
ls -laR $m
find $m -type f | sort | while read f; do cksum "$f"; done
umount $m
echo "== chkqnx6fs after remount"
chkqnx6fs -vv $d
