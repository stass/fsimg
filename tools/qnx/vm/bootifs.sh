#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# bootifs.sh FLOPPY IFS SERIAL MARKER [SECONDS]
#
# Boot one x86 QNX boot image (an IFS behind the 1088-byte BIOS loader) in a
# separate headless QEMU through the live CD's El Torito floppy, and report
# whether its boot script reached MARKER.
#
# FLOPPY is the live CD's 2.88 MB El Torito boot floppy (the boot catalog's
# image at ISO LBA 31, extracted once as a file); it is never written. A copy
# with IFS written over the floppy's own boot image at byte 22528 is booted
# with
#
#   qemu-system-i386 -accel tcg -m 256 -drive file=COPY,format=raw,if=floppy
#       -boot a -serial file:SERIAL -display none -no-reboot
#
# The floppy's loader reads that extent, so IFS must fit the old image's
# 0x440 + stored_size bytes. A startup program given -D8250.3f8.115200 puts
# the console on COM1, so the boot script's display_msg lines land in
# SERIAL. The script waits until MARKER is a line of its own there (SECONDS,
# default 120, at most), stops QEMU and waits for it to exit, writes and
# prints the transcript and ends with "BOOT PASS" or "BOOT FAIL", exiting 0
# or 1. The guest VM and its harness
# lock are not involved.
set -eu
if test $# -lt 4 || test $# -gt 5; then
    echo "usage: bootifs.sh FLOPPY IFS SERIAL MARKER [SECONDS]" >&2
    exit 2
fi
floppy=$1; ifs=$2; serial=$3; marker=$4; seconds=${5:-120}
case $seconds in
''|*[!0-9]*) echo "bootifs.sh: SECONDS must be a number" >&2; exit 2 ;;
esac
if test "$serial" -ef "$floppy" || test "$serial" -ef "$ifs"; then
    echo "bootifs.sh: SERIAL must not name an input" >&2
    exit 2
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/bootifs.XXXXXX")
case $work in
*,*) rmdir "$work"; echo "bootifs.sh: a comma in TMPDIR breaks QEMU's -drive" >&2; exit 2 ;;
esac
pidfile=$work/qemu.pid
trap 'test -f "$pidfile" && kill "$(cat "$pidfile")" 2>/dev/null; rm -rf "$work"' EXIT
trap 'exit 130' HUP INT TERM

python3 -I - "$floppy" "$ifs" "$work/fd.img" <<'EOF'
import struct, sys
OFF = 22528
fd = bytearray(open(sys.argv[1], 'rb').read())
new = open(sys.argv[2], 'rb').read()
if fd[OFF:OFF + 4] != new[:4]:
    sys.exit('bootifs.sh: the image does not start with the floppy image\'s prefix bytes')
old = 0x440 + struct.unpack_from('<I', fd, OFF + 0x440 + 0x24)[0]
if OFF + old > len(fd):
    sys.exit('bootifs.sh: the floppy\'s own image runs past its end')
if len(new) > old:
    sys.exit('bootifs.sh: %d bytes do not fit the floppy\'s %d' % (len(new), old))
fd[OFF:OFF + old] = new + bytes(old - len(new))
open(sys.argv[3], 'wb').write(fd)
EOF

# QEMU writes the transcript inside the private directory; SERIAL is
# written only once the boot is over.
: > "$work/serial.txt"
qemu-system-i386 -accel tcg -m 256 -drive "file=$work/fd.img,format=raw,if=floppy" \
    -boot a -serial "file:$work/serial.txt" -display none -no-reboot \
    -monitor none -daemonize -pidfile "$pidfile"
reached() { tr -d '\r' < "$work/serial.txt" 2>/dev/null | grep -qxF -- "$marker"; }
# The verdict is taken once, at the marker or at the deadline, whichever
# comes first; the transcript is only appended to, so the copy published
# after QEMU has exited holds every line the verdict saw.
verdict=FAIL
started=$(date +%s)
while :; do
    if reached; then
        verdict=PASS
        break
    fi
    test $(($(date +%s) - started)) -lt "$seconds" || break
    sleep 1
done
pid=$(cat "$pidfile")
kill "$pid" 2>/dev/null || true
i=0
while kill -0 "$pid" 2>/dev/null && test $i -lt 30; do
    sleep 1
    i=$((i + 1))
done
kill -9 "$pid" 2>/dev/null || true
rm -f "$pidfile"
rm -f "$serial"
cp "$work/serial.txt" "$serial"
tr -d '\r' < "$serial" | cat -v
echo "BOOT $verdict"
test "$verdict" = PASS
