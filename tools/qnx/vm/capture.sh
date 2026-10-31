#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# capture.sh [-d hd0|hd1] [-i IMAGE] NAME 'GUEST-COMMANDS'
#
# Run one native experiment and keep its evidence: run the guest commands
# (which format and populate the disk), sync, and save the raw disk with a
# NAME.txt record of the date, guest, medium, commands, full output and the
# commands' exit status.
#
#   hd0 (default)  $VM/test0.img, 64 MiB; filled with 0xAA from inside the
#                  guest first, so a decoder can tell which bytes QNX wrote.
#                  With -i, IMAGE (a host file of at most 64 MiB, such as
#                  one fsimg created) is then written over the start of the
#                  disk by the guest itself through load.py, and the record
#                  names its SHA-256. Saved as NAME.img.gz.
#   hd1            $FSIMG_QNX_HD1, default $VM/big.img, 5 GB sparse; not
#                  reset, so it holds what earlier experiments or a host
#                  writer put there, and the record names the file.
#                  Saved as NAME.img.zst.
#
# The commands run as one line in the guest shell (a line break is refused;
# an unquoted '#' would comment out the rest). The capture is refused if NAME
# already exists, and is published only if every harness step succeeded: the
# image first, then the record, each linked into place so neither can
# overwrite an existing file. An image without its record is therefore an
# incomplete capture; publishing needs a capture directory on a filesystem
# with hard links. On any failure the temporary files are removed. The
# guest commands' own nonzero status is recorded, not treated as a failure.
set -eu
. "$(dirname -- "$0")/env.sh"
disk=hd0
load=
while getopts d:i: opt; do
    case $opt in
    d) disk=$OPTARG ;;
    i) load=$OPTARG ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
if test $# -ne 2; then
    echo "usage: capture.sh [-d hd0|hd1] [-i IMAGE] NAME 'GUEST-COMMANDS'" >&2
    exit 2
fi
if test -n "$load"; then
    if test "$disk" != hd0 || ! test -f "$load" || ! test -r "$load"; then
        echo "capture.sh: -i needs a readable image and disk hd0" >&2
        exit 2
    fi
    load_bytes=$(wc -c < "$load" | tr -d ' ')
    if test "$load_bytes" -gt 67108864; then
        echo "capture.sh: $load is larger than hd0" >&2
        exit 2
    fi
    load_sha=$(shasum -a 256 "$load" | cut -d ' ' -f 1)
fi
name=$1; cmds=$2
check_capture_name "$name"
case $cmds in
*"
"*|*"$(printf '\r')"*)
    echo "capture.sh: GUEST-COMMANDS must be a single line" >&2
    exit 2 ;;
esac
case $disk in
hd0) image=$VM/test0.img; suffix=img.gz; timeout=600 ;;
hd1) case $HD1 in /*) image=$HD1 ;; *) image=$VM/$HD1 ;; esac; suffix=img.zst; timeout=1800 ;;
*) echo "capture.sh: unknown disk $disk" >&2; exit 2 ;;
esac
record=$CAPTURES/$name.txt
saved=$CAPTURES/$name.$suffix
if test -e "$record" || test -e "$saved"; then
    echo "capture.sh: capture $name already exists in $CAPTURES" >&2
    exit 1
fi
mkdir -p "$CAPTURES"
harness_lock
tmp_record=$CAPTURES/.$name.txt.tmp
tmp_saved=$CAPTURES/.$name.$suffix.tmp
tmp_status=$CAPTURES/.$name.status.tmp
harness_cleanup() { rm -f "$tmp_record" "$tmp_saved" "$tmp_status"; }
# Temporary files a killed run left behind would block or leak into this one.
harness_cleanup

qsh() { python3 -I "$T/qsh.py" "$@"; }
guest=$(qsh "$SER" 'uname -a' 20)
if test $disk = hd0; then
    fill='tr "\000" "\252" </dev/zero | head -c 67108864 >/dev/hd0; sync'
    qsh -s "$tmp_status" "$SER" "$fill" 300 >/dev/null
    if test "$(cat "$tmp_status")" != 0; then
        echo "capture.sh: filling /dev/hd0 failed in the guest" >&2
        exit 1
    fi
    media='QEMU IDE raw 64 MiB (67108864 bytes), /dev/hd0 whole disk,'
    media="$media pre-filled with 0xAA"
    if test -n "$load"; then
        python3 -I "$T/load.py" "$SER" "$load" /dev/hd0 >&2
        media="$media, then bytes 0 to $load_bytes written by the guest"
        media="$media from host image $(basename -- "$load") (SHA-256 $load_sha)"
    fi
else
    size=$(wc -c < "$image" | tr -d ' ')
    media="QEMU IDE raw $size bytes, /dev/hd1 whole disk, sparse host file"
    media="$media $(basename -- "$image"), not reset by the capture"
fi
# printf, not echo: some sh echo builtins interpret backslashes.
{
    printf 'capture: %s\n' "$name"
    printf 'host-date: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'guest: %s\n' "$guest"
    printf 'media: %s\n' "$media"
    printf 'commands: %s\n' "$cmds"
    printf '%s\n' '---- output ----'
} > "$tmp_record"
# Keep the commands' status across the syncs. qsh.py -s writes the guest
# status to a file and fails only for harness errors, which discard the run.
if ! qsh -s "$tmp_status" "$SER" \
    "{ $cmds; } 2>&1; s=\$?; sync; sync; (exit \$s)" $timeout \
    >> "$tmp_record"; then
    echo "capture.sh: harness failed; capture $name discarded" >&2
    exit 1
fi
printf 'exit: %s\n' "$(cat "$tmp_status")" >> "$tmp_record"
if test $disk = hd0; then
    gzip -9c "$image" > "$tmp_saved"
else
    zstd -q -T0 -o "$tmp_saved" "$image"
fi
ln "$tmp_saved" "$saved"
ln "$tmp_record" "$record"
