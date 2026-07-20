---
id: ISSUE-0065
title: "KS1.3 label text corruption outside Musashi single-core"
status: doing
priority: critical
type: bug
owner: agent
created_at: 2026-07-17
updated_at: 2026-07-20
tags: [ks13, bitplanes, multicore, emu68, timing]
related_files:
  - AI_context/issues/ISSUE-0070.md
  - AI_context/issues/ISSUE-0064.md
  - AI_context/consolidated/issue_harness_ks13_boot_screen.md
  - src/cpu/cpu_bridge.c
  - src/runtime/core_chipset.c
  - src/machine/machine_rigel_bus.c
  - src/machine/machine_rigel_step.c
---

# Symptom

The KS1.3 insert-disk screen reaches stable display geometry, palette and
artwork, but the text inside the floppy label is composed from corrupted or
misordered glyph fragments. The reference screenshot for this exact symptom
is the local investigation artifact
`ks13_bug(multicore and now emu68sc).jpeg` (not tracked).

Confirmed configuration matrix from hardware observation:

| CPU / topology | Label text |
| --- | --- |
| Musashi single-core | correct |
| Musashi multicore | corrupt |
| Emu68 multicore | corrupt |
| Emu68 single-core after ISSUE-0064 STOP fix | corrupt |

This supersedes the initial classification as either Emu68-specific or purely
multicore-specific. Emu68 single-core now reaches the same execution phase and
reproduces the old multicore visual defect. Crucially, older Emu68 single-core
builds displayed this screen correctly. The current result must therefore be
treated as a regression in shared integration semantics, not as an inherent
Emu68 instruction-generation defect.

# Current localization

The intact hand/floppy geometry makes a general Denise, Copper, palette or
framebuffer failure unlikely. The corruption must be localized in the bitmap
payload or in how the final bitplane words are fetched/composed.

The most useful comparison is therefore Musashi single-core (correct) against
Emu68 single-core (corrupt), with the multicore queue removed from the A/B.

**Caveat on that A/B (recorded 2026-07-20).** It is not as clean as it looks.
Structural work that day made explicit that there is exactly one
implementation of guest memory topology and it is Emu68's
(`bellatrix_emu68_attach_rom_and_ram` / `bellatrix_emu68_map_guest_memory`,
declared in `src/cpu/emu68/bellatrix.h`); the Musashi product build runs on
it. So the two arms of this comparison **share** the guest address-space
layout — chip RAM mirror and ext-ROM probe window included — and differ only
in the CPU core and in which time-advance path runs (`rigel_step_until()` on
Core 2 versus `bellatrix_machine_advance()` from the CPU progress hook,
`src/machine/machine_rigel_step.c:909`). Those two are the remaining
shared-semantics axes; memory topology is not a variable here.

That structural work changed no behaviour (verified statement-by-statement),
so it neither fixed nor could have fixed this defect. Same axis as
ISSUE-0070.
An initial blitter-boundary trace was implemented and run with KS1.3/Emu68
single-core through frame 300. No blitter-register write occurred before or
during construction of this display. This agrees with the older KS1.3 harness
investigation, which observed zero blitter activity in the relevant producer
phase. The diagnostic was removed rather than retained as misleading noise.

This distinguishes the symptom from ISSUE-0024 (KS2.0 text missing because of
`BLTCON0L`). The current KS1.3 label is not evidence of `BBUSY`, `BLTSIZE` or
posted blitter-register ordering.

The payload A/B now uses a dedicated `BELLATRIX_PLANE_DIAG_BUILD` probe. It is
deliberately independent from `BELLATRIX_RIGEL_TRACE_BUILD`: enabling the full
Rigel/JIT trace reproduced the already-documented pinned-register contamination
and trapped Emu68 repeatedly at `$00000680`, so that run was invalid evidence.

Using the exact `src/roms/KS13.rom` supplied for the hardware reproduction, the
minimal probe produced this frame-300 comparison:

| CPU / topology | BPL1 pointer/hash | BPL2 pointer/hash | BPLCON0 |
| --- | --- | --- | --- |
| Musashi 68000 single-core | `$738a` / `8c12de7c` | `$92ca` / `f83bb845` | `$2200` at frame 200 |
| Musashi 68040 single-core | `$738a` / `8c12de7c` | `$92ca` / `f83bb845` | `$0302` |
| Emu68 single-core | `$738a` / `8c12de7c` | `$92ca` / `4eace77c` | `$2302` |

The hashes cover `$2000` bytes from each live pointer. The first plane is
byte-identical across the good Musashi controls and corrupt Emu68 result. The
second plane differs deterministically at frames 200 and 300 in Emu68, while
both Musashi CPU models converge on the same second-plane hash. This is the
first hard localization of the visual defect: the bad pixels already exist in
the BPL2 RAM payload; they are not introduced by Rigel's bitplane fetch or
composition. It also makes CPU model selection alone an insufficient
explanation.

A second pass split BPL2 into 32 blocks of `$100` bytes. Blocks 0--15 and
21--31 are identical between Musashi 68040 and Emu68. Only blocks 16--20 differ,
localizing the payload divergence to guest addresses `$00a2ca..$00a7c9`:

| block | Musashi 68040 | Emu68 |
| --- | --- | --- |
| 16 | `82f3201c` | `099b5fcb` |
| 17 | `a24c94ac` | `00132e0a` |
| 18 | `2652996f` | `75544dfc` |
| 19 | `c4264c84` | `4552cfc6` |
| 20 | `f65919b5` | `357ec2e8` |

The surrounding payload and pointers are identical. The next probe therefore
must target writes to this five-block window, rather than tracing all chip RAM
or video registers.

The direct-map coherence hypothesis was tested and falsified. At frame 200 in
the Emu68 run, FNV over BPL2 through the high backing used by Rigel and through
the low direct CPU alias both returned `4eace77c`. Thus the two mappings observe
the same bytes: this is not a stale cache line, synonym, or chipset-facing view
problem. Guest execution under the current integration semantics produced
different bytes in `$00a2ca..$00a7c9`. Because historical Emu68 single-core was
correct and both CPU backends reproduce the glitch in multicore, this evidence
does not by itself implicate generated JIT stores. The primary comparison is
now current Emu68 single-core versus its last known-good single-core integration,
focusing on CPU/chipset advancement, MMIO flush ordering and interrupt delivery
around construction of this exact BPL2 window.

Commit `0e113d2` (`emu68: restore chipset liveness; fix JIT pinned-register
clobber`) is the first concrete historical baseline candidate: its AI context
records the original working single-core STOP/liveness implementation. The
current tree differs broadly in the CPU bridge, scheduler and both Emu68
patches, so the useful comparison is behavioral at the localized producer, not
a blanket revert or attribution to the public API alone.

The next A/B must localize the first differing BPL2 bytes and their producer:

1. split BPL2 into small blocks and identify the first differing block;
2. dump only that block in the Musashi and Emu68 single-core runs;
3. identify the CPU routine writing that address and compare its registers and
   store widths/values at the first divergence;
4. compare current and last-known-good Emu68 single-core integration semantics
   at that producer before inspecting generated code or changing chipset logic.

Do not use `BELLATRIX_RIGEL_TRACE_BUILD` for this A/B. The dedicated plane probe
runs from the frame callback, outside live pinned JIT context, and preserves the
normal boot path.

## Rejected STOP-PC experiment

The post-STOP interrupt path currently adds 2 to the MainLoop `PC`, while the
generated STOP unit contains an internal add of 4. Changing the MainLoop path to
add 4 based only on the architectural four-byte STOP length was tested and
immediately falsified: KS1.3 corrupted all guest registers and escaped to
`PC=$0000083a` before reaching the screen. At this boundary Emu68's internal PC
already incorporates part of instruction retirement, so the apparently
asymmetric constants are not interchangeable. The experiment was reverted to
`PC += 2`; do not revisit it without first measuring the internal PC convention.

# Acceptance

- KS1.3 label text is correct in all four matrix configurations;
- no regression in KS2.0/Workbench text or other bitplane modes;
- the fix is based on the first differing plane word or a proven identical
  plane payload with divergent fetch/composition, not visual resemblance alone.
