#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
"""Run one shell command in the guest over the serial shell.

Usage: qsh.py [-s STATUS-FILE] SOCKET COMMAND [TIMEOUT-SECONDS]

Prints the command's output byte for byte (CR LF becomes LF). The end of
output is found by a random marker the guest shell echoes after the command,
together with the command's exit status.

Without -s the exit status is the guest command's, which suits interactive
use; a harness failure then exits 124 (timeout, lost connection, output cap)
or 1 (refused command, missing lock, connection error), which a guest status
can also be. With -s the guest status is written to STATUS-FILE and the exit
status reports only the harness: 0 when the command completed, nonzero otherwise
(refused command, lost connection, timeout, output cap). Callers that record
evidence use -s so a harness failure can never pass for a guest status.

COMMAND must be one line without CR: a line break would make the
interactive guest shell print prompts into the output. On timeout or when
output exceeds the cap, the last 4 KiB of output and a notice go to stdout;
the guest command may still be running, so nothing after it should trust the
guest state.

The guest has one shell, so this runs only under the harness lock that the
scripts in this directory take (see env.sh); guest.sh wraps it for one-off
commands.
"""
import os
import socket
import sys
import time
import uuid

CAP = 8 << 20

args = sys.argv[1:]
status_file = None
if args[:1] == ['-s']:
    status_file = args[1]
    args = args[2:]
if len(args) not in (2, 3):
    sys.exit('usage: qsh.py [-s STATUS-FILE] SOCKET COMMAND [TIMEOUT]')
sock_path, command = args[0], args[1]
timeout = float(args[2]) if len(args) == 3 else 120

vm_dir = os.path.dirname(os.path.realpath(sock_path))
if os.environ.get('FSIMG_QNX_LOCKED') != vm_dir or \
        not os.path.isdir(os.path.join(vm_dir, 'harness.lock')):
    sys.exit('qsh.py: run through a harness script that holds the lock')
if '\n' in command or '\r' in command:
    sys.exit('qsh.py: COMMAND must be a single line')


def harness_failure(out, why):
    sys.stdout.buffer.write(bytes(out[-4096:]).replace(b'\r\n', b'\n'))
    sys.stdout.buffer.write(f'\nqsh.py: {why}\n'.encode())
    sys.stdout.buffer.flush()
    sys.exit(124 if status_file is None else 1)


sock = socket.socket(socket.AF_UNIX)
sock.connect(sock_path)
sock.settimeout(1)

# Discard whatever earlier commands left in the serial buffer.
drained = 0
try:
    while drained < CAP:
        chunk = sock.recv(65536)
        if not chunk:
            break
        drained += len(chunk)
except socket.timeout:
    pass

tag = uuid.uuid4().hex[:12]
mark = f'@@{tag} '.encode()
# The empty quotes keep an echoed command line from matching the marker.
cmdline = command + f'; echo "@@""{tag} $?"'
sock.sendall((cmdline + '\n').encode())

# Collect output until the marker line is complete. The serial port delivers
# bytes in small bursts, so the marker and its line end may arrive in
# separate reads: once found, the marker position is kept, and before that
# each search starts just before the bytes not yet searched.
out = bytearray()
found = -1
searched = 0
deadline = time.time() + timeout
while True:
    if found < 0:
        found = out.find(mark, max(0, searched - len(mark)))
        searched = len(out)
    if found >= 0 and b'\n' in out[found:]:
        break
    if time.time() > deadline:
        harness_failure(out, 'timeout')
    if len(out) > CAP:
        harness_failure(out, 'output overflow')
    try:
        chunk = sock.recv(65536)
    except socket.timeout:
        continue
    if not chunk:
        harness_failure(out, 'serial connection closed')
    out += chunk

text = bytes(out[:found]).replace(b'\r\n', b'\n')
echoed = cmdline.encode() + b'\n'
if text.startswith(echoed):
    text = text[len(echoed):]
sys.stdout.buffer.write(text)
sys.stdout.buffer.flush()
guest_status = int(out[found:].split()[1])
if status_file is None:
    sys.exit(guest_status)
with open(status_file, 'w') as f:
    f.write(f'{guest_status}\n')
