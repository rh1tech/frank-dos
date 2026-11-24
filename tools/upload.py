#!/usr/bin/env python3
#
# frank-386 — copy a file to the board's SD card over the console
# (frank-ecat's tools/upload.py, paced: the console can be a UART with no
# flow control, so the board answers "+" for every 512 bytes it has written)
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: MIT
#
# Usage: upload.py LOCAL_FILE /PATH/ON/CARD [PORT]
# The firmware ("~C put PATH SIZE~") makes the directories and writes the file.

import os
import sys
import time

import serial


def checksum(data):
    s = 0
    for b in data:
        s = (s * 31 + b) & 0xffffffff
    return s


def main():
    if len(sys.argv) < 3:
        sys.exit('usage: upload.py LOCAL REMOTE [PORT]')
    local, remote = sys.argv[1], sys.argv[2]
    port = sys.argv[3] if len(sys.argv) > 3 else '/dev/cu.usbmodem2134102'
    data = open(local, 'rb').read()
    s = serial.Serial(port, 115200, timeout=0.2)
    s.reset_input_buffer()
    s.write(('~C put %s %d~' % (remote, len(data))).encode())
    buf = b''
    t = time.time() + 5
    while b'PUT READY' not in buf and b'PUT FAIL' not in buf and time.time() < t:
        buf += s.read(256)
    if b'PUT READY' not in buf:
        sys.exit('upload: no PUT READY: %r' % buf[-200:])
    t0 = time.time()
    for i in range(0, len(data), 512):
        s.write(data[i:i + 512])
        t = time.time() + 5
        while time.time() < t:
            c = s.read(1)
            if c == b'+':
                break
        else:
            sys.exit('upload: no answer after %d bytes' % i)
        if (i // 512) % 128 == 0:
            print('\r%d KB' % (i // 1024), end='', flush=True)
    buf = b''
    t = time.time() + 10
    while b'PUT OK' not in buf and b'PUT FAIL' not in buf and time.time() < t:
        buf += s.read(256)
    line = [l for l in buf.decode(errors='replace').splitlines() if 'PUT' in l]
    line = line[-1] if line else 'no answer'
    want = '%08x' % checksum(data)
    good = 'PUT OK' in line and line.split()[-1] == want
    print('\rupload %s -> %s: %s (%.0f s)%s' % (os.path.basename(local), remote, line,
          time.time() - t0, '' if good else ', checksum should be ' + want))
    sys.exit(0 if good else 1)


if __name__ == '__main__':
    main()
