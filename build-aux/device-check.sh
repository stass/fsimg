#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
#
# Opt-in native reads of a disposable GPT disk, never an existing device.
# The caller needs hdiutil on macOS or noninteractive sudo for provisioning
# loop/md devices. Parsing and extraction run as the invoking user.
set -eu

test "$#" -ge 1 && test "$#" -le 2 || {
	echo "usage: $0 /absolute/path/to/fsimg [512|4096]" >&2
	exit 2
}
fsimg=$1
quantum=${2:-512}
case "$fsimg" in /*) ;; *) echo "fsimg path must be absolute" >&2; exit 2 ;; esac
case "$quantum" in 512|4096) ;; *) exit 2 ;; esac
test -x "$fsimg"
native=$(dirname "$fsimg")/test-device
test -x "$native"
host=$(uname -s)
case "$host" in
Darwin)
	if test "$quantum" != 512; then
		echo "SKIP: hdiutil fixture uses 512-byte logical sectors" >&2
		exit 77
	fi
	policy=auto
	;;
Linux|FreeBSD) policy=required ;;
*) echo "SKIP: no disposable-device fixture for $host" >&2; exit 77 ;;
esac

as_root() {
	if test "$(id -u)" -eq 0; then
		"$@"
	else
		sudo -n "$@"
	fi
}

if test "$host" != Darwin && ! as_root true; then
	echo "SKIP: disposable-device provisioning needs sudo" >&2
	exit 77
fi
fixture=$(mktemp -d "${TMPDIR:-/tmp}/fsimg-device.XXXXXX")
device=
endpoint=
cleanup() {
	status=$?
	trap - EXIT HUP INT TERM
	if test -n "$device"; then
		case "$host" in
		Darwin) hdiutil detach "$device" >/dev/null || status=1 ;;
		Linux) as_root losetup -d "$device" || status=1 ;;
		FreeBSD) as_root mdconfig -d -u "${device#/dev/md}" || status=1 ;;
		esac
	fi
	if test "$status" -eq 0; then
		rm -rf "$fixture"
	else
		echo "Retained device-check fixtures: $fixture" >&2
	fi
	exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -m 0700 "$fixture/source" "$fixture/out"
printf 'device input payload\n' >"$fixture/source/payload"
"$fsimg" c -R -F fat16/gpt -s 8M -o "gpt:sector=$quantum" \
    -f "$fixture/disk.img" -C "$fixture/source" .
case "$host" in
Darwin)
	hdiutil attach -readonly -nomount -imagekey diskimage-class=CRawDiskImage \
	    "$fixture/disk.img" >"$fixture/attach.log"
	device=$(awk '$1 ~ /^\/dev\/disk/ { print $1; exit }' "$fixture/attach.log")
	test -n "$device"
	endpoint=$device
	;;
Linux)
	device=$(as_root losetup --find --show --read-only --sector-size "$quantum" \
	    "$fixture/disk.img")
	# A private node gives the invoking user read access without changing
	# ownership or permissions of the host's reusable loop-device node.
	numbers=$(stat -c '%t %T' "$device")
	major=${numbers% *}
	minor=${numbers#* }
	endpoint=$fixture/endpoint
	as_root mknod "$endpoint" b "$((0x$major))" "$((0x$minor))"
	as_root chown "$(id -u)" "$endpoint"
	as_root chmod 0400 "$endpoint"
	;;
FreeBSD)
	unit=$(as_root mdconfig -a -t vnode -o readonly -S "$quantum" \
	    -f "$fixture/disk.img")
	device=/dev/$unit
	# mdconfig creates this node for this fixture and removes it at detach.
	as_root chown "$(id -u)" "$device"
	as_root chmod 0400 "$device"
	endpoint=$device
	;;
esac

check_reads() {
	input=$1
	"$fsimg" i -S "$policy" -T - -l spool=1 -f "$input"
	"$fsimg" t -S "$policy" -T - -f "$input"
	"$fsimg" x -O -S "$policy" -T - -f "$input" /payload \
	    >"$fixture/extracted"
	cmp "$fixture/source/payload" "$fixture/extracted"
	"$fsimg" W -S "$policy" -T - -f "$input"
}
check_reads "$endpoint"
native_status=0
"$native" --native "$endpoint" || native_status=$?
if test "$host" = Darwin && test "$native_status" -eq 77; then
	echo "SKIP: macOS device confinement unavailable" >&2
else
	test "$native_status" -eq 0
fi
"$fsimg" x -E -S "$policy" -T - -f "$endpoint" -C "$fixture/out"
cmp "$fixture/source/payload" "$fixture/out/payload"
if test "$host" = Darwin; then
	check_reads "/dev/r${device#/dev/}"
	if test -e "${device}s1"; then
		check_reads "${device}s1"
	else
		echo "SKIP: macOS fixture partition endpoint unavailable" >&2
	fi
elif test "$host" = FreeBSD; then
	if test -e "${device}p1"; then
		as_root chown "$(id -u)" "${device}p1"
		as_root chmod 0400 "${device}p1"
		check_reads "${device}p1"
	else
		echo "SKIP: FreeBSD fixture partition endpoint unavailable" >&2
	fi
fi
echo "PASS: $host disposable device, $quantum-byte logical sectors"
