# AI_context/consolidated/issue_disk_dsken_vbl_timing.md

# Issue: KS1.3 disk DMA — DSKEN cleared by VBL code mid-DMA

## Status: FIXED — Rigel commit `14ca9ab` (paula: complete KS1.3 disk DMA handoff)

## Root cause

KS1.3 disk DMA requires two back-to-back DMA transfers (DMA1 → DSKBLK1 →
DMA2 → DSKBLK2). Between DSKBLK1 and DMA2, the Kickstart executes normal VBL
interrupt handlers because Rigel fires VBL at the correct hardware rate
(~10+ frames during a real DMA1 transfer).

One of those VBL paths — code at `0xFF4570` — clears DSKEN as part of display
setup. This code never ran at this timing in the legacy chipset emulator because
legacy DMA was instantaneous (0 frames between DMA1 and DMA2).

**Sequence with bug:**

```
DMA1 starts
  → ~10 VBL interrupts fire during DMA1 (hardware-accurate timing)
  → PC=0xFF4570 clears DSKEN, BLTEN, COPEN from DMACON
DMA2 is armed (DSKLEN written) — but DSKEN is now 0
  → slot_scheduler "legacy priority" still grants DMA2 slot (ignores DSKEN=0)
  → DMA2 completes → DSKBLK2 fires
  → DSKBLK2 handler sees DSKEN=0 in DMACONR → skips sector decode/copy
  → io_actual never written → trackdisk driver stalls at START=2 forever
```

The divergence between Rigel and legacy was visible in DMACON traces:

```
Legacy between ACK2 and DMA2:  dmacon stays at 22D0 (DSKEN=1) ✓
Rigel between ACK2 and DMA2:   dmacon drops to 2200 (DSKEN=0) via 0xFF4570 ✗
```

The reason Rigel takes a *different ROM path* during DSKBLK1 handling:
the handler at `0xFC4AF0` does `jmp (A5)` — A5 is the same (`0xFEA372`) in both
cases, but the pre-DSKBLK1 state diverges because Rigel executed VBL handlers
from chip RAM (`PC=0x4B0`) that legacy never reached (VBL timing difference).

## Fix applied (slot_scheduler.c + disk.c)

When "legacy priority" mode grants a disk DMA slot while `DSKEN=0`, re-enable
DSKEN before the grant so the DSKBLK handler sees a consistent state:

```c
if (owner == AGNUS_SLOT_DISK) {
    if (!dmacon_dsken(sched->dmacon)) {
        rigel_dma_domain_write_dmacon(&ctx->chipset.agnus.dma,
                                       DMACON_SETCLR | DMACON_DSKEN);
        sched->dmacon      = rigel_dma_domain_read_dmacon(...);
        sched->table_dirty = true;
    }
}
```

`disk.c` also received a fix to re-fire DSKSYNC on DMA completion to keep
state consistent.

## Key insight for future debugging

If disk DMA stalls at START=2 (DMA1 completes, DMA2 never finishes), check
whether DSKEN is being cleared between DSKBLK1 and DMA2. The culprit is
usually VBL code that ran more times than expected due to hardware-accurate
VBL timing. Compare DMACON traces between Rigel and legacy at the DSKBLK1
ACK point.

## Affected files (Rigel submodule)

- `src/chipset/agnus/timing/slot_scheduler.c`
- `src/chipset/paula/disk.c`
- `src/domains/disk/disk_domain.c`
- `tests/test_agnus_domains.c`
- `tests/test_paula.c`
