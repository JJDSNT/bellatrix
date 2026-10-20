---
id: ISSUE-0088
title: "Refresh AROS from f7b538f to master in nine milestones, not one bump"
status: ready
priority: high
type: infra
owner: unassigned
created_at: 2026-10-20
updated_at: 2026-10-20
tags:
  - aros
  - upstream
  - submodules
  - aarch64
  - m68k
blockers:
  - "M0: no like-for-like boot of the current pin exists (ISSUE-0085, 'What the boot established')"
related_files:
  - external/aros
  - patches/aros
  - aros/arch/m68k-emu68
  - aros/arch/arm-native
  - AI_context/issues/ISSUE-0058.md
  - AI_context/issues/ISSUE-0069.md
  - AI_context/issues/ISSUE-0085.md
  - AI_context/consolidated/history/ISSUE-0087.md
---

# Summary

This is the execution plan for the refresh whose contract is
[ISSUE-0069](ISSUE-0069.md). The 0069 contract still stands: its four-way
patch classification, its acceptance matrix and its exit criteria. Its
snapshot hashes and its "immediate next slice" do not. The SD group it names
was taken in [ISSUE-0085](ISSUE-0085.md).

Snapshot, fetched 2026-10-20:

```text
Bellatrix AROS pin : f7b538f865  2026-08-31  (same on main and bluetooth-aarch64-port)
AROS origin/master : a399fe2fc7  2026-10-19
range              : 1243 commits, linear (first-parent == all)
pace               : ~100/week in September, ~270/week in October
```

ISSUE-0085 records the pin as `3fb8adfd3a`. The merge `ef88b5f` moved it
further the same day to `f7b538f`. `f7b538f` is the real starting point.

Each milestone is a separate pin bump, built, booted and committed before the
next one starts. That is "one pack, one variable" applied to upstream.

# Relation to the audit in `AI_context/specs/aros_upstream_head_update.md`

That spec (2026-10-09, upstream `3914027415`, 814 commits) is the detailed
audit, and its acceptance checklist is the definition of done for the whole
refresh. This issue is how it gets executed: the same reconciliation, cut
into milestones that are each built, booted and committed. The spec allows
that ("stages may be validated separately"). This file and the spec were
written without knowing about each other; they were reconciled on
2026-10-20, and where they differ, the spec's findings win.

What the spec adds, placed by milestone:

| M | From the spec |
|---|---|
| M2 | Patches `0061`-`0067`, `0070` and `0071` edit the **Mesa 20.0.8** patch file. Mesa 26 becomes the default here, so they still apply but no longer reach the active build. Audit each against Mesa 26 and keep `0036` (m68k trampolines in `galliumglue.py`, which upstream still limits to arm/aarch64), `0069` and `0079`. Check `7756f31db8` (64-bit atomics) against `0057`. |
| M5 | **Build blocker:** `38dc95623a` removes `iet_private1/2`, `iet_LastBusy` and `iet_LastUsageStamp` from `IntETask` on m68k, and our `kernel/cpuusage.c` reads and writes `iet_private2` and `iet_LastBusy` (lines 147-218). Keep the accounting: put the fields back under a port capability, or replace them with an explicit design. Keep `0093`/`0094` consistent with that. Do not delete the accounting to make the build pass. |
| M5 | usb2otg: the SOF wake (`2b1739fe22`) uses system timer channel 1, while our heartbeat is on channel 3. Check IRQ registration, ownership and acknowledgement before taking it. |
| M6 | `0037` is replaced by upstream's volume-scaled cache (`efbf24238c`), then measured. `0038` is refreshed, not retired: HEAD still does not align the data, and the refresh must put the alignment slack inside the `b != NULL` branch. Do not take `b76276f7dc` (2 KiB system stacks) without a measurement, and do not undo `0005`. |
| M6/M7 | `0057` (m68k test-and-set returns the old value) stays: HEAD's version is still wrong. `0058` is reviewed against the new allocator, not dropped by title. |
| M7 | `0006`, `0008` and `0023` retire, confirmed site by site in the spec; keep `9213952b4f`'s explicit-width dates. `ba01955a0b` also changes `ExitIntr()`'s calling convention and routes dispatch through `ex_LaunchPoint`, so every local producer and consumer of the reschedule flags is checked, not only `dispatch.S`. |
| M3 | `ec982ac781` (`hdmiaudio`) and `3ecd43bdfa` (`dma.resource`) are ported together. Also check `8397060fe5` (cache-line-aligned mailbox messages). |

**Deviation from the spec's workflow, recorded.** The spec, step 1, says to
use an isolated checkout or worktree and not to reset a working development
tree. M1 was done in place in the main checkout, with the user's
authorisation, after `setup.sh --verify` had shown the submodule was exactly
pin plus series, with no local edits. From M2 on, use a worktree as the spec
says, because the reconciliations there leave the tree unbuildable for a
while.

# Three ways upstream reaches this port

A pin bump only delivers the first. The plan has to name all three, because
the third is invisible to `setup.sh --verify` and to a clean build.

1. **Compiled from upstream in place.** These arrive with the bump and need
   a boot, not a port: `arch/m68k-amiga/hidd/amigavideo` (20 commits in
   range), `arch/arm-native/.../2708/dma`, `rom/*`, and every `m68k-all`
   file we do not override.
2. **Patches.** `patches/aros/` has 75 numbered patches. The series was
   simulated at every cut point in this plan. It applies 75/75 at the pin
   and 59 at master. Five patches went upstream, eleven conflict.
3. **Forks and overrides.** Nothing arrives here; each change is read and
   ported by hand. There are 65 files in `arch/m68k-emu68` that shadow a
   same-named upstream file; 12 of the originals changed in range. On top of
   those, seven driver forks are kept as copies: `soc/sdcard`,
   `soc/usb/usb2otg`, `soc/wifi/{bwfm,sdio}`, `soc/bluetooth`,
   `hidd/vcgfx`, `hidd/vc4gallium`.

The header shadow from ISSUE-0058 §1 belongs to both 1 and 3:
`arch/arm-native/.../2708/dma/dma_init.c` is compiled in place against
**our** `hardware/bcm2708.h`. Upstream has already drifted from it. It
renamed `BCM2711_GPUIRQ_OFFSET` to `BCM271X_GPUIRQ_OFFSET`, guarded
`DMA0_BASE` for BCM2712, and changed the `CLID_I2C_BCM2708` block. The DMA
change lands in M2 (`3ecd43bdfa`), so M2 has to re-sync the header or it
will not compile.

# What the bump will not bring: `scripts/upstream-forks.py`

The hand-made inventory of the third path above missed six forks:
`hdmiaudio`, `battclock`, `arasan.h`, `videocore.h`, `mbox_init.c` and
`sdcard_bcm2708init.c`. So the inventory is now a script. It pairs every
file under `aros/` with an upstream file of the same name. A pair is kept
when the contents still resemble each other (difflib ≥ 0.35), or always
when ours replaces an `m68k-all`, `m68k-amiga` or `rom` file by path. The
build swaps those by name, however far they have drifted. Pairing runs at
both ends of the range, so an origin that only appears inside it is found
too; the Bluetooth transport upstream adopted from us is one. The script
then lists the upstream commits that touched each origin.

```bash
scripts/upstream-forks.py --base <previous milestone> --to <this milestone>
```

At 2026-10-20 there are 120 files of ours with an upstream origin, and 60 of
them have changes in `f7b538f..a399fe2fc7`. Run the script at every
milestone. Each commit it lists is ported, or rejected in this file with a
reason, before the milestone counts as done. That is the guarantee that
nothing relevant upstream is lost between our pin and HEAD.

What it reports per milestone (`→` our directory):

| M | Upstream commits to our forks |
|---|---|
| M1 | `20091ece37` GPCLK2/GPFSEL → `include/hardware` (**done**, re-synced); `17da3e3db8` is our transport arriving upstream → `soc/bluetooth`, which diverges on purpose (ISSUE-0087) |
| M2 | `a3cfa659ed`, `622d07129a` Mesa 26 glue → `hidd/vc4gallium`; `3ecd43bdfa`, `72311ea24c`, `f3b47ce8e1` → `include/hardware`; RP5 only → `soc/wifi`, `soc/sdcard`, `hidd/vcgfx` |
| M3 | **`ec982ac781` RPiHDMI with the new `dma.resource` → `hdmiaudio`, all 8 files** (the audio path ISSUE-0055 confirmed) |
| M4 | `642046b4a1` → `soc/usb`; `b6c3d80b2b` → `timer/ticks.c`; `95f5d15b77` → `kernel/getsystemattr.c` (SMP, likely a reject) |
| M5 | usb2otg batch of 19/09 (6 commits) → `soc/usb`; `8a8d180631` → `timer/ticks.c` |
| M6 | vc4gallium of 27/09 (6) → `hidd/vc4gallium`; **`28674a157b` → `battclock`**; `ca064a18a5` → `timer/timer_init.c`; `2fce860abc`, `eb67886d75` → `include/hardware`; `cf91862a8d` → `soc/wifi`; vcgfx BCM2712 (12) → `hidd/vcgfx` |
| M7 | **`ba01955a0b` → `exec/dispatch.S`**; `73eba4ee38`, `ee2e5812c1` → `soc/bluetooth` |
| M8 | `bd0d438f7d` → `soc/wifi`; `fd3ad20c3a` → `soc/usb`; `71623f789e` → `soc/sdcard`; doc-only `595121c85c`, `75659fb32b` |
| M9 | **`4ac3fbc085` → `kernel/kernel_cpu.c`**; `6042b4a8ea` → `soc/sdcard` (the write wait is done, see M0) |

# Standing rules every milestone follows

These come from the notes and each was paid for once.

- **Bump procedure:** ISSUE-0085 "Doing it". Clear `skip-worktree`
  *before* any patch is deleted (CLAUDE.md), then reset, check out, re-apply,
  `--verify`.
- **Redundancy check:** ISSUE-0058's method. Replay the series as commits
  and `git rebase --empty=drop --onto <milestone>`. Do not judge patches one
  by one against a pristine tree.
- **Header diff:** `diff -u` arm-native's `bcm2708.h` against ours on every
  bump (ISSUE-0058 §1).
- **ccache:** when a milestone adds a file to the injected tree, build once
  with `CCACHE_RECACHE=1` (ISSUE-0085, *The cause was a stale object*).
- **Configure:** the digest check in `build-aros.sh` forces the reconfigure.
  If a build dies on an unsubstituted `@variable@`, remove `mmake.config`.
- **Build order:** lean build as the smoke test, then `full` before
  `make-sdcard.sh`. A card made from a stale distribution tree tests the old
  modules (ISSUE-0058 §7). Regenerate the card rather than reuse it, because
  the card poisons itself (memory: card self-poisoning).
- **Boot:** one QEMU run is a smoke test. A series is needed only if
  behaviour changed. No other QEMU running.
- **Records:** every issue whose local fix is replaced gets the upstream hash
  (ISSUE-0069 step 9).

# M0 — before the pin moves

1. **Like-for-like baseline at `f7b538f`.** ISSUE-0085 left the desktop
   unreached and unattributed. It stops after `png.datatype` and parks in
   `emu68_DispatchFrame`, and "a boot of the pin bump alone" was never run.
   Every later milestone is judged against this boot, so it must exist:
   current pin, current series, `full` build, fresh card, QEMU. Record how
   far it gets.
2. **Close the Bluetooth branch.** `bluetooth-aarch64-port` is 4 commits
   ahead of main and 1 behind (`abe68c7`). Its purpose is fulfilled by
   upstream in M1, so it merges into main after M1 (see M1). From M2 on,
   work goes on a branch cut from main.
3. **sdhost lost-sector fix, independent of the pin.** Upstream `6042b4a8ea`
   (2026-10-18) found that on Pi 1-3 the CMD12 sent right after a write's
   DMA ends makes the card drop the last block. DMA completion only means
   the last word reached the FIFO. Our `soc/sdcard/sdcard_sdhost_bus.c` is a
   fork with no such wait. Port only `sdhost_wait_write_done()`. Do **not**
   port the switch to `DMAWaitChannel()` in the same commit: upstream says
   sleeping is safe "because DMA only ever targets the line-aligned bounce
   buffer", and ours DMAs straight into the caller's buffer since `9df9c35`.
   Our comment at line 258 describes the corruption sleeping would cause.
   Belongs to [ISSUE-0040](ISSUE-0040.md). It goes first because every
   milestone after it writes to the card.

## M0 — result (2026-10-20)

**Baseline: `f7b538f` with branch `bluetooth-aarch64-port` at `e05f39f`
reaches the desktop under QEMU.** Wanderer is up with RAM Disk, the card
volume and Wireless Manager. The serial log has 645 lines; the BootUI
sequence runs BELLATRIX, EXEC, KERNEL, DOS, SERVICES, WANDERER. The only
failures are WiFi's, which are expected with no chip under QEMU (`SDIO
probe failed: initial CMD5`). SD reads run at 46 KB/s
(`[SDHost00] 1024 cmds ... 4096 KB in 87217 ms`).

**Why ISSUE-0085's boot did not get there.** The first `build-aros.sh full`
here died on
`No rule to make target '.../workbench/hidds/v3d/v3d_aros_override.h'`.
`gen/workbench/libs/mesa/20.0.8/gallium_v3d/` was entirely from 2026-08-25/26,
from before upstream moved v3d to `arch/arm-native/.../hidd/v3d`
(`983cc6704b`, in the 08-31 bump). Its `.d` files still named the old path.
So **no `full` build had completed since that bump.** The card ISSUE-0085
booted carried modules from a distribution tree older than its kernel,
which is ISSUE-0058 §7's trap. That is the likeliest reading of the stall
it left unattributed. The fix was to delete that directory and
`gen/lib/mesa20.0.8/libgallium_v3d.a`, both generated. On a bump that moves
a directory, look for generated `.d` files that still name its old path.

**M0.3 done (`721e3b5`):** the sdhost write wait is ported and the DMA sleep
is not. `tests/gl/sdwrite` copies three files (155916, 202092 and 16648
bytes) at boot. Read back off the image with `mcopy`, all three are
identical. No `write did not finish`, and the boot reaches Wanderer. This
shows the wait does not break writes. It does not show the fix: QEMU drains
the FIFO synchronously and cannot lose the block, so that needs a Pi.

Noticed, not chased: files AROS writes to the card get the FAT date
2106-01-01. Look at it in M7, where the FAT date series arrives.

# The milestones

Position `#n` is the commit's index in the 1243-commit range.

## M1 — `78f2959e68` (#19, 2026-09-01): retire the Bluetooth port

- **Upstream:** our own `0095`-`0099` and the
  `arch/arm-native/soc/broadcom/2708/bluetooth` transport, merged. Diffed
  against ours: identical apart from three `.gitignore` files.
- **Patches:** retire `0095`-`0099`. Remove `aros/arch/arm-native/`;
  after M1, `setup.sh` would silently stop linking it anyway
  ([ISSUE-0087](../consolidated/history/ISSUE-0087.md), *The silent half*).
- **Not retired:** `aros/arch/m68k-emu68/soc/bluetooth`, the m68k sibling.
- **Done when:** ISSUE-0087 closes; the branch merges into main.

**Result (2026-10-20): done.** ISSUE-0087's own order was wrong. `--reset`
re-applies the series, so the patches would have been deleted while still
applied. ISSUE-0085's procedure was used instead. After `setup.sh`,
`--verify` says `applied (70 patches)`. The only fork change in range is
`bcm2708.h`, now re-synced: it differs from upstream's in the same 99 lines
it did at the old pin, which are the guard and the Bellatrix block. The
toolchain digest is unchanged. `full` build, fresh card, QEMU: the desktop
is the same as M0's, with the same expected WiFi failures, SD at 47 KB/s
against 46, and the m68k Bluetooth sibling loading. ISSUE-0087 is
resolved.

## M2 — `2e30a0877e` (#144, 2026-09-09): Mesa 26 and the BCM2712 drivers

- **Upstream:** Mesa 26.0.0 with the vc4/v3d ports to it. Also RP5 changes
  to `sdio`, `sdcard`, `dma` and `bwfm`, `MISSING_64BIT_ATOMICS` on every
  32-bit target, ELF undefined-weak, CD32 work, and amigavideo planar
  viewport updates.
- **Patches:** `0001` and `0028` (configure hunks, re-anchor), and `0090`
  (amigavideo, against `8643e85cd8`).
- **Ports:** **`hidd/vc4gallium` will not build against Mesa 26 until the
  glue changes are ported** (`50111fcd3e`, `5ba0a02190`, `a3cfa659ed`,
  `589dba6334`). This is a blocker for the `full` build, not an option. Also
  re-sync `bcm2708.h` for `3ecd43bdfa`.
- **Watch:** `MISSING_64BIT_ATOMICS` lands on patch `0057`'s m68k
  test-and-set corner. [ISSUE-0045](ISSUE-0045.md) and
  [ISSUE-0051](ISSUE-0051.md) are reasoning about a Mesa that is about to
  be replaced, so re-baseline them here.

### M2 is split, and Mesa 20.0.8 stays supported (decided 2026-10-20)

From `2e30a0877e` on, upstream's `configure` builds Mesa 26.0.0 unless
`--with-mesagl=<version>` says otherwise. Master still ships
`mesa-20.0.8-aros.diff`, and the 20.3.2, 21.0.2 and 26.0.0 diffs, beside it.

- **M2a:** move the pin, build with `--with-mesagl=20.0.8`. What is left is
  the small part: `0001`, `0028`, `0090`, the `bcm2708.h` re-sync, and the
  RP5, ELF and atomics changes.
- **M2b:** build with upstream's default, Mesa 26. Port the `vc4gallium`
  glue, carry the m68k and big-endian fixes (`0061`-`0067`, `0070`, `0071`)
  over to Mesa 26 where they still apply, and check `0036`, `0069` and
  `0079`. M2b comes straight after M2a, so no later milestone is measured on
  a Mesa other than upstream's default.

**Follow upstream's model, so it stays easy to track.** Upstream carries
one `mesa-<version>-aros.diff` per Mesa version and picks the version in
`configure`. Bellatrix mirrors that shape instead of inventing its own:

- **One set of fixes per version, on that version's diff.** `0061`-`0067`,
  `0070` and `0071` already patch `mesa-20.0.8-aros.diff`. Their Mesa 26
  counterparts patch `mesa-26.0.0-aros.diff` in the same way, and their names
  say which version they belong to. Diffing our series against upstream's
  files then shows exactly what we carry, per version. Mesa 20.0.8 stays a
  working choice on `emu68-m68k`, with its fixes kept beside Mesa 26's
  rather than retired once Mesa 26 works. The 20.3.2 and 21.0.2 diffs have
  never had our fixes and are not claimed.
- **`BELLATRIX_MESAGL` is a 1:1 pass-through of `--with-mesagl`**, empty by
  default so upstream's default applies. Changing it forces a reconfigure,
  as `BELLATRIX_FRAME_POINTERS` does. It is permanent support, not an M2a
  stopgap. `build-aros.sh` currently passes `configure` only `--target` and
  `--enable-ccache`.
- **Send the fixes upstream, into their diff files.** Upstream already took
  a big-endian Mesa fix for ppc (`cfdecb2c1f`), and our FAT and Bluetooth
  work. Each fix it takes is one patch fewer to carry and re-check at every
  bump.

## M3 — `92a7198be1` (#205, 2026-09-13): m68k stack alignment

- **Upstream:** `765871daef` *Fix m68k stack alignment and minimum process
  size*, which changes every process stack on this CPU. Also the FAT clean
  shutdown bit (`294e6e2788`) and Mesa without `-ffast-math`.
- **Patches:** none new.
- **Ports:** `aros/workbench/devs/AHI/Drivers/hdmiaudio` gets `ec982ac781`
  (RPiHDMI compatible with BCM2712, following the updated `dma.resource`).
  We compile `dma.resource` from upstream in place, so it changed under our
  fork in M2. Read the fork against it here, before anyone takes the
  silence for a regression.
- **Why alone:** it is the one change in September that can break
  everything at once on m68k and nothing on any other target, and it must
  not share a boot with the toolchain change.

## M4 — `08e347406b` (#230, 2026-09-14): the toolchain moves

- **Upstream:** `52bab89955` rewrites `tools/crosstools/gnu/gcc-6.5.0-aros.diff`,
  which is **the m68k gcc**. It adds `cxx-startup%O%s` to
  `STARTFILE_SPEC`, sets `default_use_cxa_atexit=yes` and fixes the libgcc
  unwinder. `08e347406b` edits `gnu/mmakefile.src`. Both are inside
  `toolchain_digest()`, so the cached toolchain stops matching. The rest of
  the range is aarch64 SMP, which does not reach this target.
- **Cost:** hours of gcc build, or a `toolchain-<digest>` release from CI if
  one exists by then (`--yes` to accept). See [ISSUE-0026](ISSUE-0026.md).
- **Watch:** every m68k binary now links `cxx-startup.o`. A boot difference
  here is the toolchain's, which is why nothing else rides in M4.

## M5 — `19ba9b56bc` (#398, 2026-09-25): m68k task state, USB

- **Upstream:** `7c59bc3f94`, `38dc95623a`, `0162d11a35` (right-sized
  m68k CPU context, compact retained task state, sparse exception-handler
  list); geometric exec pools; Shell buffers; OpenLibrary versioning;
  `CreateNewProc` CPU placement.
- **Patches:** `0093` (it conflicts with `38dc95623a` on the `IntETask`
  layout, and `0094` reads what `0093` adds), and `0082` (AROSTCP, against
  `278a49ddc4`).
- **Ports:** `soc/usb/usb2otg` gets the 19/09 batch: NAK backoff, SOF gated
  behind pending work, no claimed isochronous support, SOF wake from the
  system timer, and NAK-as-liveness on direct bulk IN. Also carry ISSUE-0058
  §5's still-open `brcm,bcm2835-usb` compatible string.
  [ISSUE-0056](ISSUE-0056.md), [ISSUE-0063](ISSUE-0063.md) and
  [ISSUE-0064](ISSUE-0064.md) are about this exact model. Timer: read
  `8a8d180631` (tick compare for the head `MICROHZ` request) against our
  `timer/ticks.c`.

## M6 — `37313d8aa0` (#666, 2026-10-03): affinity, TLSF, FAT cache

- **Upstream:** `task.resource` `SetTaskAffinity()` and inherited affinity;
  `6aa1da1cde` TLSF area check made O(1); `efbf24238c` FAT block cache
  scaled to the volume; `b76276f7dc` m68k system tasks on 2 KiB stacks; the
  DOS boot-shell rework; planar graphics on AmigaVideo.
- **Patches:** `0023`, `0037`, `0038` (FAT). Classify `0037` and `0038`
  against `efbf24238c` before rebasing: the upstream change may supersede
  "a cache the size of the machine". `0007`, `0011` and `0045` (TLSF) still
  apply, but must be read against the new area check, because
  [ISSUE-0027](ISSUE-0027.md) is a live free-list corruption.
- **Ports:** `hidd/vc4gallium` gets the 27/09 VC4 work (VC4 is the Pi 3's
  GPU): FLDONE to CT1 service task, overlay present, expunge wait, pinned
  overlay page. Read `ca064a18a5` (E-clock reload) against our timer.
- **Watch:** 2 KiB system stacks on a JIT that has failed on stack before.

## M7 — `b3404fa127` (#798, 2026-10-08): the exec scheduler, and the hard part

- **Upstream:** `ba01955a0b` *exec: keep the scheduler flags where
  Kickstart has them, honour its hooks*. Also our FAT endianness series,
  merged (`0253a74ec0`, `286501265b`, `1fba5bcd9f`, `9213952b4f`), the ELF
  loader fixes, the pthread exit/cancel series, and Bluetooth on the Pi
  PL011 following the device tree.
- **Patches:** `0002` and `0010`, the m68k-that-is-not-an-Amiga core of the
  port, both against `ba01955a0b`. Also `0006`, `0008` and `0039`.
  `0006`, `0008` and `0023` should **retire**, not rebase, because upstream
  now carries the same fix. Confirm with `--empty=drop`.
- **Ports:** **`exec/dispatch.S` is our override of `m68k-all/exec/dispatch.S`,
  which `ba01955a0b` changed.** If the rest of exec moves the flags and our
  dispatcher does not, the two read different places. This is mandatory, and
  it is the same change as `0002`, so it is one piece of work.
  `soc/bluetooth` should get `73eba4ee38` and `ee2e5812c1` where the m68k
  sibling's divergence allows.
- **Records:** [ISSUE-0038](ISSUE-0038.md) (FAT typing). Its endianness
  half is upstream now.

## M8 — `7a69e65d61` (#1210, 2026-10-18): the rest of October

- **Upstream:** mostly outside this port: network MIDI, mDNS, per-user
  assigns, Rust std, posixc docs.
- **Patches:** none new.
- **Ports:** `soc/wifi/bwfm` gets `bd0d438f7d` (join the node the
  supplicant picked, hold the link until the keys are in), related to
  [ISSUE-0054](ISSUE-0054.md) and [ISSUE-0065](ISSUE-0065.md). Timer:
  `be37a66e96` (request lists locked, reply outside the lock).

## M9 — `a399fe2fc7` (#1243, 2026-10-19): SD speed and m68k interrupt nesting

- **Upstream:** the rest of `6042b4a8ea` (sdhost in 127-block pieces, equal
  split, card write-protect report); `a5e6a6d1c0` FAT 64 KB reads with
  neighbouring dirty ranges written together; `4ac3fbc085` m68k interrupt
  nesting and CCR across task exceptions; and `646e6ddc3c` registers
  preserved across blitter ownership.
- **Patches:** none new.
- **Ports:** **`kernel/kernel_cpu.c` is our override of
  `m68k-all/kernel/kernel_cpu.c`, so `4ac3fbc085` does not arrive by
  itself.** The sdhost pieces go in only after M0.3 has been measured, and
  never the DMA sleep (see M0.3).

# Read and set aside

These are recorded so the next pass does not re-read them. Everything BCM2712
or Pi 5 is set aside: vcgfx's 16 commits, RP1, PCIe/MSI, NVMe, and the Pi 5
power button. So are the aarch64 SMP, GIC-400, PSCI and IPI work, plus
`boot.c` and `kernel_arch.h` changes in `aarch64-raspi`, and amdgpu,
vmwaresvga and nouveau. `m68k-amiga/kernel/amiga_irq.c` (`8480de989a`,
INTENA master bit) is set aside because this port does not take the Paula
path (CLAUDE.md, *IRQ bridge*).

# Exit

The exit criteria are ISSUE-0069's. In addition: one commit (or a short
series) per milestone, each naming its upstream hash, and this file updated
with what each boot established.
