#!/bin/bash
# FRANK Next bench build (-NX --usb-cdc): "~BOOT~" over the master's USB
# console, then picotool by the master's serial (frank-ecat's way). $1: the
# image (default the newest nxp2 UF2), MASTER_SER the chip's serial.
cd "$(dirname "$0")/.."
SER=${MASTER_SER:-A300F93AED82B232}
PORT=${PORT:-/dev/cu.usbmodem213101}
IMG=${1:-$(ls -t build/lib/nxp2-*.uf2 | head -1)}
python3 -c "import serial,time;s=serial.Serial('$PORT');s.write(b'~BOOT~');time.sleep(1)" 2>/dev/null
for i in $(seq 1 30); do picotool info -d --ser $SER >/dev/null 2>&1 && break; sleep 1; done
out=$(picotool load -v -x --ser $SER "$IMG" 2>&1)
echo "$out" | grep -E "^ *OK|ERROR|FAIL|rebooted"
echo "$out" | grep -qE "FAIL|ERROR" && exit 1
for i in $(seq 1 30); do ls $PORT >/dev/null 2>&1 && break; sleep 1; done
