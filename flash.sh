#!/bin/bash
#
# FRANK DOS — flash the board with nobody at it (frank-ecat / frank-react way):
#   - the running firmware is sent "~BOOT~" on its USB CDC console and
#     reboots into BOOTSEL;
#   - picotool loads and verifies the image (-v -x) by the chip's serial;
#   - a load that did not say OK fails the script.
#
# Usage: ./flash.sh [image.uf2]          (default: the newest build/lib/*.uf2)
# Environment: MASTER_SER (default: FRANK Next master on the bench)
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: MIT

set -u
cd "$(dirname "$0")"
SER=${MASTER_SER:-A300F93AED82B232}
IMG=${1:-$(ls -t build/lib/*.uf2 2>/dev/null | head -1)}
[ -n "$IMG" ] && [ -f "$IMG" ] || { echo "no image: run ./build.sh" >&2; exit 1; }

in_bootsel() { timeout 3 picotool info -d --ser "$SER" >/dev/null 2>&1; }

# the chip's CDC port, found by its USB serial number
port_of() {
    python3 -c "
import sys
from serial.tools import list_ports
for p in list_ports.comports():
    if p.serial_number == sys.argv[1]:
        print(p.device); break" "$SER"
}

# "~BOOT~" again every 5 s: a firmware still booting (welcome screen)
# reads its console only once its main loop runs. 40 s in all.
end=$((SECONDS + 40)); last=-10
while [ $SECONDS -lt $end ] && ! in_bootsel; do
    if [ $((SECONDS - last)) -ge 5 ]; then
        last=$SECONDS
        PORT=$(port_of)
        [ -n "$PORT" ] && timeout 5 python3 -c "import serial,sys,time; s=serial.Serial(sys.argv[1], write_timeout=2); s.write(b'~BOOT~'); time.sleep(0.5)" "$PORT" 2>/dev/null
    fi
    sleep 1
done
# a firmware that died before (or with) its console: the boot ROM's reboot
# over the debug probe (tools/swd_bootsel.sh; nothing written over SWD)
if ! in_bootsel; then
    echo "no answer on the console: rebooting to BOOTSEL over SWD"
    tools/swd_bootsel.sh
    for _ in $(seq 1 20); do in_bootsel && break; sleep 1; done
fi
in_bootsel || { echo "board $SER did not reach BOOTSEL" >&2; exit 1; }

echo "flashing $IMG"
out=$(timeout 300 picotool load -v -x --ser "$SER" "$IMG" 2>&1 | tr '\r' '\n')
if ! echo "$out" | grep -q "^ *OK" || echo "$out" | grep -qE "FAILED|ERROR"; then
    echo "$out" | grep -v "^Loading\|^Verifying\|^$" | tail -5 >&2
    echo "the flash does not hold the image" >&2
    exit 1
fi
echo "verified and started"

# the console back (the firmware waits for it a few seconds at boot)
for _ in $(seq 1 30); do [ -n "$(port_of)" ] && { echo "console: $(port_of)"; exit 0; }; sleep 1; done
echo "warning: no console port after 30 s" >&2
