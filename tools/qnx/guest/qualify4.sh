#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# Guest-side native qualification of a QNX4 volume fsimg wrote: check it
# without repair, mount it and list and checksum every path, then create,
# append until a file fragments, rename, mkdir, rmdir, unlink and create long
# names natively, sync, unmount, check, remount, read again, unmount and
# check again. Any failing step stops the script with its status.
# The first argument selects the volume: 0 or nothing for the whole of
# /dev/hd0; a block count for a volume smaller than /dev/hd0, first copied
# into a file on the /w scratch volume, because chkfsys -m derives the
# expected geometry from the size of what it checks; or "hd1" for the whole
# second disk, which is modified in place and so must be a disposable copy,
# never an evidence image. A second argument "ro" skips the native writes,
# for a volume with no free block (the 43-block minimum), which is then
# only checked, read and remounted. Upload and capture with, for example:
#   test-qnx4-write --create DIR tree 4096
#   put.sh tools/qnx/guest/qualify4.sh /tmp/qualify4.sh
#   capture.sh -i DIR/writer.img q4-writer-4096-qualify "sh /tmp/qualify4.sh 4096"
set -e
d=/dev/hd0
if test "$1" = hd1; then
    # A volume written onto the second disk while the VM was stopped, such
    # as one at the size of the PCM navigation partition.
    d=/dev/hd1
elif test -n "$1" && test "$1" != 0; then
    dd if=/dev/hd0 of=/w/q.img bs=512 count="$1" 2>/dev/null
    d=/w/q.img
fi
m=/q4
mkdir -p $m
# A run that failed earlier leaves its volume mounted.
umount $m 2>/dev/null || :
check() {
    echo "== chkfsys $1"
    chkfsys -m -f -u -s -x $d </dev/null > /tmp/chk.out 2>&1 || {
        cat /tmp/chk.out; echo "chkfsys failed"; exit 1; }
    grep -v "^ *[0-9][0-9 ]*$" /tmp/chk.out
    grep -q "NEW bitmap is equal to OLD bitmap" /tmp/chk.out || {
        echo "chkfsys reported a difference"; exit 1; }
}
manifest() {
    (cd $m && find . -print | sort | while read -r f; do
        ls -ld "$f"
        if test -f "$f" && ! test -L "$f"; then cksum "$f"; fi
    done)
}
check "before mount"
echo "== mount and read"
mount -t qnx4 $d $m
manifest
if test "$2" != ro; then
echo "== create, append, rename, mkdir, rmdir, unlink, long names"
head -c 512 /dev/zero | tr "\000" x > /tmp/blk
i=0
while test $i -lt 70; do
    cat /tmp/blk >> $m/native-frag; cat /tmp/blk >> $m/native-pad
    i=$((i + 1))
done
echo created > $m/native-new
mkdir $m/native-dir
echo nested > $m/native-dir/a-native-name-longer-than-sixteen
echo long > $m/native-dir/a-native-name-longer-than-forty-eight-bytes-for-long-name-records
mkdir $m/native-dir/gone && rmdir $m/native-dir/gone
mv $m/native-new $m/native-renamed-to-a-longer-name
rm $m/native-pad
sync
manifest
fi
umount $m
check "after native writes"
echo "== remount and read"
mount -t qnx4 $d $m
manifest
umount $m
check "after remount"
# Put the natively modified volume back on /dev/hd0, which the capture saves,
# so fsimg reads what QNX wrote.
case $d in
/w/*) dd if=$d of=/dev/hd0 bs=512 2>/dev/null ;;
esac
