---
id: ISSUE-0051
title: "Opening mesa3dgl20-0.library does not finish, and the I/O is not why"
status: doing
priority: high
type: bug
owner: unassigned
created_at: 2026-08-24
updated_at: 2026-08-23
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

## What these measurements are worth (read first)

Everything timed below was taken under QEMU, and QEMU is not a timing model of
this hardware. Its SD controller is emulated with data coming from a host file
and no real latency, and an MMIO access costs far more there than in silicon --
which penalises exactly the thing being measured, a driver that makes many
accesses per transfer. **Absolute rates here are the emulation's, not the
card's.**

What survives, and what does not:

| credible under QEMU | not credible under QEMU |
|---|---|
| the bounce copy cost **55x** the DMA | 79 ms and 4420 ms as absolutes |
| DMA and burst-PIO drivers perform **the same** | how long either takes |
| **4 direct against 761** rejected for alignment | -- |
| **6844 reads, 4145 hunks, 8 KB per 12-byte request** | -- |
| request size **saturates at 64 KB** | 1.6 MB/s being a ceiling |
| FAT costs **~40%** over device level | whether ~1 MB/s is slow |

So the *structure* of the problem is established here and the *magnitude* is
not. "Do we have a speed problem" needs a Pi. This repository already said so
-- magnitude on hardware, shape under QEMU -- and these measurements were taken
without that caveat attached until now.

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

### Measured on the RDB card

A whole-disk RDB (no MBR) boots this build: `DH0`, DOSType `FAT\2`, mounts as
SYS: and the machine comes up. Sequential read of the same 10 MB file, 64 KB
buffer, cold: **1037 KB/s** -- the same as the MBR card's ~900 KB/s, so the
container costs nothing.

`DH1`, DOSType `SFS\0` in the same RDB, is **not measured**: it is still
unformatted, and `SFSformat DRIVE DH1:` answers `Unknown device DH1:`.

The boot scan does not reject it. With `patches/aros/0039` turned on it says:

    AddPartitionVolume: Partition name: DH1 bootable: 0
    AddPartitionVolume: Found on-disk filesystem 0x53465300
    AddPartitionVolume: AddBootNode(DH1, 0, 0x53465300, NULL)

So the partition is seen, the handler embedded in the RDB's FSHD **is found**,
and a BootNode is added. What does not happen is the node reaching the DOS
device list. That is a much smaller question than the one this started as.

### xSysInfo: numbers, but not the drive test

`xSysInfo BRIEF` runs on this machine:

    CPU: 68040 MHz: 142   MMU: 68040 enabled
    Dhrystones: 56060   MIPS: 31.90   MFLOPS: 20.00
    Chip RAM speed: 3464 MB/s
    Fast RAM speed: 3355 MB/s
    ROM speed: 1.25 MB/s

Useful context rather than a drive number: memory runs at ~3.4 GB/s while the
card reads at ~1 MB/s cold, three orders of magnitude apart.

**The drive benchmark is in `FULL`, and `FULL` crashes** -- the private
`MemHeader` aligned to 4 against `MEMCHUNK_TOTAL` 8, recorded above. So there is
still no xSysInfo drive figure.

Also worth noting from its `DEBUG` output: *"emu68: NO devicetree.resource
found! Assuming real CPU"*. It looks for Emu68's devicetree.resource, which
arrives on the Zorro III board this port does not carry.

#### The amplification is 2x, not 5x, and it is not what costs the time

Two corrections, both to numbers stated earlier in this issue.

**The FAT cache range is 16 KB, and the cache was never 32 KB.**
`cache.c:61` has `RANGE_SHIFT 5`, so a range is 32 sectors. The original
`Cache_CreateCache(glob, 64, 64, ...)` was therefore **64 x 16 KB = 1 MB**, not
the 32 KB `patches/aros/0037` claims in its own commit message. That patch's
premise is wrong; raising the count to 2048 makes it 32 MB, which is why it
changed nothing measurable -- 1 MB was already enough to keep the cache hitting.

**And the amplification is modest.** The device counters report ~20 MB read
while opening a 10 MB file: **2x**, not the five-fold figure quoted from
6844 reads x 8 KB. That arithmetic assumed every read missed; most hit.

Which removes I/O as the explanation for the failure. 20 MB at the device rate
measured here is on the order of **twelve seconds**, and the open does not
return in four hundred. Whatever consumes that time is not the card and not the
filesystem.

**What is left is the loading work itself**: 4145 hunks and roughly four
thousand sections to allocate and relocate, all of it m68k running under a JIT.
That is CPU, not I/O, and it is where the next measurement belongs.

## An upstream defect found while reading the cache

`cache.c` writes through the pointer before testing it:

    b = AllocVec(...);
    b->use_count = 0;
    b->state = BS_EMPTY;
    b->num = 0;
    b->data = ...;
    if (b != NULL)          /* four writes too late */

On a target where a failed allocation faults, this is a clean crash. On this
one it is not: the low page is mapped DIRECT -- 68k vectors and AbsExecBase --
so a NULL `b` would quietly write over the vector table instead. Not implicated
in anything measured here, since these allocations succeed, but worth fixing on
a port that cannot rely on null faulting.

# Next

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
this issue's: it is Exec's, and it is not obviously a defect in the program.
A binary aligning to the target's own `AROS_WORSTALIGN` and being refused by
that target's allocator is an incompatibility, and Bellatrix's answer to an
incompatibility cannot be to recompile the Amiga program. Split out as
[ISSUE-0052](../consolidated/history/ISSUE-0052.md).

# The load finishes. It is not the load.

`patches/aros/0039` now clocks the ELF loader from inside: microseconds spent
reading against microseconds spent relocating, counted per load and reported
once, and only when the load took over a second. Running `OpenMesa
mesa3dgl20-0.library` as the boot test, twice:

    [ELF Loader] 10301 KB in 6844 reads: read 12008 ms, relocate 401 ms over 2612 sections
    [ELF Loader] 10301 KB in 6844 reads: read 11076 ms, relocate 408 ms over 2612 sections

Three things follow, and the third is the one that matters.

**The relocation is not the cost.** It was the standing hypothesis -- 4145
hunks and some four thousand sections, all m68k under a JIT -- and it is wrong
by a factor of thirty. Four hundred milliseconds over 2612 sections.

**The reading is twelve seconds, and that is the whole of the I/O.** It agrees
with what the driver reports for the same run, and it is the entire budget:
10301 KB, which is the file, read once.

**The loader returns.** The line is printed after `load_seg_elf_int` comes
back, so by the time it appears the library is loaded, relocated and in memory
-- and `OpenMesa` still does not return, in four hundred seconds. Everything
this issue has measured so far has been measuring the wrong twelve seconds of
a four-hundred-second wait.

So the question moves, intact, past the loader: what runs after `LoadSeg` and
before `OpenLibrary` returns. That is `LDInit` in `rom/lddemon/lddemon.c`, and
it does two separable things -- it scans every loaded segment two bytes at a
time looking for a romtag, and then it calls `InitResident`, which runs the
library's own init. `patches/aros/0040` clocks those apart.

## What the driver said about the same twelve seconds

    [SDHost00] 1024 cmds, 8020 KB, 4096 KB in 5742 ms = 713 KB/s  [wait 150 ms, cache 34 ms, copy 0 ms]
    [SDHost00]   command total 7162 ms of which transfer 150 ms
    [SDHost00]   direct 502, bounced 7 unaligned + 0 unreachable

The alignment work from `patches/aros/0038` holds: 502 direct against 7
bounced, and the copy time is zero. But of 7162 ms of command time, 150 ms
moved data. The cost is per command, not per byte, at roughly 7 ms a command.

Magnitudes under QEMU are not evidence, but a ratio of 48:1 between command
overhead and payload is shape, and shape is. It says that whatever makes this
card slow will not be fixed by transferring faster, only by issuing fewer
commands -- which is a separate finding from this issue, and belongs to the
driver.


# Past the loader: the romtag scan costs six seconds, and is not the answer

`patches/aros/0040` clocks the two halves of `LDInit` apart, and says the first
half *before* running the second -- deliberately, because a report that waits
for both to finish prints nothing at all when the second one never returns,
which is the case this is here to decide.

    [LDInit] vcgfx.hidd: InitResident 1334 ms
    [LDInit] mesa3dgl20-0.library: romtag found after 5997 ms, 0 segments (0 KB) scanned

`0 segments scanned` means the romtag was found in the very first one -- and
finding it there still took six seconds, because the scan walks that segment
two bytes at a time looking for `RTC_MATCHWORD`. That is a real cost, it is
paid by every large library, and it is not the failure: six seconds against
four hundred.

What it does do is close the last alternative. The open now accounts for:

| stage | cost |
|---|---|
| ELF load (read) | ~11 s |
| ELF load (relocate) | ~0.4 s |
| romtag scan | ~6 s |
| `InitResident` | does not return |

Every measurable stage is seconds. The one that does not finish is
`InitResident`, and for an `RTF_AUTOINIT` library that is `MakeLibrary` and
then a single call into the library's own init vector -- Mesa's own code.

`patches/aros/0041` splits those two, printing `MakeLibrary` time and the
address of the init vector *before* entering it, so that a run which stops
inside Mesa's init says so instead of going silent.


# It is Mesa's own init, and nothing before it

`patches/aros/0041` clocks `MakeLibrary` and then names the init vector
*before* entering it, so a run that stops inside it says so rather than going
silent:

    [InitResident] mesa3dgl20-0.library: MakeLibrary 1 ms, calling init @ 0x04690aac

`MakeLibrary` is one millisecond. The call is entered and does not return.

That closes the account. Opening this library costs, in order:

| stage | cost | measured by |
|---|---|---|
| ELF read | ~11-12 s | `patches/aros/0039` |
| ELF relocate | ~0.4 s | `patches/aros/0039` |
| romtag scan | ~6 s | `patches/aros/0040` |
| `MakeLibrary` | 1 ms | `patches/aros/0041` |
| the library's own init | **does not return** | `patches/aros/0041` |

Everything AROS does for this open is about eighteen seconds. Everything after
that is Mesa's code, running under the JIT, and it is where the four hundred
seconds are.

This is worth stating plainly because three separate explanations have now
been excluded by measurement rather than by argument: the SD driver's transfer
model, the filesystem's I/O amplification, and the loader's relocation work.
None of them was ever going to account for the failure, and two of them were
improved for their own sake while being ruled out.

`MESA3DGLInit` -- the `ADD2INIT` in `workbench/libs/mesa/mesa3dgl_init.c` --
calls `st_gl_api_create()`, which in mesa-20.0.8 is one line returning the
address of a static struct. So it is not that either, and the init vector runs
more than that one function: it runs the whole `INIT` symbol set.

`patches/aros/0042` names each member of that set before entering it. It has
to be linked into the library itself, not into the kernel, so it needs
`mesa3dgl-library` rebuilt and the card remade -- the trap CLAUDE.md records:
if a change is not in the kernel ELF, check where the module on the card came
from.


# It is not slow. It crashes.

Every run so far was killed a few minutes into the init, and the conclusion
drawn from that -- "does not return in four hundred seconds" -- was the wrong
shape of conclusion. Left alone, it does not hang. It dies:

    23:47:23 [InitResident] mesa3dgl20-0.library: MakeLibrary 1 ms, calling init @ 0x04690aac
    23:47:24 [InitResident] posixc.library: MakeLibrary 2 ms, calling init @ 0x044cc452
    23:47:25 [JIT:SYS] open bus read: guest 0xfffffe0e ... m68kPC fffffe0e ret 04a35a58
    23:47:25 [AROS/Emu68] CPU exception vector 0x0000002c at PC 0xfffffe0e

One second from `posixc.library`'s init to the exception. `PC 0xfffffe0e`
with `A6 0x00000000` in the register dump is `jsr -498(a6)` on a null library
base: a library call through a base that was never set.

The chain to it, which the init reports give in full, is
`mesa3dgl20-0.library` -> `gallium.library` -> `gallium.hidd` ->
`cybergraphics.library` -> `stdcio.library` -> `z1.library` ->
`posixc.library`, and the exception is one second into the last of them.

## The control run

The instrumentation in `patches/aros/0039`-`0042` is this session's, and a
diagnostic that creates the defect it reports is worse than no diagnostic. So
the three kernel patches were taken out of the series properly -- cleared
`skip-worktree`, `setup.sh --reset`, verified back to 37 patches -- and the
tree rebuilt without them.

    23:59:04 [openmesa] opening 'mesa3dgl20-0.library'
    23:59:22 [JIT:SYS] open bus read: guest 0xfffffe0e ... ret 044c8da4
    23:59:22 [AROS/Emu68] CPU exception vector 0x0000002c at PC 0xfffffe0e

Same exception, same PC, eighteen seconds in.

**That control was wrong, and the conclusion drawn from it was wrong.** It
removed `0039`-`0041`, which live in the kernel ELF, and left `0042`, which
does not: `0042` patches `compiler/autoinit/functions.c`, which is compiled
into `libautoinit.a` and linked into every *module*. Rebuilding the kernel
does not touch a module that was already linked. The two modules that had
been relinked with it -- `mesa3dgl20-0.library` at 22:45 and
`dos64.library` at 23:50 -- kept it, and they are precisely the two the crash
moved between.

**The crash was ours.** `0042` put an unconditional `bug()` inside
`_set_call_funcs`. `bug()` resolves to the module's own `kprintf`, `kprintf`
loads the module's global `SysBase`, and the generated libinit calls
`_set_call_funcs` before that global has been set -- so `jsr -498(a6)` runs
with `a6` zero. Disassembly puts the faulting instruction inside `kprintf` in
both modules, at exactly the addresses the two runs reported:

| module carrying `0042` | reported `ret` | site |
|---|---|---|
| `dos64.library` | `0x044c8da4` | `kprintf+0x1c` (base `0x044c7740`) |
| `mesa3dgl20-0.library` | `0x04a35a58` | `kprintf+0x1c` (base `0x04690a0c`) |

Removing `0042` and rebuilding `dos64.library` moved the crash from the first
row to the second -- to the one module that still had it. That is the
experiment that decides it, and it decides against the instrumentation.

So "it crashes" is retracted. What the timestamps did establish stands: the
init is entered, and the earlier runs were killed by hand rather than
finishing.

## The clean run, at last

`0042` out of the series, `libautoinit.a` rebuilt, `dos64.library` rebuilt and
`mesa3dgl20-0.library` relinked -- verified by `nm`, no `aisd_now` in either:

    00:54:42 [openmesa] opening 'mesa3dgl20-0.library'
    00:55:05 [InitResident] mesa3dgl20-0.library: MakeLibrary 1 ms, calling init @ 0x04690aac
    00:55:05 [InitResident] gallium.library ... gallium.hidd ... cybergraphics ... stdcio ... z1
    00:55:06 [InitResident] posixc.library: MakeLibrary 1 ms, calling init @ 0x044cc452
    00:55:06 [InitResident] dos64.library: MakeLibrary 0 ms, calling init @ 0x044c774e
    01:41:43 -- nothing further, QEMU still running

**Forty-six minutes inside the init, no crash, no return.** It walks the whole
dependency chain in one second, enters `dos64.library`'s init, and from there
produces nothing at all.

Two things this settles:

- `dos64.library` is right to be there. It initialises and returns cleanly
  now; the only thing killing it was our own `bug()`. The packaging defect it
  exposed was real and is fixed in `build-aros.sh`.
- The original description was right after all, for the wrong reasons. The
  init does not finish. Everything measured around it -- eleven seconds of
  reading, six of romtag scanning, one millisecond of `MakeLibrary` -- is
  noise beside it.

## What the next instrument must not do

`0042` is the cautionary case, and the trap is worth stating because it is not
obvious and it cost this session hours:

**A patch to `compiler/autoinit` is not in the kernel. It is inside every
module already linked.** Rebuilding the ELF does not remove it; each module
has to be relinked. That is why removing the kernel patches and re-running
looked like a clean control and was not.

And the defect itself: `bug()` inside a module resolves to that module's own
`kprintf`, which reads that module's global `SysBase`. In `_set_call_funcs`
that global is not set yet -- even though `SysBase` is right there as a
function parameter. Any instrument placed that early has to use the parameter,
not the global, or not print at all.

The safer instrument for "where inside the init" touches no AROS module: Emu68
owns the CPU and can sample the guest PC. A periodic PC sample would say where
the init is spending forty-six minutes without linking a single line into
anything.

## dos64.library was missing, and was not it

The `OpenLibrary` reporting added to `0040` named one failure outright:

    [LDDemon] OpenLibrary("dos64.library", 50) opened but returned NULL

`posixc.library` and `stdcio.library` both reference `dos64.library` by name,
and it existed nowhere: not in `Libs/`, not as a symbol in the kernel ELF,
only as headers. `rom/dos64/mmakefile.src` builds it as an ordinary
`%build_module modtype=library` under the target `kernel-dos64`; it simply was
never being built here. `make kernel-dos64` produces it in seconds.

With it present the failure line is gone and no `OpenLibrary` fails at all --
**and the crash is unchanged**, same PC, same second. So a real packaging
defect was found and fixed on the way past, and it was not the cause.

## Where the null base is

`jsr -498(a6)` is LVO index 83. In `posixc.library` there are exactly four
such call sites, and disassembly gives the base register's source for each:

| site | base loaded from | index 83 in that library |
|---|---|---|
| `0x000000aa` | `.rodata` | — |
| `0x00004254` (`__entropy_available`) | `SysBase` | `OpenResource` |
| `0x00006870` (`__vfork`) | `DOSBase` | `CreateNewProc` |
| `0x000143b8` (`kprintf`) | `SysBase` | `OpenResource` |

Which of them fired is not yet decided. The return address cannot be mapped
to a site by arithmetic, because the ELF loader places hunks at unrelated
addresses -- taking the reported `ret` against each candidate puts the known
init vector on no symbol at all in every case. That needs the load map, not
more inference.


# Next

1. Find which layer turns a 12-byte read into an 8 KB transfer, and whether
   anything caches it. The FAT sector cache demonstrably does not see these.
2. Try SFS on the card. It is a card-level swap, not a permanent one, and the
   legacy tree did exactly this with ArosOne. If SFS shows the same
   amplification the fault is below the filesystem; if it does not, it is FAT's.
3. Read what `brcm-sdhc.device` does differently. It is PIO with no DMA and no
   block cache at all, and it boots this hardware every day -- so whatever
   makes our path expensive, it is not something that path has to be.
