#!/usr/bin/env python3
"""FRANK DOS bench driver: reset the board, wait for the DOS prompt, type
commands, wait for them to finish, read a result file off the card.

    bench.py doom286            Doom 286 timedemo (map 11), prints its result line
    bench.py checkit            CheckIt 3.0 Main System: Dhrystones, Whetstones
    bench.py type "TEXT"        type TEXT once DOS is up (\\r = Enter)
    bench.py screen             the text screen now
    bench.py regs               the guest CPU / BIOS counters now

The board's console is found by the master's USB serial (MASTER_SER).
Text mode is read back with the firmware's "~C screen~"; files with "~C cat~".
"""
import os
import re
import sys
import time

import serial
from serial.tools import list_ports

SER = os.environ.get('MASTER_SER', 'A300F93AED82B232')


def port():
    for p in list_ports.comports():
        if p.serial_number == SER:
            return p.device
    return None


def open_console(timeout=60):
    t = time.time()
    while time.time() - t < timeout:
        p = port()
        if p:
            try:
                return serial.Serial(p, 115200, timeout=0.3)
            except serial.SerialException:
                pass
        time.sleep(0.5)
    sys.exit('no console')


def cmd(s, c, wait=1.5):
    s.read(1 << 20)
    s.write(c.encode())
    time.sleep(wait)
    return s.read(1 << 20).decode(errors='replace')


def screen(s):
    out = cmd(s, '~C screen~')
    return '\n'.join(l[1:] for l in out.splitlines() if l.startswith('|'))


def reset_and_wait(prompt_re=r'1Help|[A-Z]:\\[^>]*>', timeout=120):
    s = open_console()
    s.write(b'~RST~')
    s.close()
    time.sleep(3)
    s = open_console()          # (the firmware waits for the console at boot)
    t = time.time()
    while time.time() - t < timeout:
        time.sleep(3)
        try:
            sc = screen(s)
        except serial.SerialException:
            s = open_console()
            continue
        if re.search(prompt_re, sc):
            return s, time.time() - t
    sys.exit('no DOS prompt after %d s' % timeout)


def wait_text_prompt(s, timeout, prompt_re=r'1Help|[A-Z]:\\[^>]*>'):
    """until the program has gone to graphics and come back to the prompt"""
    t = time.time()
    while time.time() - t < 120:
        time.sleep(2)
        if 'graphics mode' in cmd(s, '~C screen~'):
            break
    while time.time() - t < timeout:
        time.sleep(5)
        sc = screen(s)
        if sc and re.search(prompt_re, sc):
            return time.time() - t
    return None


def doom286():
    s, boot = reset_and_wait()
    print('dos up in %.0f s' % boot)
    cmd(s, '~C rm /BENCH/DOOM286/RES.TXT~', 1)
    cmd(s, '~C type mapdrive\rh:\rcd \\bench\\doom286\rtimedemo\r~', 8)
    took = wait_text_prompt(s, 1800)
    out = cmd(s, '~C cat /BENCH/DOOM286/RES.TXT~', 2)
    m = re.search(r'(\d+)\s+gametics\s+in\s+(\d+)\s+realtics', out)
    if m:
        g, r = int(m.group(1)), int(m.group(2))
        print('doom286 map11: %d gametics in %d realtics = %.2f fps (wall %.0f s)' % (g, r, g * 35.0 / r, took or -1))
    else:
        print('no result; file said:\n' + out[-800:])


def checkit():
    s, boot = reset_and_wait()
    print('dos up in %.0f s' % boot)
    cmd(s, '~C type c:\rcd \\checkit.v3\rcheckit\r~', 10)
    for keys, wait in ((' ', 6), (' ', 5), ('>>', 3), ('\r', 5)):
        cmd(s, '~C type %s~' % keys, wait)
    t = time.time()
    while time.time() - t < 180:
        time.sleep(5)
        sc = screen(s)
        d = re.search(r'(\d+) Dhrystones', sc)
        w = re.search(r'([\d.]+K) Whetstones', sc)
        if d and w:
            mhz = re.search(r'\((80\d86), ([\d.]+) MHz\)', sc)
            print('checkit 3.0: %s Dhrystones, %s Whetstones%s' % (d.group(1), w.group(1),
                  ', %s %s MHz' % mhz.groups() if mhz else ''))
            cmd(s, '~C type \x1b\x1b~', 2)
            return
    print('no checkit result:\n' + screen(s))


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else 'doom286'
    if what == 'doom286':
        doom286()
    elif what == 'checkit':
        checkit()
    elif what == 'type':
        s = open_console()
        cmd(s, '~C type %s~' % sys.argv[2].replace('\\r', '\r'), 1)
    elif what == 'screen':
        print(screen(open_console()))
    elif what == 'regs':
        print(cmd(open_console(), '~C regs~'))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
