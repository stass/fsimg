#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# guest.sh 'COMMAND' [TIMEOUT-SECONDS]
# Run one single-line command in the guest's serial shell under the harness
# lock, print its output and exit with its status; see qsh.py for the
# statuses a harness failure gives.
set -eu
. "$(dirname -- "$0")/env.sh"
if test $# -lt 1 || test $# -gt 2; then
    echo "usage: guest.sh 'COMMAND' [TIMEOUT-SECONDS]" >&2
    exit 2
fi
harness_lock
status=0
python3 -I "$T/qsh.py" "$SER" "$@" || status=$?
exit $status
