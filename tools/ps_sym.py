#!/usr/bin/env python3
"""FRANK DOS: name the PC sampler's buckets (~C ps~ lines on stdin) from the ELF:
every function a bucket overlaps, the hottest first.

    ps_sym.py build/frank.elf < ps.txt
"""
import bisect, re, subprocess, sys

NM = '/Users/A2506402/.pico-sdk/toolchain/14_2_Rel1/bin/arm-none-eabi-nm'
syms = []
for line in subprocess.run([NM, '-S', '--defined-only', '-C', sys.argv[1]],
                           capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 3)
    if len(p) == 4 and p[2].lower() in 'tw':
        syms.append((int(p[0], 16) & ~1, int(p[1], 16), p[3]))
syms.sort()
starts = [a for a, _, _ in syms]
for line in sys.stdin:
    m = re.match(r'ps ([0-9a-f]{8}) ([sf]) (\d+) (\S+)', line)
    if not m:
        if line.startswith('ps:'):
            print(line.rstrip())
        continue
    lo = int(m.group(1), 16)
    hi = lo + (128 if m.group(2) == 's' else 2048)
    i = max(bisect.bisect_right(starts, lo) - 1, 0)
    names = []
    while i < len(syms) and syms[i][0] < hi:
        a, n, name = syms[i]
        if a + max(n, 1) > lo and name not in names:
            names.append(name)
        i += 1
    print('%s %6s %-6s %s' % (m.group(1), m.group(3), m.group(4), ', '.join(names[:4]) or '?'))
