# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# Shared settings for the QNX 6.5.0 VM harness, sourced (not run) by the other
# scripts in this directory. FSIMG_QNX_VM is the VM working directory (disks,
# sockets, pid file), FSIMG_QNX_ISO the QNX 6.5.0 x86 live CD and
# FSIMG_QNX_CAPTURES where new captures go; its default is deliberately not
# the directory holding the recorded evidence, and no script overwrites an
# existing capture. Nothing here writes into the source tree.
T=$(CDPATH= cd -P -- "$(dirname -- "$0")" && pwd)
VM=$(CDPATH= cd -P -- "${FSIMG_QNX_VM:-$HOME/qnx65-vm}" && pwd)
ISO=${FSIMG_QNX_ISO:-$HOME/qnx-6.5.0-x86-201007091524.iso}
CAPTURES=${FSIMG_QNX_CAPTURES:-$VM/new-captures}
# The second disk, /dev/hd1, relative to $VM unless absolute.
HD1=${FSIMG_QNX_HD1:-big.img}
MON=$VM/mon.sock
SER=$VM/ser.sock

# Scripts redefine this to remove their partial outputs on any exit.
harness_cleanup() { :; }

# Every script that sends commands or keystrokes to the guest holds this lock
# for its whole run, so two captures or uploads can never interleave in the
# one guest shell. The Python helpers that talk to the sockets refuse to run
# unless the lock directory exists and FSIMG_QNX_LOCKED, which only a locked
# script exports, names it; this guards against accidental direct use. mkdir
# is atomic on every host; a script started by a locked script inherits the
# lock through FSIMG_QNX_LOCKED. A lock left by a killed script names its pid
# in $VM/harness.lock/pid and is removed by hand.
harness_lock() {
    if test "${FSIMG_QNX_LOCKED:-}" = "$VM"; then
        return 0
    fi
    if ! mkdir "$VM/harness.lock" 2>/dev/null; then
        echo "$(basename -- "$0"): harness busy (lock $VM/harness.lock," \
            "pid $(cat "$VM/harness.lock/pid" 2>/dev/null || echo unknown))" >&2
        exit 1
    fi
    trap 'harness_cleanup; rm -rf "$VM/harness.lock"' EXIT
    trap 'exit 130' HUP INT TERM
    echo $$ > "$VM/harness.lock/pid"
    FSIMG_QNX_LOCKED=$VM
    export FSIMG_QNX_LOCKED
}

# Capture names become file names under $CAPTURES.
check_capture_name() {
    case $1 in
    ''|.*|*[!A-Za-z0-9._-]*)
        echo "$(basename -- "$0"): bad capture name '$1'" >&2
        exit 1 ;;
    esac
}
