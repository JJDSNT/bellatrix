---
id: ISSUE-0015
title: "AROS Workbench boot stall"
status: doing
priority: high
type: bug
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-26
tags:
  - aros
  - workbench
  - disk
  - floppy
  - rigel
  - dskchg
related_files:
  - external/rigel/src/core/rigel_cia_api.c
  - external/rigel/src/floppy/floppy_drive.c
  - external/aros/rom/dosboot/bootstrap.c
  - external/aros/arch/m68k-amiga/devs/trackdisk/trackdisk_hw.c
  - src/roms/aros.rom
  - src/disks/aros.adf
---

# Issue: AROS Workbench Boot Stall

## Status: DISK DMA WORKING — VERSION MISMATCH SUSPECTED (2026-06-26)

AROS boots past the boot screen and reaches a gray Workbench backdrop with a
visible mouse pointer, but the Workbench desktop never populates — no disk
icons, no title bar content.

---

## Root Cause History

### Root Cause 1 — Fixed: /DSKCHG not visible when drive deselected

**Problem**: `cia_b_prb_update_floppy()` in Rigel only updated CIA-A PRA bits
2-5 when exactly one drive was selected (`selected_count == 1`). When the drive
was deselected, CIA-A PRA bit 2 reverted to 1 (inactive = no change).

AROS's trackdisk.device reads `/DSKCHG` from CIA-A PRA bit 2 WITHOUT first
selecting the drive (AROS's `td_getDiskChange()` just reads `ciaa->ciapra & 0x04`
directly). On real hardware, `/DSKCHG` is an open-drain signal — it remains
asserted even when the drive is deselected.

**Effect before fix**: AROS's `dosboot_DevicePresent()` issued `TD_CHANGESTATE`,
got io_Actual=1 (no change = "no disk"), returned FALSE, and never issued
`CMD_READ`. DSKLEN was never written in 5000+ frames.

**Fix applied** (in this session):
1. `external/rigel/src/floppy/floppy_drive.c` — `floppy_init()` now sets
   `disk_changed = 0` (not 1) so that phantom drives (DF1-3) never contribute
   to `/DSKCHG` assertion. `floppy_insert()` still sets `disk_changed = 1`.
2. `external/rigel/src/core/rigel_cia_api.c` — `/DSKCHG` (bit 2) updated
   from ALL drives (OR across drives[0..3]) outside the `selected_count == 1`
   guard. Bits 3-5 (WPROT, TRK0, DSKRDY) remain gated by selection.
   ID scan mode (drive selected, motor off) still overrides bit 2 with the
   ID shift register bit.

**Wrong fix attempted and reverted** (same session): changed CIA-A → CIA-B
PRA for the entire status block. AROS trackdisk reads CIA-A, not CIA-B, so
that moved ALL status signals to the wrong register. Regression: AROS boot
screen stall + "DF0 failed to recalibrate" (recal reads /TRK0 from CIA-A
bit 4 — now wrong). Reverted immediately.

**Verification of CIA assignment**: AROS trackdisk reads:
- `tdb->ciaa->ciapra & 0x04` → CIA-A PRA bit 2 = `/DSKCHG` ✓
- `tdb->ciaa->ciapra & 0x08` → CIA-A PRA bit 3 = `/WPROT` ✓
- `tdb->ciaa->ciapra & 0x10` → CIA-A PRA bit 4 = `/TRK0` ✓
- `tdb->ciab->ciaprb` → CIA-B PRB = drive control (STEP, DIR, SELx) ✓

Rigel's `cia_b_prb_update_floppy` correctly updates CIA-A PRA (not CIA-B).

---

## Current State After Fix

With the /DSKCHG fix applied:

1. AROS detects DF0 disk via `TD_CHANGESTATE` → io_Actual=0 → dosboot sees disk
2. trackdisk starts motor, recalibrates, reads the disk
3. Disk DMA works: DSKLEN written, DSKBLK fires, tracks decoded
4. AROS reads boot block (cyl=0 side=0), root block (cyl=40), and extensive
   directory scan (cyl 24-28 area, tracks 47-58, read repeatedly)

Disk read pattern observed at 5784 frames (RIGEL_DISK_TRACE=1):
- cyl=40 side=0 (track 80) — root block
- cyl=24-29, cyl=40-44 — directory and bitmap traversal
- Repeated cycling between tracks 48 and 51 (cyl 24 side 0 / cyl 25 side 1)

**The Workbench still does not load.** AROS reads the disk but does not advance
past InitCode (stuck during afs-handler init at priority -1).

CHIP-DUMP at bitplane addresses (0x29200, 0x29500, 0x24700) shows all 0xFF —
Workbench content was never rendered.

---

## Strongest Remaining Hypothesis: ROM ↔ ADF Version Mismatch

The user notes: "AROS builds change and you need correct pairs between ROM and ADF."

Evidence consistent with mismatch:
- AROS reads the root block and then extensively scans directories (cyl 24-29
  area), cycling repeatedly between the same tracks — classic pattern of
  a hash/BTree lookup finding blocks that don't satisfy a version check
- afs-handler (pri -1) never completes within 5784 frames
- dosboot (pri -50) never runs — we never see "dosboot.resource" being initialized
- The disk has valid FFS structure (boot block checksum OK, disk name "AROS Kickstart")
- Non-OS disks work fine with AROS ROM → disk DMA path is not the issue

The directory scan reads (tracks 48/51 cycling) suggest AROS is trying to
find a specific file version and failing or looping.

**Action required**: Obtain a matched ROM+ADF pair from the same AROS build.
Current files: `src/roms/aros.rom` (1MB) + `src/disks/aros.adf` (880KB).

---

## File Map

| File | Content |
|------|---------|
| `external/rigel/src/core/rigel_cia_api.c` | /DSKCHG fix: OR of all drives, not gated by selection |
| `external/rigel/src/floppy/floppy_drive.c` | floppy_init: disk_changed=0 for phantom drives |
| `external/aros/arch/m68k-amiga/devs/trackdisk/trackdisk_hw.c` | AROS trackdisk HW layer |
| `external/aros/rom/dosboot/bootstrap.c` | dosboot_DevicePresent, dosboot_BootBlock |
| `src/roms/aros.rom` | AROS ROM (1MB, 0xE00000 + 0xF80000) |
| `src/disks/aros.adf` | AROS ADF (880KB, "AROS Kickstart") |

---

## How to Enable Disk Trace

```bash
RIGEL_DISK_TRACE=1 ./build_harness_rigel/harness src/roms/aros.rom \
    --adf src/disks/aros.adf --frames 10000
```

---

## Frame-by-Frame Progression (with fix)

1. Frame 1–40: AROS hardware reset, BPLCON0=0x0200, COLOR00=0x0111 (boot color)
2. Frame 40–1575: Exec init (Enable/Disable loop waiting for VBL). Not infinite.
3. Frame 1575+: VBL fires. AROS renders 672×256 2-bitplane display. Residents load.
4. Frame ~1600: trackdisk/disk.resource init → CIA-B PRB written → /DSKCHG now 0
5. Frame ~1600+: dosboot detects disk → td_motoron → td_recalibrate → disk reads start
6. Frame 1600–5784: afs-handler reads directory repeatedly (cyl 24-40 area)
7. Frame 5784: harness terminated; Workbench not loaded, CHIP-DUMP all 0xFF

---

## What Is NOT the Problem (confirmed)

- Disk DMA path: DSKLEN written, DSKBLK fires, MFM decoded ✓
- CIA/interrupt chain: VBL at IPL 3, PORTS at IPL 2 ✓
- Graphics pipeline: backdrop renders at frame 1575+ ✓
- Sprite/mouse pointer: visible ✓
- CIA-A PRA hardware assignment: /DSKCHG bits 2-5 are on CIA-A (not CIA-B)
- Fake DMA path not used (media=1 confirmed in DISK-START logs)

---

## Open Questions

1. **Version mismatch**: Are `aros.rom` and `aros.adf` from the same build?
   AROS version numbers may be extractable from ROM header or from disk files.
   `BootBlockCheck()` in dosboot checks FileSystem.resource for dostype 0x444F5300.
   An incompatible ADF could cause dosboot to reject the disk silently.

2. **afs-handler loop**: Why does afs-handler cycle between tracks 48/51 repeatedly?
   Could be: B-tree traversal (OFS block chaining), version check loop, or a
   genuine version mismatch causing a retry that never succeeds.

3. **dosboot.resource never reached**: At 5784 frames, dosboot (pri -50) has
   never been called. This means afs-handler (pri -1) has not returned.
   If afs-handler blocks indefinitely (e.g., seeking a file that doesn't exist
   in this ADF version), dosboot never runs.
