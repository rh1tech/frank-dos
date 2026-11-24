#!/bin/bash
# FRANK DOS: a firmware that died before its USB console came up cannot be
# sent "~BOOT~". Ask the RP2350's boot ROM (its reboot API, over the debug
# probe) to restart into BOOTSEL; then ./flash.sh loads with picotool as usual.
# Nothing is written to flash over SWD.
# PROBE: the debug probe on the master (murm386's flash.sh --swd-id finds it).
PROBE=${PROBE:-E6616407E335BB29}
timeout 30 openocd -f interface/cmsis-dap.cfg -c "adapter serial $PROBE" -c "adapter speed 5000" \
    -f target/rp2350.cfg -c "init" -c "halt" -c "rp2xxx rom_api_call RB 0x0002 100 0 0" -c "exit" 2>&1 \
    | grep -E "Error|ROM API" || true
