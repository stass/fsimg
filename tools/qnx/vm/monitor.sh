#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# monitor.sh 'MONITOR-COMMAND'...
# Send QEMU monitor commands under the harness lock and print the replies.
set -eu
. "$(dirname -- "$0")/env.sh"
harness_lock
python3 -I "$T/mon.py" "$MON" "$@"
