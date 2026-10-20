#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# shot.sh [OUT.png]: save a screenshot of the guest console (default
# $VM/screen.png). Fails if QEMU produces no complete dump within 10 s.
set -eu
. "$(dirname -- "$0")/env.sh"
harness_lock
out=${1:-$VM/screen.png}
ppm=$VM/screen.ppm
case $ppm in
*[!A-Za-z0-9/._-]*)
    echo "shot.sh: $ppm cannot be passed through the monitor" >&2; exit 1 ;;
esac
rm -f "$ppm" "$out"
python3 -I "$T/mon.py" "$MON" "screendump $ppm" >/dev/null
# QEMU writes the dump asynchronously; retry until a complete image converts.
for i in 1 2 3 4 5 6 7 8 9 10; do
    sleep 1
    python3 -I "$T/ppm2png.py" "$ppm" "$out" 2>/dev/null && exit 0
done
exit 1
