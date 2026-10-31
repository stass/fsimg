#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# Type text into the guest through QEMU monitor sendkey. Usage: type.py SOCKET
# TEXT, where TEXT takes Python backslash escapes, so '\n' presses Return.
import os, socket, sys, time
names = {' ':'spc','\n':'ret','-':'minus','=':'equal','/':'slash','.':'dot',',':'comma',';':'semicolon',"'":'apostrophe','`':'grave_accent','[':'bracket_left',']':'bracket_right','\\':'backslash','\t':'tab'}
shifted = {'_':'minus','+':'equal','?':'slash','>':'dot','<':'comma',':':'semicolon','"':'apostrophe','~':'grave_accent','{':'bracket_left','}':'bracket_right','|':'backslash','!':'1','@':'2','#':'3','$':'4','%':'5','^':'6','&':'7','*':'8','(':'9',')':'0'}
# Only a harness script holding the lock (see env.sh) may drive the guest.
_vm = os.path.dirname(os.path.realpath(sys.argv[1])) if len(sys.argv) > 1 else ''
if os.environ.get('FSIMG_QNX_LOCKED') != _vm or \
        not os.path.isdir(os.path.join(_vm, 'harness.lock')):
    sys.exit(os.path.basename(sys.argv[0]) +
             ': run through a harness script that holds the lock')
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
text = sys.argv[2].encode().decode('unicode_escape')
for c in text:
    if c in names: k = names[c]
    elif c in shifted: k = 'shift-' + shifted[c]
    elif c.isupper(): k = 'shift-' + c.lower()
    else: k = c
    s.sendall(b'sendkey ' + k.encode() + b'\n'); time.sleep(0.04)
time.sleep(0.3)
