#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# Drive the live CD's graphical boot through the QEMU monitor: answer the boot
# prompts, log in as root and open a terminal. The delays and pointer offsets
# are fixed values measured on the development host; the last step saves a
# screenshot in $VM/boot.png to check the result. Run by vmstart.sh.
set -eu
. "$(dirname -- "$0")/env.sh"
harness_lock
mon() { python3 -I "$T/mon.py" "$MON" "$@" >/dev/null; }
sleep 70; mon "sendkey f2"
sleep 50; mon "sendkey alt-x"
sleep 20; python3 -I "$T/type.py" "$MON" 'root\n'
sleep 20; python3 -I "$T/click.py" "$MON" 233 -195
sleep 3; python3 -I "$T/click.py" "$MON" 15 122
sleep 5; "$T/shot.sh" "$VM/boot.png"
