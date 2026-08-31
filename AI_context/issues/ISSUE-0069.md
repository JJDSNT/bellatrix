---
id: ISSUE-0069
title: "Refresh AROS to upstream HEAD without losing the m68k-emu68 platform contracts"
status: investigating
priority: high
type: integration
owner: unassigned
created_at: 2026-08-29
updated_at: 2026-08-29
tags:
  - aros
  - upstream
  - aarch64
  - sdcard
  - cache
  - vc4
  - integration
blockers:
related_files:
  - external/aros
  - patches/aros
  - aros/arch/m68k-emu68
  - scripts/setup.sh
  - scripts/build-aros.sh
  - AI_context/consolidated/vc4_memory_coherency_upstream.md
---

# Summary

Bellatrix's AROS pin is behind an upstream HEAD that now contains reference
implementations directly relevant to this port, especially in AArch64 and the
shared Raspberry Pi drivers. Updating is valuable, but a blind submodule bump
would mix platform improvements with ABI changes and with Bellatrix patches
that may already have equivalent fixes upstream.

This issue defines the review and landing contract for that refresh. The goal
is not merely "all patches apply": it is to retain Bellatrix's established
memory, DMA, interrupt, boot, graphics, SD, Bluetooth, Wi-Fi and filesystem
behaviour while adopting upstream implementations where they supersede local
work.

# Baseline examined

The upstream comparison was made after fetching the remote, without moving or
editing the pinned checkout:

```text
Bellatrix AROS pin: fbea2d8b8d6beca257be82583aae1b389909ee7c
AROS origin/master: 1bc141e0d18167f23b1ddedca82fa8bdff2eb656
                    2026-08-29T18:03:56+01:00
```

At that point the full range contains 372 changed files, roughly 26.9K
insertions and 7.3K deletions. It includes large security, Bluetooth, network,
filesystem, Exec and toolchain changes in addition to Raspberry Pi work. This
is too broad to validate as one undifferentiated behavioural change.

The exact candidate commit must be recorded again when the refresh begins;
"HEAD" is a moving target and the hash above is only the researched snapshot.

# High-value upstream work for Bellatrix

## 1. SD card: generic core and BCM backend must move together

This is the most immediately reusable slice. The relevant upstream commits are:

```text
02e77729de sdcard: address MMIO through IPTR rather than ULONG
9356f3d2ad sdcard: report the controller and clock configuration
315dc60e85 sdcard: latch command errors separately from shared IRQ status
8cd421c89f sdcard: fail unit start when CMD7 leaves the card in stand-by
35a88e4b7b sdcard: preserve bus-established flags when media is registered
2222f29ec7 sdcard: bounce buffers the controller cannot address
bd326fe033 sdcard: read the PIO data port directly
4f6e4f50b0 sdcard: do not count aborted chunks as transferred
e98d741e98 sdcard: bulk-read the data port through a bus callback
```

The changes cross two ownership areas:

- `rom/devs/sdcard/sdcard_bus.[ch]` and `sdcard_ioops.c` change the shared bus
  structure and command/data state machine;
- `arch/arm-native/soc/broadcom/2708/sdcard/` implements the optional
  `sdcb_IOReadLongs` callback, uses `IPTR` for MMIO arithmetic and reports the
  actual controller capabilities.

They must not be cherry-picked independently. In particular:

- `sdcb_CmdError` latches errors for the command in flight so a later IRQ
  cannot erase them from `sdcb_BusStatus`;
- listeners and DMA flags are cleared when error recovery resets the
  controller;
- ADMA falls back to the known-contiguous bounce buffer when a source mapping
  is not representable, rather than silently dropping to a large PIO transfer;
- an aborted multi-block operation no longer increments `io_Actual`;
- the bulk PIO callback removes one indirect call per 32-bit data word;
- signal fields become signed so `-1` remains a valid "not allocated" state.

Bellatrix has a project-owned SDHost implementation under
`aros/arch/m68k-emu68/soc/sdcard/`, not the BCM backend verbatim. The refresh
must therefore adopt the generic `rom/devs/sdcard` ABI first and explicitly
adapt the m68k-emu68 backend. Compilation alone will catch the new callback but
will not prove command-error lifetime, DMA fallback or truthful `io_Actual`.

Required SD validation:

- controller/base-clock/capability log identifies the expected QEMU and Pi 3
  controller;
- filesystem boot and repeated reads on QEMU;
- hardware PIO fallback, aligned ADMA and deliberately unaligned/bounced ADMA;
- injected command timeout/error followed by a successful command;
- multi-block abort reports only completed bytes;
- the existing `sdbench`, FAT, SFS and disk stress paths remain clean.

## 2. AArch64 MMU and GPU memory are now a useful policy reference

Upstream commit `16b6c7551c` implements `KrnMapGlobal`/`KrnUnmapGlobal` for the
AArch64 Raspberry Pi port. Its mapping policy is explicit:

- ordinary memory is Normal Write-Back;
- `MAP_WriteThrough` is deliberately interpreted as MAIR `0x44`, **Normal
  Non-Cacheable**;
- `MAP_CacheInhibit`/`MAP_Guarded` mean Device-nGnRnE;
- changing mappings commits page tables with TLB invalidation and barriers.

The new upstream V3D arena code cleans an allocation once, remaps its single
identity view Normal-NC while shared with the GPU, and returns it to cacheable
before `FreeMem`. It does not create cached and uncached aliases of the same
physical pages.

Bellatrix already recorded and implemented the corresponding VC4 decision in
`AI_context/consolidated/vc4_memory_coherency_upstream.md` and Emu68 patch
`0018-map-vc4-memory-normal-non-cacheable.patch`. During the refresh, verify
that upstream changes do not make that patch redundant or change MAIR index
semantics. Do not reintroduce a `vid_base | 0x80000000` mixed-attribute alias.

Also compare the AArch64 `CachePreDMA`/`CachePostDMA` implementation with
Bellatrix's m68k-under-Emu68 cache path. The architectural operations are not
copyable as m68k code, but their ownership contract is: clean before a device
reads, and clean/invalidate after a device writes while preserving dirty edge
line neighbours.

## 3. AArch64 timer: read the hardware clock on demand

Commit `765f360dca` adds `arch/aarch64-raspi/timer/ticks.c`. `EClockUpdate()`
reads the 64-bit free-running system timer safely across low-word wrap and
updates time from elapsed hardware microseconds. `GetSysTime`, `GetUpTime` and
`ReadEClock` no longer inherit the periodic 100 Hz tick's 10 ms resolution.

Bellatrix already has platform timer and boot timestamp work. The refresh must
compare contracts rather than copy names:

- one owner updates the accumulated clock;
- interrupt and on-demand reads cannot count an interval twice;
- the high/low/high read handles the ~71-minute low-word wrap;
- timer.device deadlines and profiling timestamps remain monotonic;
- BootUI and driver timing logs retain useful sub-tick resolution.

## 4. Raspberry Pi boot/platform corrections

Relevant AArch64 HEAD changes include:

- `61fa94f492`: align the relocated kernel physical base to its 2 MiB mapping
  granularity. Bellatrix loads an m68k ELF through Emu68 rather than using this
  native layout, but every place that derives a mapped physical base from the
  VideoCore memory split should be audited for the same invariant.
- `969a1c762e`: derive LED GPIO, polarity, disabled state and ownership from
  the device tree. Bellatrix should not copy fixed Pi-board GPIO numbers where
  firmware/expander ownership applies.
- `3c34721a93`: implement `ShutdownA()` poweroff and reboot through explicit
  AArch64 syscalls and the BCM PM watchdog. The m68k-emu68 port needs an
  Emu68/platform boundary rather than direct reuse, but reset callbacks and
  action semantics should match.

These are references for platform contracts, not automatic candidates for
copying native AArch64 code into the translated m68k port.

## 5. Graphics: distinguish VC4/Pi 3 from V3D/Pi 4

HEAD adds/moves a substantial V3D driver and changes vcgfx/Gallium selection:

- `f83ebdc86f`: vcgfx hands GL to the SoC-specific Gallium driver;
- `983cc6704b`: V3D moves beside the other BCM283x HIDDs;
- `5b3d8d5f41`: VC4 Gallium/V3D build tools use target suffixes;
- vcgfx HVS5 discovery and channel/pixel-valve handling changed.

Bellatrix on Pi 3 remains VC4/V3D 2.1. Do not replace `vc4gallium` with the Pi
4 V3D 4.2 path merely because it is newer. Reuse these parts selectively:

- Gallium driver selection by detected SoC;
- target-suffixed generated artifacts to avoid cross-target contamination;
- HVS/channel discovery fixes that apply to the actual Pi generation;
- the Normal-NC memory ownership model described above.

Validation must retain `GL_RENDERER: VC4 V3D 2.1` on Pi 3, confirm there is no
SoftPipe fallback, and exercise real command submission, completion and
presentation. QEMU cannot validate VC4 cache coherency or rendering.

## 6. Core changes with broad blast radius

The range also contains changes that are not platform-only:

- Exec locks system lists and semaphore operations and adds SMP tests;
- `SumLibrary`/`SetFunction` and removal paths change;
- security.library, usergroup, DOS integration and multiple filesystems are
  extensively rewritten;
- Bluetooth gains large stack/class changes including PAN and serial work;
- BCMGENET and network build selection are added;
- LLVM/toolchain and configured language-standard handling change.

These may be desirable, but they enlarge the validation surface far beyond the
Raspberry Pi fixes. In particular, Bellatrix has local Bluetooth, Wi-Fi,
AROSTCP, DOS boot, filesystem and semaphore investigations. Each overlapping
patch must be classified as:

1. identical upstream — drop the Bellatrix patch;
2. upstream supersedes it — drop and retain only Bellatrix-specific glue;
3. complementary — rebase as a smaller follow-up patch;
4. conflicting behaviour — stop and resolve with an explicit test.

# Refresh procedure

1. Fetch and record the exact candidate upstream commit; do not use an
   unrecorded moving `origin/master`.
2. Create a disposable worktree or branch for the refresh. Keep the current
   working tree and its unrelated local changes untouched.
3. Compute the touched-file intersection between `patches/aros/*.patch` and
   the upstream range before applying anything.
4. Classify every intersecting patch using the four categories above. Preserve
   issue provenance when dropping a patch because upstream absorbed it.
5. Update the submodule pin, then rebuild the patch series in numeric order.
   Do not edit applied patches in place during an iteration; add narrowly
   scoped follow-ups until the refresh series is stable.
6. Run `./scripts/setup.sh --verify` and confirm the submodule tree equals the
   tree derived from the recorded series.
7. Build AROS serially. Start with affected targets (`sdcard`, timer, vcgfx,
   Gallium, Bluetooth/network) before relinking the kernel and regenerating the
   distribution only where contents changed.
8. Validate QEMU and hardware separately using the matrix below.
9. Update each affected AI issue with the upstream commit that replaced or
   changed its local fix; do not leave historical claims pointing at code that
   no longer exists.

# Acceptance matrix

| Area | QEMU | Raspberry Pi 3 hardware |
| --- | --- | --- |
| boot/Exec | desktop/services, no resident or semaphore regression | repeated cold boots and shutdown/reboot |
| SD | boot, filesystem reads, PIO path | PIO, ADMA, bounce, timeout recovery, stress |
| timer | monotonic time and sub-tick reads | wrap-safe long run and timer.device deadlines |
| VC4/vcgfx | driver startup and framebuffer handover | HVS scanout, VC4 GL render/submit/present |
| cache/DMA | structural/log validation only | GPU, SD, USB, audio and framebuffer coherency |
| Bluetooth/Wi-Fi | initialization must not block boot | transport, firmware, association/input tests |
| filesystems/DOS | boot volume, FAT/SFS basic operations | stress, aborted I/O and power-cycle recovery |

# Exit criteria

- the repository pins an exact reviewed AROS commit;
- every Bellatrix AROS patch either applies, was explicitly retired as
  upstream, or has a documented replacement;
- the generic SD core and m68k-emu68 backend agree on the same bus ABI;
- no cache/DMA path relies on comments that disagree with its actual mapping;
- QEMU boot reaches services and hardware passes the platform matrix;
- AI context names the new upstream hashes and removes stale next steps;
- the refresh is split into reviewable commits rather than one pin bump mixed
  with unrelated behavioural fixes.

# Immediate next slice

Start with the SD group because it is cohesive, measurable and already affects
shared code Bellatrix uses. Prototype the generic `rom/devs/sdcard` refresh and
adapt `aros/arch/m68k-emu68/soc/sdcard` to `sdcb_IOReadLongs` plus
`sdcb_CmdError`; do not begin with the full security/Exec range.
