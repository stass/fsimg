#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# Print the native manifest of the QNX4 volume on /dev/hd0, mounted read-only:
# one line per path in sorted order with ls -ld output, and a cksum line for
# each regular file. Upload with put.sh and run through capture.sh -i with a
# capture image, so the manifest is recorded next to that image:
#   put.sh tools/qnx/guest/manifest4.sh /tmp/manifest4.sh
#   capture.sh -i IMAGE q4-manifest-NAME "sh /tmp/manifest4.sh"
# A volume that is not exactly /dev/hd0 is first copied into a file of
# BLOCKS blocks given as the first argument.
dev=/dev/hd0
if test -n "$1"; then
    dd if=/dev/hd0 of=/w/m.img bs=512 count="$1" 2>/dev/null || exit 1
    dev=/w/m.img
fi
mkdir -p /q4 && mount -t qnx4 -r "$dev" /q4 || exit 1
cd /q4 || exit 1
find . -print | sort | while read -r f; do
    ls -ld "$f"
    if test -f "$f" && ! test -L "$f"; then cksum "$f"; fi
done
cd / && umount /q4
