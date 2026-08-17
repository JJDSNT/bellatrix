---
id: ISSUE-0040
title: "Optimising the SDHOST data path, against itself"
status: doing
priority: medium
type: investigation
owner: unassigned
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - sdcard
  - sdhost
  - dma
  - performance
  - emu68
blockers:
related_files:
  - external/aros/arch/arm-native/soc/broadcom/2708/sdcard/sdcard_sdhost_bus.c
  - external/aros/arch/arm-native/soc/broadcom/2708/sdcard/sdcard_sdhost_init.c
  - aros/arch/m68k-emu68/soc/sdcard/mmakefile.src
  - patches/aros/0024-sdcard-report-what-the-pio-data-loop-costs.patch
  - AI_context/consolidated/history/ISSUE-0013.md
---

# Summary

The card boots on SDHOST with DMA (ISSUE-0013). Nobody knows what that path
costs, and the target is **SDHOST against itself** — a baseline, then each
change measured against it.

**It is not SDHOST against Arasan.** That comparison is settled and not by
speed: the Arasan has to be free for SDIO or WiFi is impossible on this board.
Whatever the numbers said, the card would still move. So Arasan is not the
control, and dragging it into the measurement would only confuse what is being
asked.

# The constraint that shapes everything else

An A/B against yourself needs an instrument that **does not change between the
two halves**. Instrument the driver and optimise the driver in the same round,
and there is nothing left to compare. So the measurement is deliberately split
in two, with different rules:

| | what it is | may it change? |
|---|---|---|
| **Scoreboard** | `xSysInfo FULL`, an external binary on the card | **No.** Same binary, every variant, QEMU and Pi |
| **Map** | instrumentation inside `sdcard_sdhost_bus.c` | Yes, but must survive every variant so its columns stay comparable |

`xSysInfo` measures the right layer for this: `measure_drive_speed()`
(`src/drives.c:1151`) opens the exec device named by the drive's `handler_name`
and issues `CMD_READ` in chunks capped by `MaxTransfer`, timed with EClock. It
never goes through DOS or the filesystem, so it measures `sdcard.device` and
nothing above it. It is also comparable with the rest of the world — the same
binary reports an A1200 with a CF card, a PiStorm, a Pi 4.

What it does **not** do is decompose. It gives one number, bytes/sec on large
sequential reads. That is why the map exists as well, and why the two are not
interchangeable.

# What is already known about the path

Read from `sdcard_sdhost_bus.c` rather than assumed:

* **Multi-block already works.** `:451-456` computes `nblks` and writes
  `SDHBLC`, and one DMA covers the whole `sdDataLen`. It is not one command per
  block, which removes the first optimisation anyone would reach for.
* **The bounce buffer is 64 KB** (`sdcard_sdhost_init.c:237`), so transfers are
  not falling off it into the error path.
* **Every block is copied anyway**, because the bounce is unconditional by a
  diagnostic left switched on (`:457-461`): *"Diagnostic: always route through
  the bounce buffer so every transfer uses one well-aligned, known-good DMA
  address."* Both directions — `:468` caller→bounce before a write, `:513`
  bounce→caller after a read.
* **The copy is word-at-a-time here.** `sdhost_neon_copy()` moves 64 bytes per
  iteration through `vldm/vstm` under `#if defined(__arm__)`; that is false for
  this target and the `#else` word loop runs.

# The first unknown, and it reorders everything after it

**How large is `sdDataLen` in practice?**

Nobody has looked, and the answer decides which optimisations matter:

* If the caller asks for 512 bytes at a time, the cost is dominated by the
  *per-command* overhead — setup, `SDHBLC`, DMA descriptor, wait, cache
  maintenance — and neither the bounce nor the copy is where the time is. The
  win would be in getting larger requests.
* If it asks for tens of kilobytes, the command amortises and the bounce copy
  becomes the visible term.

This is one histogram and it is the cheapest thing on this page.

# Plan

1. **Baseline.** `xSysInfo FULL` on the current build. This number is frozen as
   the reference; the method must not change while this issue is open.
2. **Map.** One instrument in `sdcard_sdhost_bus.c` reporting per megabyte, in
   four columns, designed to stay switched on across every later variant:
   a histogram of `sdDataLen`; time in `sdhost_dma_wait()`; time in
   `sdhost_neon_copy()`; total time in the data path of `SendCmd`. The
   remainder is command overhead.
3. **Then optimise, in whatever order the map says.** The candidates, with the
   condition each depends on:

   | candidate | worth doing when |
   |---|---|
   | larger requests (fat-handler buffers, or readahead in `sdcard.device`) | the histogram is dominated by small transfers |
   | drop the unconditional bounce | transfers are large, *and* the caller's buffer meets whatever the engine requires |
   | `movem.l` instead of the word loop | the bounce survives step 2 and the copy is a visible share |

4. **Each change re-runs the scoreboard**, on an idle host, per the boot
   measurement discipline.

# Notes on the candidates

**Dropping the bounce is the one that deletes work** rather than accelerating
it. Before touching it: what does the direct path require of the caller's
buffer? `dma_bounce` is aligned to 32 bytes by hand, which is a hint about the
requirement and not an answer, and upstream chose the bounce deliberately.

**`movem.l` is the m68k answer to the NEON block**, not NEON. The guest is
m68k and cannot emit NEON at all, so that block is dead code here rather than a
conflict. `movem.l` moves up to 16 registers in one instruction — 64 bytes, the
same chunk the NEON path uses, 8 instructions per 512-byte block against 128.
Under a JIT the unit of cost is the instruction, which is the same reasoning
that made `CPUSHP` beat a `CPUSHL` line loop by about eleven seconds of boot in
`CacheClearE`.

**Not checked, and it decides whether `movem.l` is worth anything:** how Emu68
translates it. If it breaks into individual loads the advantage evaporates.
Read `external/emu68/src/M68k_LINE4.c`; do not assume.

**NEON inside Emu68 is a separate question with a scar.** If anyone ever
accelerates this copy on the AArch64 side, `external/emu68/CMakeLists.txt:36-39`
reserves `v19`-`v26` for the JIT, and ISSUE-0038's root cause was a clobber of
`x12`/`v28` because GCC ignores `-ffixed` in the prologue. That is about Emu68,
not about the guest-side driver.

# Notes

**Inside the standing freeze** as measurement and as making what exists faster.
The card already boots this way.

**Both backends still build**, switched by one line in
`soc/sdcard/mmakefile.src`. Kept so a regression on one is answerable against
the other without a bisect — not as a performance control.

# Execution log

- 2026-08-17 — **First attempt at the baseline produced nothing.** `xSysInfo`
  v0.9.0's prebuilt binary (118204 bytes, a valid AmigaOS hunk executable) was
  copied to `C:` on a card copy and run from the Startup-Sequence as
  `C:xSysInfo FULL >>SDCARD0P0:bench.log`. The log contains the header written
  before it and **not** the marker written after it, so the sequence did not
  continue past that line as expected — yet the boot still reached
  `BootUI display takeover`.

  Cause not established. Three candidates, none tested: the binary did not load
  (stderr was not redirected, so a shell complaint would have been lost); it
  ran and wrote nothing; or its output never reached the card because the
  emulator was killed before the FAT write was flushed. The last is the
  cheapest to eliminate and the most likely, given the card was mounted
  read-write rather than through `snapshot=on`.
- 2026-08-17 — Opened while verifying the SDHOST port, and rewritten the same
  day once the target was stated as SDHOST against itself rather than against
  Arasan. That reframing is what introduced the requirement for an unchanging
  external scoreboard.
