#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# Boot the QNX 6.5.0 live CD headless with $VM/test0.img as /dev/hd0 and
# $VM/big.img (or FSIMG_QNX_HD1) as /dev/hd1, log in through the graphical console and start a
# shell on the serial port ($VM/ser.sock). Refuses to start while qemu.pid
# names a live process. Stop the VM with: kill "$(cat "$VM/qemu.pid")".
# If a boot step fails, QEMU keeps running; inspect it with shot.sh.
set -eu
. "$(dirname -- "$0")/env.sh"
harness_lock
cd "$VM"
# The monitor socket can run host commands; keep the directory private.
chmod 700 "$VM"
if test -f qemu.pid && ps -p "$(cat qemu.pid)" >/dev/null 2>&1; then
    echo "vmstart.sh: VM already running (pid $(cat qemu.pid))" >&2
    exit 1
fi
rm -f mon.sock ser.sock
# The first available accelerator wins: hvf on macOS, kvm on Linux, else tcg.
qemu-system-x86_64 -accel hvf -accel kvm -accel tcg -m 1024 \
    -cdrom "$ISO" -boot d \
    -drive file=test0.img,format=raw,if=ide,index=0 \
    -drive file="$HD1",format=raw,if=ide,index=1 \
    -monitor unix:mon.sock,server,nowait -serial unix:ser.sock,server,nowait \
    -display none -vga std -daemonize -pidfile qemu.pid
"$T/boot.sh"
python3 -I "$T/type.py" "$MON" 'stty -echo +edit </dev/ser1; on -t /dev/ser1 sh &\n'
sleep 3
python3 -I "$T/qsh.py" "$SER" 'mkdir -p /mnt/t /mnt/u; ls /dev/hd1' 20
