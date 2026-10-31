#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
"""Copy a host image onto a guest disk through the serial shell.

Usage: load.py SOCKET IMAGE DEVICE

The guest writes the image through its own block layer, so it sees the new
contents without a reboot or a host-side write to a disk QEMU has open. The
image travels gzip-compressed and uuencoded in two phases: the guest switches
its serial line to raw input and reads exactly the encoded length into /tmp
with head -c, then decodes it onto DEVICE with uudecode, gzip and dd. The
guest's cksum of the written range must equal the host's; the exit status is
0 only then.

The guest's serial input overflows above about 10 KB/s, so the sender paces
itself, and its /tmp is a small RAM disk, so the encoded image is limited to
3 MiB: a mostly empty filesystem image of 64 MiB compresses far below that.
Like qsh.py this runs only under the harness lock; capture.sh -i uses it.
"""
import binascii
import gzip
import os
import socket
import subprocess
import sys
import time
import uuid

ENCODED_MAX = 3 << 20

if len(sys.argv) != 4:
    sys.exit('usage: load.py SOCKET IMAGE DEVICE')
sock_path, image, device = sys.argv[1:4]
vm_dir = os.path.dirname(os.path.realpath(sock_path))
if os.environ.get('FSIMG_QNX_LOCKED') != vm_dir or \
        not os.path.isdir(os.path.join(vm_dir, 'harness.lock')):
    sys.exit('load.py: run through a harness script that holds the lock')
if not device.startswith('/dev/') or \
        not all(c.isalnum() or c in '/._-' for c in device):
    sys.exit('load.py: unsupported guest device name')

with open(image, 'rb') as f:
    data = f.read()
if len(data) == 0 or len(data) % 512 != 0:
    sys.exit('load.py: image length must be a positive multiple of 512')
packed = gzip.compress(data, 9, mtime=0)
lines = [b'begin 644 -']
for i in range(0, len(packed), 45):
    lines.append(binascii.b2a_uu(packed[i:i + 45]).rstrip(b'\n'))
lines += [b'`', b'end', b'']
payload = b'\n'.join(lines)
if len(payload) > ENCODED_MAX:
    sys.exit('load.py: encoded image exceeds the guest /tmp budget of 3 MiB')

sock = socket.socket(socket.AF_UNIX)
sock.connect(sock_path)
sock.settimeout(1)
try:
    while sock.recv(65536):
        pass
except socket.timeout:
    pass


def run(command, timeout, data=None):
    """Send one command line, optionally followed by raw input, and return
    its output and status, found by a random marker as in qsh.py."""
    tag = uuid.uuid4().hex[:12]
    mark = f'@@{tag} '.encode()
    sock.sendall(f'{command}; r=$?; echo "@@""{tag} $r"\n'.encode())
    if data is not None:
        time.sleep(2)
        for i in range(0, len(data), 1024):
            sock.sendall(data[i:i + 1024])
            time.sleep(0.1)
    out = bytearray()
    deadline = time.time() + timeout
    while True:
        found = out.find(mark)
        if found >= 0 and b'\n' in out[found:]:
            return (bytes(out[:found]).decode(errors='replace'),
                    int(out[found:].split()[1]))
        if time.time() > deadline:
            sys.exit('load.py: guest timeout: ' +
                     bytes(out[-2000:]).decode(errors='replace'))
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            sys.exit('load.py: serial connection closed')
        out += chunk


# Phase 1: the encoded bytes into /tmp; the line editor comes back on even
# when head fails.
text, status = run(f'stty +raw -echo </dev/ser1; head -c {len(payload)} '
                   f'> /tmp/load.uu; s=$?; stty +edit -echo </dev/ser1; '
                   f'(exit $s)', 60 + len(payload) // 5000, payload)
if status != 0:
    sys.exit(f'load.py: guest transfer failed with status {status}: {text}')
# Phase 2: decode and write the device through the guest's block layer.
text, status = run(f'uudecode -p /tmp/load.uu | gzip -dc | dd of={device} '
                   f'ibs=4096 obs=65536 2>/dev/null; s=$?; rm -f /tmp/load.uu; '
                   f'(exit $s)', 600)
if status != 0:
    sys.exit(f'load.py: guest write failed with status {status}: {text}')
text, status = run(f'dd if={device} bs=512 count={len(data) // 512} '
                   f'2>/dev/null | cksum', 180)
# Both sides print the CRC and the length first; QNX cksum then names
# STDIN, so only the first two fields of its last line are compared.
expected = subprocess.run(['cksum'], input=data, capture_output=True,
                          check=True).stdout.decode().split()[:2]
lines = text.strip().splitlines()
got = lines[-1].split()[:2] if lines else []
if status != 0 or got != expected:
    sys.exit(f'load.py: guest cksum {got} differs from host {expected}')
print(f'loaded {len(data)} bytes ({len(payload)} encoded) onto {device}, '
      f'cksum {expected[0]}')
