#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# Move the guest pointer by (dx, dy) in small steps and left-click.
import os, socket, sys, time
# Only a harness script holding the lock (see env.sh) may drive the guest.
_vm = os.path.dirname(os.path.realpath(sys.argv[1])) if len(sys.argv) > 1 else ''
if os.environ.get('FSIMG_QNX_LOCKED') != _vm or \
        not os.path.isdir(os.path.join(_vm, 'harness.lock')):
    sys.exit(os.path.basename(sys.argv[0]) +
             ': run through a harness script that holds the lock')
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
dx, dy = int(sys.argv[2]), int(sys.argv[3])
def send(c): s.sendall(c.encode() + b'\n'); time.sleep(0.03)
while dx or dy:
    sx = max(-5, min(5, dx)); sy = max(-5, min(5, dy))
    send(f'mouse_move {sx} {sy}'); dx -= sx; dy -= sy
if len(sys.argv) < 5 or sys.argv[4] != 'noclick':
    send('mouse_button 1'); time.sleep(0.1); send('mouse_button 0')
time.sleep(0.3)
