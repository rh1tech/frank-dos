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
