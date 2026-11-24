#!/usr/bin/env python3
"""FRANK DOS: the Doom 286 timedemo under the native DOS, typed at VC; the
result read off the screen (Ctrl+O); the JIT's counters along the way."""
import sys, time
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import bench
s, boot = bench.reset_and_wait()
for c in ('mapdrive\r', 'h:\r', 'cd \\bench\\doom286\r', 'd213{42+12}286 -timedemo demo1 map11\r'):
    bench.cmd(s, '~C type %s~' % c, 6)
t = time.time()
while time.time() - t < 600:
    time.sleep(30)
    print(int(time.time() - t), bench.cmd(s, '~C jit~', 1).strip())
    if 'graphics' not in bench.cmd(s, '~C screen~', 1):
        break
bench.cmd(s, '~C type {29+24}~', 3)
print([l for l in bench.screen(s).splitlines() if 'Timed' in l])
