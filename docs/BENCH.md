# FRANK DOS benchmarks

FRANK Next master, 504 MHz, PSRAM 133 MHz, flash 84 MHz, HDMI. Run with `tools/bench.py`.

- **Doom 286**: D213_286 `-timedemo demo1 map11` (`/BENCH/DOOM286` on the card, from H:), fps = gametics × 35 / realtics.
- **CheckIt 3.0**: Benchmarks → Main System (`C:\CHECKIT.V3`).

| Date | Build | Doom 286 map11 fps | CheckIt Dhrystones | CheckIt Whetstones | Notes |
|---|---|---|---|---|---|
| 2026-11-24 | frank-386 v1.08 stock, its `/.config/386/NX` settings (jit=0, flash 66) | 4.778 | 4479 | 1887.0K | reference |
| 2026-11-24 | phase 1, jit=1 (frank-386 core + JIT, native BIOS, guest MS-DOS) | 4.155 | 10868 | 2564.9K | JIT: CheckIt ×2.4, Doom 286 −13% |
| 2026-11-24 | phase 1, jit=0 (frankdos.ini) | 4.785 | 4604 | 1874.9K | same settings as the reference |

| 2026-11-24 | phase 2, native FreeDOS, jit=0 | 4.780 | | | Doom 286 from H: under the native kernel |
| 2026-11-24 | phase 2, native FreeDOS, jit=1 (frank-386 JIT) | 3.483 | | | the JIT thrashes: 3000 blocks/s compiled |
| 2026-11-24 | + adaptive threshold | 3.789 | | | compiles 16/s; the compact interpreter is the cost |
| 2026-11-24 | + 2048-entry table, 48 KB cache | 3.969 | | | still below the classic interpreter |
| 2026-11-24 | native FreeDOS, jit=0, mixer on core 1 | 5.017 | | | |
| 2026-11-25 | **jit=2, the 16-bit fast loop** (fastloop16.inc) | **9.753** | | | 1205 gametics in 4324 realtics: ×1.94 |
| 2026-11-25 | jit=2 with fastloop.inc's 32-bit loop in the same overlay | **10.140** | | | 4159 realtics |

Notes:
- `--no-jit` builds put the interpreter in a slower layout (4.412 fps, 4361, 1518.3K): compare with `jit=0` at run time, not with `--no-jit`.
- No HIMEM or EMM386 in the image's CONFIG.SYS, so Doom 286 runs without XMS on both.

## The JIT on Doom 286, profiled (2026-11-24, NX at 378 MHz)

Map 11 timedemo: the interpreter gets 4.770 fps, the JIT 3.664 fps.

Taken with `-DPC_SAMPLE=ON -DCPU_JIT16_STATS=ON`: `~C ps0~` when the demo starts, `~C ps~` and `~C jit~` at its end. `tools/ps_sym.py` names the buckets.

| | jit=0 | jit=1 |
|---|---|---|
| fps | 4.770 | 3.664 |
| samples in flash | 10% | 35% |
| blocks compiled | – | 289,685 (284,815 of them recompiles) |
| evictions | – | 45,000 |
| guest instructions run in blocks | – | 466 M in 58.9 M block runs (8 per run) |

- **Recompiles:** Doom's hot real-mode code doesn't fit the 48 KB code cache. Blocks are evicted and compiled again, about 900 a second.
- **Compiler location:** the compiler (`jit_compile*`, `jit_nat_*`) runs from XIP flash.
- **Block quality:** a block calls one helper per guest instruction, so it saves the fetch and decode but not the work.

### The PSRAM block store (2026-11-24)

Compiled blocks are also kept in PSRAM (512 KB, taken from the guest's RAM when jit=1), and come back from there instead of being compiled again (`jit_restore`).

| | fps | compiles | restores |
|---|---|---|---|
| interpreter | 4.770 | – | – |
| JIT, no store | 3.664 | 289,685 | – |
| JIT, store; an evicted block back at its next entry | 1.998 | 1,943 | 13.5 M |
| JIT, store; back only once hot again (kept) | 3.843 | 2,717 | 273,440 |

- **Working set:** Doom 286's real-mode code is about 1,900–2,700 blocks. That's an order of magnitude more than the 48 KB SRAM cache holds. A compiled block is roughly 20× the size of the x86 code it covers (a helper call per instruction).
- **Restoring on first entry** churns the ring, about 22,000 copies a second.
- **With the store, compiling no longer costs anything, and the JIT is still slower than the interpreter.** So the time is lost in the blocks themselves and in switching between blocks and the interpreter, not in compiling.

### Where the interpreter's time goes (2026-11-24)

The board runs at 504 MHz (raised at boot from the card's config.ini), PSRAM 133 MHz, flash 66 MHz.

- **The audio mixer ran on core 0.** core1_entry started it with add_repeating_timer_us(), whose default alarm pool belongs to core 0. That cost 5-8% of core 0's samples (timer_callback, __udivmoddi4). It now runs on an alarm pool core 1 creates. Doom 286: 4.770 -> **5.017 fps**. HDMI sound checked.
- **Instruction fetch:** 23% of the interpreter's samples fall in one 128-byte bucket at the head of cpu_exec1_classic: the opcode byte's load from guest RAM in PSRAM (`ldrb r3, [r2, r1]`). The XIP cache (16 KB, shared by flash code, guest code and guest data) misses, and the core waits for the PSRAM.
- **Address translation and operand decode:** translate_fast about 13%, modsib_f about 8%. Part of that is also PSRAM loads.
- **The JIT's compiled blocks take 0.5% of the time** while they run about 470 M instructions. The JIT's losses are elsewhere: jit_enter 7.6%, helpers 7.8%, the interpreter around it 27%, flash 25%.
- **Spread:** the same JIT code gave 3.843 and 4.360 fps in two builds that differ only in the sampler, so code layout moves results by about 10%.

### Hot code pages in SRAM (tried and removed, 2026-11-24)

Tried as jit=2: the JIT variant's compact interpreter, with the JIT's 60 KB of table and cache holding copies of the hottest 4 KB code pages. Fetches were redirected through ifetch.xaddr, and a store to a copied page dropped the copy.

| | fps |
|---|---|
| classic interpreter (jit=0) | 5.017 |
| compact interpreter alone | 3.766 |
| compact interpreter + code pages in SRAM (99% of page switches served from SRAM) | 3.766 (the same realtics) |

- **Fetch isn't the bottleneck:** reading the code from SRAM instead of PSRAM changed nothing. The interpreter is compute-bound, and the profile's hot "fetch" bucket is the dispatch at the top of its loop.
- **SRAM size buys speed:** the classic interpreter (137 KB, more inlined) is a third faster than the compact one (62 KB). Taking SRAM from the classic interpreter for anything costs more than that thing gains.

### DOS/4GW Doom 1.9: is a native DPMI host worth it? (2026-11-24)

`doom386 -timedemo demo1` (H:\DOOM19), with the PC sampler also counting the guest's mode at each sample and MOV CR0 changes of PE:

| layout | realtics (1710 gametics) | fps | guest in real mode | PE switches |
|---|---|---|---|---|
| classic interpreter in SRAM (the build) | 17,556 | 3.41 | 2.3% | 9,887 in 498 s (20 a second) |
| frank-react's: fast loop and kernel in SRAM, cpu_exec1 in flash (`-DCPU_EXEC1_SRAM=OFF -DCPU_JIT16=OFF`) | 27,954 | 2.14 | 2.4% | 15,856 in 797 s |

- **A native DPMI host would save at most the real-mode share:** DOS/4GW's DOS and BIOS calls and their mode switches are 2.3% of the time. Not built.
- **The fast loop loses here:** with cpu_exec1 in flash, 76% of the samples are in flash, because the fast loop falls back to cpu_exec1 for much of Doom's code. The classic interpreter in SRAM stays.

### What an x86 instruction costs (2026-11-25, Doom 286 mid-demo, 504 MHz)

Measured with `~C ps0~`, then `~C ps~` a second later (PC_SAMPLE builds: guest instructions over the window, the XIP cache's own counters), and `~C membench~`.

| | |
|---|---|
| guest instructions | 3.29 M a second, **153 cycles each** |
| XIP cache | 22 M accesses a second (6.7 per instruction), 98.8% hits, ~250 k misses a second |
| a read: SRAM / PSRAM cached / PSRAM missing the cache | 8 / 8 / 155 cycles (loop included) |
| cpu_exec1 built -O2 instead of -Os (+26 KB of SRAM; tried with a 224 KB VGA buffer) | 150 cycles each: 2% |

- **Memory:** cache misses cost about 7% of the CPU. A cache hit costs nothing over SRAM.
- **The rest is the interpreter's own instructions:** the loop head (about 45 ARM instructions with its spills), modsib_f and translate_fast calls, OptAddr round trips, lazy flags. Compiler settings don't change that much, and neither will trimming spills. Only a leaner decode for the common real-mode instructions would: a fast loop as frank-react has for flat 32-bit code.

### Opcode coverage, and the fast loop on DOS/4GW Doom (2026-11-25)

Opcode histograms (`-DEXEC1_OPHIST=ON`, `~C ophist0~` / `~C ophist~`, 60 s of each timedemo):

| | opcode values for 90% / 95% / 99% / 99.9% of the bytes run | top |
|---|---|---|
| Doom 286 (real mode) | 54 / 69 / 97 / 131 | MOV Ev,Gv 15%, MOV Gv,Ev 12%, ES: 6%, ADD 4%, MOV Sw,Ew 3% |
| DOS/4GW Doom 1.9 (32-bit) | 33 / 47 / 78 / 118 | MOV Gb,Eb 13%, GRP1 Ev,Iv 10%, 0F xx 9%, MOV Gv,Ev 9% |

frank-react's fast loop in the frank-react layout (`-DCPU_EXEC1_SRAM=OFF -DCPU_JIT16=OFF`, `-DFX_STATS=1`, `~C fxstats~`), 30 s of DOS/4GW Doom, 48.4 M instructions:
- **Its table takes 99% of the opcodes, but it runs only 54% of the instructions.**
- **37% are in 16-bit protected-mode code,** which fx_mode() refuses (code16). None of it is real mode.
- **8.7% fall back to cpu_exec1:** SHLD (0F A4) 2.4 M (Doom's fixed-point multiply), `66 89` 0.76 M, `88` 0.68 M (byte stores, likely to VGA memory), `66 0F` 0.18 M, SHRD 0.06 M, OUT/IN, PUSHA/POPA.
- **Consequence:** a fast loop for 16-bit code (segment bases, 16-bit operands and addressing) would cover real-mode games and this 37% of DOS/4GW Doom at once. SHLD/SHRD and a VGA store path would close most of the rest.

The frank-react layout is fragile on the board: `~C RST~` and some flashes leave it without a console or HDMI until it is flashed again.

### The 16-bit fast loop (2026-11-25)

DOS/4GW Doom 1.9 (`doom386 -timedemo demo1`, 1710 gametics): classic interpreter 17,556 realtics, **jit=2 13,388 realtics (×1.31)**. jit=2 is now the default (`cfg_jit`), and the settings menu's "CPU core" item cycles Classic / JIT / Fast loop.

`src/fastloop16.inc`: frank-react's fast-loop design for real-mode 16-bit code: direct-threaded dispatch, operands through the TLB tags (FX_MEM), lazy flags as i386.c keeps them. Whatever it lacks falls back to cpu_exec1 one instruction at a time. When fallbacks come back to back (code under 66h/67h), the classic core takes up to 32 instructions at once. On NX it is the SRAM overlay's third variant (`jit=2` in frankdos.ini, `~C jit 2~`): this loop (36 KB), fastloop.inc's 32-bit C loop (33 KB, without the Thumb-2 kernel, which doesn't fit) and a compact cpu_exec1 without the JIT (62 KB), cold opcodes in flash; 139,376 bytes, within the overlay's size. It also runs 16-bit protected-mode code (segment loads, far transfers, CLI and POPF fall back there) on a 16- or 32-bit stack; checked by `FZMODE=4` (16-bit protected mode, SS.B in half the cases).

Checked with `CPUBENCH_REF=worktree FZMODE=3 CUR_FLAGS=-DFX16_ENABLE=1 tools/cpubench/run.sh fuzz 20000 "1 2 3 4 5 6 7 8"`: 0 of 160,000 cases differ from cpu_exec1. The fuzzing turned up three bugs, now fixed:
- **cpu_exec1's REP MOVS/STOS/INS/OUTS ignored the 16-bit SI/DI wrap.** They chunked by page only, so a STOSW at DI=FFF0h kept going linearly past ES:FFFF. The chunks now also stop at the offset's wrap (STR_WRAP16).
- **Budget.** The fast loop checked its step budget only at jumps; it now checks it on every instruction, as fastloop.inc does. A fallback also charges what cpu_exec1 actually counted: MOV SS/POP SS takes the next instruction with it.
- **The HLE trap (0F FF) and the nested native→guest calls.** A trap that returns false now sets cpu->stop, as i386.c's UD0 does; without it the board hung at "Press F8". fastloop.inc had the same gap. Fallbacks and traps also return when an interrupt is let in.

qemu M33 (`ARM_FLAGS=-DFX16_ENABLE=1 tools/cpubench/run.sh arm16`, M33 instructions per x86 instruction):

| guest | cpu_exec1 | fast loop |
|---|---|---|
| rmloop | 80.6 | 39.0 |
| jcctest | 68.9 | 36.0 |
| smctest | 97.6 | 39.4 |
| fartest | 120.6 | 72.6 |
| dhry (gcc -m16: all 66h/67h) | 104.1 | 113.6 |
| whet (66h/67h, x87) | 220.1 | 232.2 |

What still falls back (`CUR_FLAGS="-DFX16_ENABLE=1 -DFX16_FBSTATS" run.sh bench16`): 66h/67h code, INT/IRET, POP Ev (8F), x87. 16-bit protected-mode code (DOS/4GW's 37%) is not covered yet.

**Correction (2026-11-25):** the "16-bit protected mode" share of DOS/4GW Doom (37% above, 21% in a later count) is an artifact. FX_STATS assigns a whole 4096-instruction slice to the mode it starts in, and interrupts are taken at slice start: Doom's 140 Hz timer starts about 147 slices a second in DOS/4GW's 16-bit IRQ handler, which goes back to 32-bit code within a few instructions. Running that code in fastloop16.inc changed nothing (18,245 vs 18,260 realtics). The time is in flat 32-bit code, so fastloop.inc's loop in SRAM is what gained.

