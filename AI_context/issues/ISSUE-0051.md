---
id: ISSUE-0051
title: "Reading a 10 MB file costs more than reading it"
status: doing
priority: high
type: bug
owner: unassigned
created_at: 2026-08-24
updated_at: 2026-08-24
tags:
  - sdcard
  - sdhost
  - filesystem
  - performance
blockers:
related_files:
  - aros/arch/m68k-emu68/soc/sdcard/sdcard_sdhost_bus.c
  - aros/arch/m68k-emu68/soc/sdcard/sdcard_sdhost_init.c
  - external/aros/rom/filesys/fat/volume.c
  - external/aros/rom/dos/internalloadseg_elf.c
  - AI_context/issues/ISSUE-0045.md
---

# Summary

Split out of ISSUE-0045, which turned out not to be about GL at all.
`OpenLibrary("mesa3dgl20-0.library")` -- a 10 MB disk library -- does not
return, while a 574 KB one opens promptly. The minimal reproducer is
`C:OpenMesa`, one program that calls `OpenLibrary` and says what came back.

## The card is not the problem

Counters were added to our own SDHOST driver (`sdhost_stat_tick`, one line per
512 commands). During the "hang" the card keeps working, steadily:

    [SDHost00]  512 commands,  7848 blocks (3924 KB)
    [SDHost00] 1024 commands, 16040 blocks (8020 KB)
    [SDHost00] 1536 commands, 24232 blocks (12116 KB)
    [SDHost00] 2048 commands, 32424 blocks (16212 KB)
    [SDHost00] 2560 commands, 40616 blocks (20308 KB)

**20 MB read and climbing, for a 10 MB file.** Every wait loop in the driver is
bounded, and none of its failure paths print. The driver is neither stuck nor
slow: something above it asks for the same data over and over.

DMA is on and always was. The driver requires it -- no channel means
`goto sdhost_fail` -- and it now says so once per boot rather than behind
`D()`:

    [SDHost] DMA channel 8, bounce 0208e240 (64 KB)

That line exists because the question was answered wrong twice from reading
source. `dma.resource`'s own probe prints `allocated channel 9` then
`released channel 9`, which is the resource testing itself and reads like the
card failing to hold one.

## Where the amplification is not

**It is not the FAT sector cache.** That cache was 64 blocks of 512 bytes --
32 KB, on a machine with 840 MB -- which looked like the whole answer.
Raising it to 2048 blocks (1 MB, `patches/aros/0037`) changes **nothing**: the
counters are byte-identical, command for command and block for block, at every
report. The object was rebuilt and the value is in the tree, so the reads do
not go through that cache at all.

**It is not the cluster size.** The card is formatted at 4 sectors per cluster
= 2 KB. The driver moves 16 blocks -- 8 KB -- per command, so a command is four
clusters, which is read-ahead rather than cluster granularity.

## What is known about the shape

From the ELF loader's own trace (since removed): opening this library makes
**6844 `elf_read_block` calls and 4145 hunks**, most of the reads a few dozen
bytes, because Mesa is built with `-ffunction-sections` and the library carries
thousands of sections. 6844 reads against 8 KB per command is on the order of
55 MB, which is the amplification, and it is consistent with what the counters
report.

# Next

1. Find which layer turns a 12-byte read into an 8 KB transfer, and whether
   anything caches it. The FAT sector cache demonstrably does not see these.
2. Try SFS on the card. It is a card-level swap, not a permanent one, and the
   legacy tree did exactly this with ArosOne. If SFS shows the same
   amplification the fault is below the filesystem; if it does not, it is FAT's.
3. Read what `brcm-sdhc.device` does differently. It is PIO with no DMA and no
   block cache at all, and it boots this hardware every day -- so whatever
   makes our path expensive, it is not something that path has to be.
