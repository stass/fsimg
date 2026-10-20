#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# put.sh LOCAL GUEST-PATH
# Copy a small text file, such as a script from ../guest/, into the guest over
# the serial shell, one line per command, then compare POSIX cksum output on
# both sides. Each line travels inside single quotes through the guest's tty
# line editor, so the file must hold only printable ASCII and tabs, no single
# quote, and end with a newline; anything else is refused up front.
set -eu
. "$(dirname -- "$0")/env.sh"
if test $# -ne 2; then
    echo "usage: put.sh LOCAL GUEST-PATH" >&2
    exit 2
fi
local_file=$1; guest_path=$2
if ! test -f "$local_file" || ! test -r "$local_file"; then
    echo "put.sh: cannot read $local_file" >&2
    exit 1
fi
case $guest_path in
/*) ;;
*) echo "put.sh: guest path must be absolute" >&2; exit 1 ;;
esac
case $guest_path in
*[!A-Za-z0-9/._-]*)
    echo "put.sh: unsupported character in guest path" >&2; exit 1 ;;
esac
if LC_ALL=C grep -q "[^[:print:]	]" "$local_file" ||
    grep -q "'" "$local_file"; then
    echo "put.sh: $local_file holds a single quote or a control byte" >&2
    exit 1
fi
last=$(tail -c 1 "$local_file" | od -An -c | tr -d ' ')
if test -s "$local_file" && test "$last" != '\n'; then
    echo "put.sh: $local_file does not end with a newline" >&2
    exit 1
fi
harness_lock
qsh() { python3 -I "$T/qsh.py" "$SER" "$@"; }
qsh ": > $guest_path" 10 >/dev/null
while IFS= read -r line; do
    qsh "print -r -- '$line' >> $guest_path" 10 >/dev/null
done < "$local_file"
want=$(cksum < "$local_file" | awk '{ print $1, $2 }')
got=$(qsh "cksum < $guest_path" 10 | awk '{ print $1, $2 }')
if test "$want" != "$got"; then
    echo "put.sh: checksum mismatch: host $want, guest $got" >&2
    exit 1
fi
