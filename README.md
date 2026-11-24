# FRANK DOS

Native DOS for the RP2350: the FreeDOS kernel, the BIOS and the DOS services run as ARM code. x86 programs run on frank-386's i386 core, with frank-386's real-mode JIT. Native ARM programs (EZ format) run directly on the M33.

Boards: FRANK Next (master) and FRANK / Murmulator M2. Video and sound go out over HDMI (sound also over I2S); guest RAM is in PSRAM; files come from the SD card.

## How it fits together

```
core 0: x86 core (frank-react's fast loop + Thumb-2 kernel, frank-386's jit16)
          | 0F FF <id> traps in the ROM (src/hle/rom.asm)
          v
        native BIOS (src/hle): INT 08 09 10 11 12 13 14 15 16 17 18 19 1A, POST
        native FreeDOS kernel (src/fdos, DnCraptor's port): INT 20-2F, CONFIG.SYS,
          FCOM shell, XMS, EXEC, the EZ loader; on coroutines (src/fdos/dosrt.c)
        native application ABI v21 (src/ezabi): the system table at 0x10100000
        emulated hardware games program directly: VGA, SB16, OPL, PIT, PIC, 8042, DMA, CMOS
core 1: HDMI scan-out, audio mixing, OPL synthesis
```

- **The BIOS** is a 64 KB ROM in x86 (nasm), with the IBM fixed entry points (POST at E05Bh, reset at FFF0h, and so on). Every vector the BIOS owns points at a stub that traps into ARM code. Programs that hook a vector chain to the stub as they would on a PC. A call that would block (INT 16h with no key, INT 15h/86h) waits in the stub with interrupts on.
- **The kernel** is DnCraptor's ARM port of FreeDOS (murm386). It runs on coroutines whose stacks live in a 256 KB native arena at the top of PSRAM.
  - Every DOS entry, and the boot itself, gets its own coroutine. When the kernel needs guest code (a BIOS call, a device driver, a child program), it yields to the main loop for one CPU batch at a time.
  - The kernel's data sits in PSRAM too (`0x117C0000`, linker region `PSRAM_FDOS`), because SRAM is full.
- **Disk images** are opened through FatFs, for writing too (read-only when the file is). A card set up for frank-386 is used as it is: its `386/config.ini` and images (C:, D:). Writes go into the image at once and are flushed a quarter of a second after writing stops.
- **The SD card** itself is drive H: from the start, through frank-386's redirector: the shell maps it before AUTOEXEC.BAT (`src/fdos/sddrive.c`), as `mapdrive` did (which still works, and changes nothing).
- **Booting from the SD card:** with `boot=sd` in `frankdos.ini`, no hard disk image is attached and the card's own FAT is C:. The kernel boots from it: CONFIG.SYS and AUTOEXEC.BAT are read from the card's root. Floppy images stay A: and B:.

## Building and flashing

```
./build.sh -NX --usb-cdc     # FRANK Next, the USB port as the console (bench builds)
./build.sh -M2               # M2
./flash.sh                   # ~BOOT~ over the console, then picotool; SWD fallback
```

`flash.sh` falls back to `tools/swd_bootsel.sh` when the firmware doesn't answer on its console. That script asks the boot ROM, over the debug probe, to reboot into BOOTSEL; nothing is written to flash over SWD.

Optional settings go in `/.config/dos/<board>/frankdos.ini`: frank-386's `[frank-386]` keys, such as `jit=`, `cpu_freq=` and `psram_freq=`, applied on top of `config.ini`, and frank-dos's `boot=sd` (boot from the SD card, which is then C:).

## The host runner

The same CPU core, devices, BIOS and FreeDOS kernel also build for macOS (SDL2), for debugging DOS without the board:

```
cmake -S host -B build-host && make -C build-host -j8
build-host/frankdos-host -sd DIR -hda hdd1.img        # DIR: the SD card's files, images in DIR/386
build-host/frankdos-host -sd DIR -sdboot              # boot=sd: DIR is C:
build-host/frankdos-host -sd DIR -hda hdd1.img -headless -typeat 8 -type 'mapdrive\r' -shot out.ppm 20
```

- stdin takes `type TEXT`, `screen`, `regs`, `shot FILE.ppm`, `quit`.
- The guest RAM is mapped at the same offsets as the board's PSRAM, and the kernel runs on ucontext coroutines.
- Native applications load as on the board, but their ARM code cannot run on the Mac. `host/ezapp_host.c` stands in for it: it makes the application runtime's calls (`int386()` through the system table) on the application's own stack.

## The bench

The console takes these commands (USB CDC, or UART on HID builds):

| Command | What it does |
|---|---|
| `~BOOT~`, `~RST~` | BOOTSEL, reset |
| `~C regs~` | the guest CPU, the BIOS's counters, the kernel's coroutines (with code addresses for addr2line) |
| `~C screen~` | the text screen, as text |
| `~C type TEXT~` | types into the guest; `\r` is Enter, `{N}` a raw keycode, `{M+N}` N with M held (`{29+24}` is Ctrl+O) |
| `~C cat PATH~`, `~C rm PATH~`, `~C put PATH SIZE~` | files on the card (`tools/upload.py` uses `put`) |
| `~C cds~`, `~C dbg21 N~`, `~C dbg13 N~`, `~C jit~` | the kernel's drives, INT 21h and INT 13h traces, the JIT's counters |
| `~C beep~` | the speaker, through PIT channel 2 |
| `~C mouse DX DY BUTTONS~` | the PS/2 mouse moves (screen direction), buttons bit 0 left, 1 right, 2 middle |
| `~C jit N~` | the CPU at the next boot: 0 the classic interpreter, 1 the JIT (frankdos.ini) |
| `~C ps~`, `~C ps0~` | the PC sampler's hottest code (`-DPC_SAMPLE=ON` builds), `tools/ps_sym.py` names it |

Tools:

- `tools/capture.sh`: grabs the HDMI picture ("USB Video"); the HDMI audio is "USB Digital Audio".
- `tools/bench.py` and `tools/doom286_run.py`: drive DOS and the benchmarks. Results are in `docs/BENCH.md`.

## Status (2026-11-24)

**Working:**
- The native BIOS, and a guest MS-DOS booted on it.
- The native FreeDOS kernel, from the frank-386 card's `hdd1.img`: CONFIG.SYS, AUTOEXEC (`lh vc`), the FCOM shell.
- Real-mode programs: Volkov Commander, CheckIt, Wolf3D, Doom 286 (4.78 fps on the map 11 timedemo, the same as frank-386), DOOM8088 with its WAD in XMS.
- DOS/4GW Doom 1.9 in protected mode.
- Native (EZ) applications: TEST.EXE, EZ Doom (plays its demo), and TinyCC: the SDK in `\TCC` on the card (DnCraptor's `apps/tcc`, target `tcc_sdk`) passes its CHECK.BAT, compiling, converting and running a C program on the board.
- Sound over HDMI.
- The PS/2 mouse: INT 15h C2h and IRQ 12 in the BIOS, so CTMOUSE (or any PS/2 mouse driver) works. `~C mouse DX DY BUTTONS~` moves it from the console; `tools/dos/mtest.asm` shows what INT 33h reports.

**Not yet:**
- **M2 on hardware.**
- **`H:\DOOM19\DOOM.EXE`** (an ARM build of Doom; `DOOMARM.EXE` plays) stops with "Insufficient PSRAM for DOOM zone!".

**Measured and set aside** (docs/BENCH.md):
- **A native DPMI host:** DOS/4GW Doom spends 2.3% of its time in real mode.
- **The real-mode JIT:** 2.4× faster on CheckIt, slower than the interpreter on Doom 286. Hence `jit=0`.
- **Hot code pages in SRAM:** no change.
- **frank-react's fast-loop layout:** 59% slower on DOS/4GW Doom.

## Licences

MIT for frank-dos's own code; the FreeDOS kernel parts are GPL-2.0-or-later. Derived from frank-386 / murm386 (Mikhail Matveev, DnCraptor), frank-react, tiny386 (Chunhui He) and FreeDOS.
