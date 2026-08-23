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

## Where the driver's own time went, and what fixed it

Counters were gated behind a compile-time switch after the fact
(`SDHOST_STATS`, default off) because sdcard.md sec.25 is right: a hot path
that is logging is not the hot path. The numbers below were taken with it on.

Splitting a data command into wait, cache maintenance and bounce copy:

    4096 KB in 25944 ms = 157 KB/s   [wait 79 ms, cache 41 ms, copy 4420 ms]

**The DMA engine costs 79 ms to move 4 MB. The bounce copy costs 4420 ms** --
fifty-five times as much -- because `sdhost_neon_copy` has no NEON on m68k and
its word loop is JITted, 2048 iterations per 8 KB.

Every transfer went through that copy, and the code said why: *"Diagnostic:
always route through the bounce buffer"*. A diagnostic, left on.

Direct DMA into the caller's buffer needs two things, both of them the
bounce's own reasons for existing: 32-byte alignment, because the cache
invalidate after a read works on whole lines; and reachability, because the
engine addresses the low 1 GB. Counting which failed:

    direct 4, bounced 761 unaligned + 0 unreachable

Alignment, every time. The FAT cache puts its data immediately after a header
inside one `AllocVec`, so the alignment is whatever `sizeof(struct BlockRange)`
leaves. `patches/aros/0038` gives each buffer 31 bytes of slack and rounds up:

    direct 761, bounced 4 unaligned + 0 unreachable
    4096 KB in 10614 ms = 385 KB/s   [wait 32 ms, cache 27 ms, copy 0 ms]

The copy is gone. Boot to icons, hot path silent, three runs: 01:10 / 00:49.6 /
00:50.0 against a 00:57.7 baseline -- but two variables changed between those
measurements, so that is not claimed as a proven gain.

**385 KB/s is still slow**, and the driver now accounts for 59 ms of every
10.6 seconds. The rest is above it.

## SFS: the card is right now, and it still does not mount

`BELLATRIX_SFS=1` builds it end to end, reproducibly:

- second MBR partition of **type 0x30**, which
  `rom/partition/partitionrdb.c:198` accepts as an RDB container (0x2f, which
  `partition_types.c:33` maps to DOSType `SFS\0`, produces no device node at
  all -- that was the first attempt);
- an **RDB** written into it by `rdbtool`, from `external/amitools`, now a
  submodule rather than borrowed from the legacy tree;
- one partition `DH0` with DOSType `0x53465300` = `SFS\0`;
- **the handler embedded in the RDB**, in FSHD/LSEG. The legacy tree's working
  SFS disk carried its driver that way, and the reason is that a DOSType says
  what a volume is, not how to serve it. 24 RDB cylinders, because the default
  reserves 32 blocks and `L:sfs-handler` is 135 KB.

`rdbtool info` confirms all of it:

    LogicalDisk:   24  8127  259328  126Mi  rdb_blks=[0:767,#768]
    Partition: #0 'DH0'  100.00%  SFS0/0x53465300  auto
    FileSystem #0 SFS0/0x53465300 version=1.86 size=135328

And the booted system still lists only `SDCARD0P0:`. No device node for the
second partition.

Excluded so far:

- **the cylinder-boundary check** in `rom/dosboot/bootscan.c:206`, which skips a
  sub-table silently when its start block is not a multiple of blocks per
  cylinder. It cannot be this: `sdcard_bus.c` reports `sdcu_Heads = 1` and
  `DE_BLKSPERTRACK = 1`, so blocks-per-cylinder is 1 and any start passes.
- **the RDB itself**, which rdbtool reads back correctly.
- **a missing handler**, now embedded.

### What the legacy disk actually was

`~/bellatrix-legacy/src/disks/ArosOne-Lite.hdf` begins with `RDSK` at offset 0.
It is a **whole-disk RDB image with no MBR at all** -- one partition `UDH0`,
`SFS0`, 498 MB, bootable -- used in a harness where the HDF *is* the disk.

So the legacy tree did not do two partitions, and nested RDB-in-MBR is not what
it proved. That is not a reason to abandon the nested approach, since the Pi
firmware needs a FAT partition to boot from and the SFS volume here is a work
volume beside it. It is a reason to stop treating it as a known-good recipe.

Two numbers from that disk are worth keeping: `max_transfer=0x1fe00` and
`mask=0x7ffffffe`, with `fs_block_size=1024` and two sectors per block.

### MaxTransfer and Mask are not the amplification

sdcard.md sec.22 asks whether restrictive transfer constraints are making the
filesystem split requests. They are not: `rom/devs/sdcard/sdcard_bus.c:494`
declares `DE_MAXTRANSFER = 0x00200000` -- 2 MB -- and `DE_MASK = 0x7FFFFFFE`.
Both are more generous than the working ArosOne disk's `0x1fe00`.

### QEMU does not need the FAT partition at all

`run.sh` passes `-kernel Emu68.img -dtb ... -initrd aros-emu68-m68k.elf`. **The
Pi firmware never runs under QEMU**, so nothing reads `config.txt` and nothing
requires a FAT partition. The card is attached purely as a block device for
AROS, and Emu68 gets its m68k ELF from `-initrd`.

That means the legacy shape is available here: a whole-disk RDB, no MBR. It
also means the card could carry two RDB partitions -- one for SYS: and one for
SFS -- which is the cleanest way to fill rows B and D of the matrix.

Built and verified with rdbtool, though not yet booted:

    Partition: #0 'DH0'   50.00%  FAT2/0x46415402
    Partition: #1 'DH1'   50.00%  SFS0/0x53465300
    FileSystem #0 SFS0/0x53465300 version=1.86 size=135328

DH0 takes DOSType `FAT\2` so `mformat`/`mcopy` can write it from the host and
this port's own `fat_handler` can mount it. What stopped the attempt was
populating DH0: `mcopy` into a partition at an offset flattened the tree, and
chasing that was not worth more of the session.

### The other shape, from Emu68 itself

Emu68's own SD layout is reported to be **FAT with a nested RDB**, which is
exactly what was tried first (MBR type 0x30 + RDB) and did not mount. That
makes the nested case more likely to be workable than the survey of legacy
disks suggested -- none of those had an MBR at all -- and it moves suspicion to
the RDB's own geometry, since rdbtool wrote it as though it were a standalone
disk rather than a region inside a partition.

### Next

Two experiments, either of which closes this:

1. Instrument downward, the method that has worked everywhere else this
   session and has not been pointed here: does `PartitionMBROpenPartitionTable`
   make a handle for the type 0x30 entry, and does `bootscan.c` call
   `OpenPartitionTable` on it?
2. Compare our nested RDB against a real Emu68 card's, field by field. If the
   difference is geometry expressed relative to the wrong origin, it will show
   there immediately. Whether `PartitionMBROpenPartitionTable` produces a
handle for the 0x30 partition at all, and whether `bootscan.c` calls
`OpenPartitionTable` on it, is two `bug()` calls away.

## SFS: the earlier attempt, superseded

sdcard.md's 2x2 matrix needs SFS on the same card. `BELLATRIX_SFS=1` in
`make-sdcard.sh` now lays down a second partition, and `kernel-fs-sfs` is
built so `L:sfs-handler` ships.

It does not mount. Type 0x2f is what `partition_types.c:33` maps to DOSType
`SFS `, which is why it was chosen, and booted with it the system lists only
`SDCARD0P0:` -- no device node for the second partition at all.

The supported path is **an RDB nested inside the MBR**:
`rom/partition/partitionrdb.c:198` looks for a RigidDiskBlock only inside an
MBR partition of type **0x30 or 0x76**, and the DOSType then comes from the
RDB's partition blocks. Writing that RDB is what remains.

## Against the plan

sdcard.md sec.29 says do not begin by optimizing the driver, and to produce the
four-configuration baseline first. That is not what happened here: the
alignment fix and the direct-DMA path were made before any baseline existed,
from instrumentation of a single configuration. They are measured and they
stand, but they are Bellatrix+FAT only, and the matrix they should have been
judged against does not exist yet.

## Two drivers, measured: they perform the same

`SDCARD_BACKEND := emu68sd` selects a second driver, ours, built on Emu68's
model rather than on arm-native's: **no DMA channel, no bounce buffer, no cache
maintenance**, and the FIFO drained by the CPU in bursts of eight words with
the level read once per burst. It announces itself:

    [Emu68SD] PIO backend, no DMA channel and no bounce buffer

Boot to icons, three runs each, alternating, hot path silent:

| | run 1 | run 2 | run 3 |
|---|---|---|---|
| A -- ours, DMA direct | 01:06.4 | 00:45.4 | 00:43.8 |
| B -- Emu68 model, PIO burst | 00:45.3 | 00:48.4 | 00:47.5 |

Discounting each set's first run -- both are the boot straight after writing
the image, and both are the outlier -- **the two are the same, within noise**,
with ours perhaps marginally ahead.

That is worth more than a win would have been. It says the transfer model is
not where this port loses time: a driver with no DMA, no bounce and no cache
work performs like one with all three, on the same controller and the same
card. Whatever the remaining ceiling is, both drivers hit it.

It also retires the hypothesis this comparison was built to test. Emu68's
driver was fast and ours was slow, so the model looked like the difference. It
was not: the difference was the bounce copy that unaligned buffers were forcing
(`patches/aros/0038`), and once that was gone the models became
indistinguishable.

Kept as a real second backend rather than deleted. sdcard.md's matrix wants two
drivers, and now there are two.

## Sequential read, buffer swept -- and the first answer was wrong

`C:SDBench` (`aros/arch/m68k-emu68/c-posixc/SDBench.c`) reads one file end to
end with a chosen buffer and reports bytes, ticks and rate. The file is
`mesa3dgl20-0.library`: already on the card, 10 MB, and ten times the FAT
cache, so a pass cannot be held in it.

Ascending sweep, driver A:

    4 KB    12.38 s     824 KB/s
    16 KB    1.08 s    9454 KB/s
    64 KB    0.82 s   12452 KB/s
    256 KB   0.92 s   11098 KB/s

That reads as sdcard.md sec.7's "fails to benefit from larger requests", and
it was reported that way for about a minute. It is not. Descending, then the
smallest again:

    256 KB  10.92 s     935 KB/s     <- cold
    64 KB    1.14 s    8957 KB/s
    16 KB    0.96 s   10636 KB/s
    4 KB     2.16 s    4727 KB/s
    64 KB    0.88 s   11603 KB/s
    4 KB     2.18 s    4683 KB/s

**The first pass is slow whatever its size.** 256 KB cold gives 935 KB/s, just
as 4 KB cold gave 824. That is sec.13, cold versus warm, not sec.7 -- and
putting the sweep in one order was enough to confuse the two.

What survives, from the warm passes, which repeat:

- buffer size does matter, but modestly: ~4700 KB/s at 4 KB against
  ~9000-11600 KB/s at 16-64 KB, and both repeat within a run.
- 256 KB is no better than 64 KB.

And the number that matters for this issue: **cold sequential read is about
900 KB/s**, against the 385 KB/s measured while opening the same file through
`OpenLibrary`. Same file, same driver, same card. The gap is the amplification,
not the medium.

## xSysInfo: it does run from a script, and it crashes

**Correction.** It was reported here as GUI-only with no command line, from a
`strings` grep that found no TEMPLATE. The repository says otherwise in two
lines -- `src/main.c:81` is
`#define TEMPLATE "DEBUG/S,BRIEF/S,FULL/S,DARK/S"` and the README says *"Run
`xSysInfo FULL` to write the full report format to the CLI"*. It is scriptable.

`tests/sysinfo/` now stages binaries into `C:` and `S:sysinfo` runs
`xSysInfo FULL >DEBUG:`. The program starts and then dies:

    Error: 0x8100000C - Sanity check on memory list failed
    Task : 0x04233ABC - xSysInfo
    Module xSysInfo Segment 0 - offset 0x000175CE
    In Allocate, size 16
    MemHeader 0x045ee534 (0x045ee554 - 0x045f2534)
    - Unaligned first chunk address (0x045ee554)

xSysInfo builds a private `MemHeader` and AROS refuses it.
`rom/exec/memory.c:85` tests `(IPTR)mh_First & (MEMCHUNK_TOTAL - 1)`, and
`MEMCHUNK_TOTAL` is **8** on m68k while the program aligned its pool to **4**
(`0x...554`).

**That is the same seam for the third time in this investigation.** m68k has
`AROS_WORSTALIGN` 4 and an 8-byte `MemChunk`, so "aligned" means two different
things depending on who is asking:

1. TLSF gave out 4-byte blocks that could not hold an 8-byte free node
   (ISSUE-0037);
2. the FAT cache aligned its buffers to whatever a header left, and the SD
   driver needs 32 (`patches/aros/0038`);
3. xSysInfo aligns a pool to 4 and `Allocate` wants 8.

No number from xSysInfo until that is resolved, and the resolution is not
obviously ours: a program aligning to the target's own `AROS_WORSTALIGN` and
being rejected is at least arguable.

# Next

1. Find which layer turns a 12-byte read into an 8 KB transfer, and whether
   anything caches it. The FAT sector cache demonstrably does not see these.
2. Try SFS on the card. It is a card-level swap, not a permanent one, and the
   legacy tree did exactly this with ArosOne. If SFS shows the same
   amplification the fault is below the filesystem; if it does not, it is FAT's.
3. Read what `brcm-sdhc.device` does differently. It is PIO with no DMA and no
   block cache at all, and it boots this hardware every day -- so whatever
   makes our path expensive, it is not something that path has to be.
