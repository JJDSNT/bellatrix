---
id: ISSUE-0009
title: "Locate the FAT byte-order defect: filesystem handler or SD card backend"
status: backlog
priority: high
type: research
owner: unassigned
created_at: 2026-08-06
updated_at: 2026-08-06
tags:
  - fat
  - sdcard
  - endianness
  - boot
blockers:
related_files:
  - external/aros/rom/filesys/fat/direntry.c
  - external/aros/rom/filesys/fat/ops.c
  - external/aros/rom/filesys/fat/lock.c
  - external/aros/rom/filesys/fat/fat_fs.h
  - external/aros/rom/devs/sdcard/sdcard_bus.c
  - aros/arch/m68k-emu68/soc/sdcard/sdcard_bcm2708bus.c
  - docs/known-good-baseline.md
---

# Summary

The SD card poisons itself: files AROS writes during boot come back with their
size, first-cluster and date fields byte-swapped, and every subsequent boot
hangs in `OpenDiskFont`. Two different layers could produce that, and the
question has never been settled by measurement — it has only been assumed to be
the FAT handler, and the change made on that assumption turned out to be a
regression.

This issue is to decide **where the byte order is actually lost**: in the FAT
filesystem code, which composes the on-disk structures, or in the SD card
backend, which moves the sector bytes.

# Problem

## What is observed

The font indexes (`arial.font`, `ttcourier.font`, `xen.font`, `stop.font`,
`fixed.font`, `fontcache`) ship in no distribution — they are generated during
boot. After one boot, `mdir -i out/aros/sd.img@@1M ::/Fonts` shows a file whose
real size is 2604 bytes reported as 738852864, and a month field of 15.

`2604` is `0x0000_0A2C`; `738852864` is `0x2C0A_0000`. That is a clean 32-bit
byte reversal of the same value.

Every later boot hangs with nothing further on the serial, inside `OpenDiskFont`
called from `FontPrefs_Handler`. `mdel` on the host hangs in the same way,
walking a 738 MB cluster chain that does not exist.

## Why the obvious repair is not the answer

The drift parked on branch `codex-2026-08-05` as part of
`AI_context/codex-2026-08-05/aros-extras-drift.patch` changed
`rom/filesys/fat/{date,direntry,ops}.c` to convert on both sides. Promoted to
`patches/aros/0007` and built through the normal path it produced **three runs,
three failures**, one stopping as early as `IPrefs`; reverted, on a freshly
generated card, two runs and two Workbench screens. Measured, not reasoned
about — see `docs/known-good-baseline.md`.

So "add `AROS_*2LE` everywhere" is not the fix, and the layer at fault is still
an open question rather than a detail.

## The two candidates

**Candidate A — the FAT handler.** Upstream `rom/filesys/fat` is *asymmetric*:
it converts on read and not on write.

| direction | site | converts? |
|---|---|---|
| read | `fat_fs.h:304` `FIRST_FILE_CLUSTER()` | yes, `AROS_LE2WORD` |
| read | `lock.c:227` `gl->size = AROS_LE2LONG(de.e.entry.file_size)` | yes |
| write | `direntry.c:581-584` `first_cluster_lo/hi`, `file_size` | **no** |
| write | `ops.c:745,882-884,1056-1058` same fields | **no** |
| read | `direntry.c:242-243` raw field compared against a native cluster | **no** |

On a little-endian host every one of those is a no-op, which is why the defect
is invisible upstream. On m68k the read half is right and the write half stores
host order straight into the on-disk structure. That matches the observation
exactly, and it also explains why the both-sides patch regressed: it added
conversion to read sites that already converted, doubling it and breaking every
lookup.

**Candidate B — the SD card backend.** `sdcard_bcm2708bus.c` deliberately
distinguishes the SDHCI data FIFO from the registers: `sdc_is_data_port()` makes
`BCMMMIOReadLong()`/`BCM283xWriteLong()` pass `SDHCI_BUFFER` through unswapped
while every other register is converted. If that distinction were wrong in one
direction only, sectors would come back reversed in 4-byte groups.

Three things weaken this candidate but do not eliminate it:

- The data port is reached only through `IOReadLong`/`IOWriteLong`
  (`rom/devs/sdcard/sdcard_bus.c:835,842`), and both apply the same rule, so the
  path looks symmetric.
- `BCMMMIOReadByte()`/`ReadWord()` do **not** special-case the data port, and
  `BCMMMIOWriteByte()`/`WriteWord()` read-modify-write through the *converting*
  accessor before calling the *non-converting* write. Nothing is known to use
  them at that offset — but nothing checks either.
- A whole-sector 4-byte reversal would scramble the 8.3 name at offset 0 of the
  directory entry, and the names read back correctly.

That last point is the strongest argument for A, and it is exactly the kind of
argument that should be confirmed by looking at bytes rather than reasoned from.

# Goal

A statement of which layer loses the byte order, backed by an inspection of the
actual bytes on the card, and a minimal change that stops the card poisoning
itself without regressing the boot.

# What is left

**1. The discriminating experiment.** Boot to a shell on a freshly generated
card, write a file with known content and a known length (something with
non-symmetric content, e.g. the 16 bytes `00 01 02 ... 0f`), shut down, and on
the host dump both the directory entry and the file's data cluster with
`hexdump`.

| what the dump shows | conclusion |
|---|---|
| name intact, size/cluster/date reversed, **file content intact** | the FAT handler composes the entry in host order — candidate A |
| **file content reversed in 4-byte groups** | the block path swaps user bytes — candidate B, and the FAT symptom is a consequence |
| both reversed | both layers, fix B first and re-measure |

The file content is the decisive part: it passes through the block layer and
through none of the FAT field code.

**2. If it is A** — apply conversion to the *write* sites only, leaving the read
sites exactly as upstream has them, and separately fix the two raw comparisons
at `direntry.c:242-243` (which must compare in the same domain, not in two).
`date.c` needs the same audit for the packed date/time words.

**3. If it is B** — audit `sdc_is_data_port()` against the byte/word accessors,
and check whether anything reaches `SDHCI_BUFFER` through them.

**4. Either way, decide where the change belongs.** A write-side conversion in
`rom/filesys/fat` is target-neutral and upstream-shaped: it is a patch series
entry, and arguably an upstream submission. A backend fix is ours and lives in
`aros/arch/m68k-emu68/soc/sdcard/`.

# Decisions taken

None yet. Recorded so it is not re-decided by assumption: the both-sides
conversion is rejected on measurement, not on review.

# Acceptance criteria

- [ ] A hex dump of a file written by AROS, showing whether the *content* is
      byte-reversed, is recorded in this issue
- [ ] The layer at fault is named, with that dump as the evidence
- [ ] A minimal change is applied through `patches/` or `aros/`, never by
      editing a submodule in place
- [ ] After the change, a boot generates the font indexes and
      `mdir -i out/aros/sd.img@@1M ::/Fonts` reports plausible sizes and dates
- [ ] A **second** boot on the same card — not a regenerated one — reaches the
      Workbench with icons
- [ ] At least 3 serial runs per configuration on an idle machine, per the
      measurement discipline in `CLAUDE.md`

# Notes

**The card contaminates the next run.** With the defect present, one boot is
enough to poison the card, so an A/B comparison whose second half runs on the
first half's card is measuring the first half. **Regenerate the card before
every single run** — `./scripts/make-sdcard.sh` — including between repeats of
the same configuration.

Wanderer takes around eight minutes under QEMU to draw the icons. A run judged
at 200 s shows an empty screen and reads as a failure; see
`docs/known-good-baseline.md`.

# Execution log

- 2026-08-06 — opened. The both-sides FAT change is already measured as a
  regression (3 runs / 3 failures, against 2 runs / 2 desktops reverted); the
  layer at fault has never been established from the bytes.
