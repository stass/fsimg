#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# Send QEMU monitor commands over the Unix socket; print the replies.
import os, socket, sys, time
# Only a harness script holding the lock (see env.sh) may drive the guest.
_vm = os.path.dirname(os.path.realpath(sys.argv[1])) if len(sys.argv) > 1 else ''
if os.environ.get('FSIMG_QNX_LOCKED') != _vm or \
        not os.path.isdir(os.path.join(_vm, 'harness.lock')):
    sys.exit(os.path.basename(sys.argv[0]) +
             ': run through a harness script that holds the lock')
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.settimeout(2)
def rd():
    out=b''
    try:
        while len(out) < 1<<20:
            c=s.recv(65536)
            if not c: break
            out+=c
    except socket.timeout: pass
    return out
rd()
for cmd in sys.argv[2:]:
    s.sendall(cmd.encode()+b'\n'); time.sleep(0.3)
print(rd().decode(errors='replace'))
