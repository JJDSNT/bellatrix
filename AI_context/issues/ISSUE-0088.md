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

> **Correction, 2026-10-24: M2 was not split after all, and Mesa 20.0.8 is
> no longer supported.** Building it at this pin showed that upstream's
> `--with-mesagl=20.0.8` has not built for anyone since its Mesa 26 port. The
> option and some 20.0.8 paths remain, but `mesa3dgl.library`'s own glue was
> rewritten for Mesa >= 22. See *M2 — result* below. The model described here
> still holds: per-version fix files, and `BELLATRIX_MESAGL` passed through
> unchanged.

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
- **Sending anything to AROS is not part of this refresh** (the user,
  2026-10-24). The refresh updates Bellatrix. Upstreaming, if it happens,
  is a separate decision taken after it.

### Our Mesa fixes stop being patches of a patch (researched 2026-10-20)

**Found while researching this: the Mesa build is not reproducible today.**
`0061`-`0071` edit `mesa-20.0.8-aros.diff`, upstream's patch against the
Mesa sources. With `0061` applied, that file is **malformed**. Its
`ralloc.c` hunks carry wrong line counts: `@@ -160,13 +164,18 @@` heads 11
old lines and 15 new ones. `patch` rejects the whole file
(`malformed patch at line 1253`) under every later patch in the series.
`git apply` of the outer patch cannot notice, because it never parses the
diff inside.

It has gone unseen since 2026-08-25 because `fetch.sh` never looks again:

- it copies each patch into `Ports/mesa/` once and from then on prefers
  that copy (`fetch_cached` checks the destination before the origins);
- it stamps `.mesa-20.0.8.tar.xz.unpacked` and `.<patch>.applied`, and
  never unpacks or patches again while those exist.

This tree's copy is upstream's original 42512-byte file from 2026-08-21.
The fixes reached the compiled sources because they were applied to
`Ports/mesa/mesa-20.0.8/` by hand (`670955f`: "applied and tested"). So
this machine builds a GL that works. A fresh clone, CI, or anything that
clears `Ports/mesa` fails in `mesa3d-fetch` instead.

**The fix is the model chosen above, and it already exists.**
`aros/workbench/libs/mesa/bellatrix/mesa-20.0.8-bellatrix.diff` (158 lines,
15 hunks) is `diff -ruN` between tarball + upstream's unmodified
`aros.diff` and the hand-patched sources in `Ports/mesa`. Those differ in
exactly the four files `0061`-`0071` target, and nothing else:
`cso_cache.c`, `ff_fragment_shader.cpp`, `formats.c` and `ralloc.c`.
Applied with `patch -p1` on top of upstream's diff, it reproduces the
tested sources byte for byte. It is committed now, ahead of M2a, because
until then the only other copy of these fixes is a build tree that a
`clean` deletes.

How it goes in, in M2a. Both questions are answered from `scripts/fetch.sh`:

- **A second patch is supported.** `PATCHSPEC` is a list
  (`for patch in $patches`), applied in order, ours after upstream's.
- **It cannot be named with a subdirectory.** `bellatrix/x.diff` would
  have its copy and its `.applied` stamp written under a `.bellatrix/`
  that does not exist. `patches_origins` *is* a list, though
  (`fetch_multiple`: `for origin in $origins`). So the patch keeps a flat
  name and `bellatrix/` is added as a second origin.
- **One numbered patch, two lines:** `mesa.cfg` appends
  `$(ARCHBASE)-bellatrix.diff:$(ARCHBASE):-p1` to `PATCHSPEC` only when that
  file exists (`$(wildcard ...)`; a listed patch that cannot be fetched is
  a fetch error), and the `%fetch` in `mmakefile.src` gets
  `patches_origins="$(SRCDIR)/$(CURDIR) $(SRCDIR)/$(CURDIR)/bellatrix"`.
  A version we carry nothing for builds exactly as upstream's.
- **`0061`-`0067`, `0070` and `0071` are deleted** (un-applied first, per
  CLAUDE.md). `0064` and `0067` existed only to repair the inner diff's
  context.
- **Editing a patch must invalidate `Ports/mesa`, and today nothing
  does.** That holds for upstream's `aros.diff` on a pin bump too, so M2b
  needs it either way. `build-aros.sh` stamps a digest of the patch files
  the selected `PATCHSPEC` names, and on a mismatch removes
  `Ports/mesa/<version>`, the copied patches and their stamps. This is the
  same pattern as the configure digest.
- **Proof:** clear `Ports/mesa`, build, and boot GL as ISSUE-0045 did.
  Until a fetch from scratch has been seen to work, the old failure is
  only moved, not fixed.

Mesa 26 gets `mesa-26.0.0-bellatrix.diff` in M2b, written the same way.

### M2 — result (2026-10-24): done, on Mesa 26

Done in the worktree `/home/jaime/bellatrix-m2a`, branch
`aros-refresh-m2a`, from a build with no prior tree. A build that never
touched a hand-patched tree is the reproducibility proof this milestone
needed. Pin `2e30a0877e`, 61 numbered patches.

**Mesa.** Upstream's `--with-mesagl=20.0.8` fails in five places. Four are
peripheral, and each was fixed the way upstream fixes the same thing next
door: the `vk_video` include copy, `egl.library` (never built before Mesa
26), vc4's CLE XML, and v3d's unconditional place in vc4's GalliumCoreAPI
union. Those are patches 0101-0104, kept in `33eb0af`'s history and then
retired. The fifth is the library glue itself: `mesa3dgl_types.h` lost its
`__has_include` fallback and `mesa3dgl_*.c` now use the post-22 API without
`st_api`. Keeping 20.0.8 would mean forking that glue. So M2 follows
upstream's default, and the M2a/M2b split collapsed into one milestone.

`mesa-26.0.0-bellatrix.diff` carries what Mesa 26 still needs:

- **ralloc**, aligned allocation and a realloc-free resize. Mesa 26 has
  `alignas(8)` on the header but still `malloc()`s it.
- **cso**, with `cso_construct_key()` folding a key's tail in instead of
  asserting the size is a multiple of four.

The `formats.c` fix is now in Mesa itself, and the fixed-function
instrumentation's file is gone. Under QEMU: `GL_VERSION: 3.3 (Compatibility
Profile) Mesa 26.0.0`, renderer softpipe, through `tests/gl/gl-info`. VC4
needs a Pi.

**Mechanism verified.** Patch 0100 applies the per-version diff as a
second `PATCHSPEC` entry; the fetch stamps both patches `.applied`. The
`Ports/mesa` invalidation had a bug: it read `OPT_MESAGL` from the wrong
file. Once fixed, it rebuilt Mesa 26's sources from the tarball on its
first run.

**Reproducibility, found on the way.** A from-scratch `full` build made a
card without the Bluetooth stack, the WiFi and Bluetooth firmware, the
network prefs, `sfs-handler`, `dos64.library` or our `C:` probes. Those
targets had followed the lean build only, or had never been in the script,
and the main tree had them only because they had once been built into it.
`build-aros.sh` now builds them after `full` as well. Card contents now
match M1's, except for four stale `Devs/` copies of modules the kernel
carries (`audio.device`, `dosboot`, `lddemon`, `pl011bt`), which the main
tree should drop as well. `make-sdcard.sh` and the GL probes no longer name
`mesa3dgl20-0`; they read `ENVARC:SYS/GL.default`, as `gl.library` does.

**Boot, QEMU.** The desktop comes up with Wireless Manager, the WiFi
failures are the expected ones, and SD reads at 41 KB/s (single run).

**Forks** (`upstream-forks.py`, M1..M2):

- **Ported:** `bcm2708.h` (`72311ea24c`, `3ecd43bdfa`); the sdio rename and
  `IPTR`; bwfm's fail-without-sdio (`2e797599cc`); the vc4gallium glue
  (`622d07129a`, `a3cfa659ed`).
- **Rejected as BCM2712-only:** `f3b47ce8e1` (arasan.h, sdcard),
  `b6fd46e3d6` (vcgfx), the 2712 bail in `72311ea24c`.
- **Retired:** `0090`, because upstream `8643e85cd8` removed
  `waitvblank()`.

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

### M3 — result (2026-10-24): done

Pin `92a7198be1`, and the series applies unchanged (61 patches).

- **Stack alignment (`765871daef`).** It reaches this target through one
  path: `AROS_STACKALIGN 8` in `arch/m68k-all/include/aros/cpu.h`. The
  generic `rom/exec/newaddtask.c` uses it to align `tc_SPReg` before our
  `PrepareContext` runs, and we override neither file.
  `PROC_MINSTACKSIZE` in m68k-amiga's `dos_platform.h` does not apply,
  because this target compiles `rom/dos/dos_platform.h`. The `cpu.h`
  change rebuilds the whole tree, and ccache cannot help with that.
- **Mesa.** Upstream changed `mesa-26.0.0-aros.diff` in this range. The
  build noticed and rebuilt `Ports/mesa` from the tarball, the first time
  that mechanism fired on an upstream change and not one of ours.
- **Fork (`hdmiaudio`, `ec982ac781`).** Ported only the RAM-packet fix:
  disable the infoframe slot and wait for it to go idle before writing,
  then enable it together with the packet path (bit 16). Not taken: DMA4
  and dma40 (BCM2712), the second HDMI port, 64-bit peripheral bases, and
  the legacy engine's address translation, which `GPU_BUS_ADDR` already
  does since `51a300c`. Audio cannot be tested under QEMU, so this needs
  a Pi.
- **QEMU.** The desktop comes up as in M2, with the same expected WiFi
  failures, SD at 47 KB/s, and `gl-info` reporting Mesa 26.0.0 on
  softpipe.

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

### M4 — result (2026-10-24): done

Pin `08e347406b`, and the series applies unchanged. The toolchain digest
moved from `a88db85e62ede04f` to `8469c4648a37b6a6`. No prebuilt copy
existed (the CI release 404'd), so `build-aros.sh clean full --yes` built
binutils 2.32 and gcc 6.5.0 and then all of AROS: 1 h 53 min on this
machine. The new toolchain is in `~/.cache/bellatrix/toolchain`. This is
the only toolchain change between the old pin and HEAD; no later milestone
moves the digest.

- **Forks** (`upstream-forks.py`): three commits, all rejected because the
  m68k guest runs on one core. `642046b4a1` (usb2otg worker priority)
  touches a task that exists only under `__AROSEXEC_SMP__`. `b6c3d80b2b`
  (EClock watermark CAS) fixes a race between cores inside `Disable()`.
  `95f5d15b77` (getsystemattr) is the aarch64 SMP layer.
- **QEMU.** Same as M3: desktop, expected WiFi failures, SD at 43 KB/s,
  `gl-info` on Mesa 26.0.0 and softpipe, all now built with the new gcc.

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

### M5 — result (2026-10-24): done

Pin `19ba9b56bc`, 62 numbered patches.

- **The IntETask build blocker.** New patch `0044` keeps the full structure
  on emu68: `#ifndef __mc68000` becomes "not m68k, or `__EMU68__`", and
  `ETaskAlertData()` keeps `iet_AlertData`. `0093` is re-anchored inside
  the gated block. `tests/gl/cpu-usage` (TaskList late in boot) shows real
  shares, for example input.device 42.84%, the Denise plane task 13.05%
  and the Exec housekeeper 0.76%. `CPU Time` reads zero, as before: this
  port fills the share, not `iet_CpuTime`.
- **`0082` re-anchored.** Upstream starts WirelessManager with `NOGUI`
  (`278a49ddc4`), and we keep our order. The visible effect is that the
  Wireless Manager AppIcon is no longer on the desktop; the WiFi
  preferences program drives the supplicant now. WiFi is still opened at
  boot.
- **`7c59bc3f94` taken as is.** It sizes `KrnCreateContext()` by FPU and
  68080 presence, which is exactly what our `kernel/context.c` saves.
- **Forks.** usb2otg took upstream's six commits of 2026-09-19 unchanged.
  The SOF wake is on system timer channel 1, which is free here: our
  heartbeat uses 3, VideoCore uses 0 and 2, and Emu68 reads only CLO. It
  also took the `brcm,bcm2835-usb` binding left open by ISSUE-0058 §5.
  `dwc2emu68` is still the card's default controller.
- **Deferred, not rejected: `8a8d180631`** (arm the tick compare for the
  head `MICROHZ` request). Here the compare channel belongs to the
  platform heartbeat in the kernel, so `timer.device` re-arming it needs a
  design of its own, not a port inside a bump. Until then a `Delay` waits
  for the periodic tick, as it always has.
- **QEMU.** The desktop comes up, with the same expected WiFi failures, SD
  at 42 KB/s, and `gl-info` on Mesa 26.0.0.

Also found while this ran: the CI toolchain job had been broken since
`06fd8ad` (2026-08-24), because `build-aros.sh` built the USB drivers even
for `--toolchain-only`. It is fixed on main, and the release
`toolchain-8469c4648a37b6a6` is now published, so M4's toolchain downloads
instead of building.

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

### M6 — result (2026-10-24): done

Pin `37313d8aa0`, 62 numbered patches.

- **FAT.** `0037` is retired in favour of upstream's volume-scaled cache
  (`efbf24238c`). `0038` is refreshed, with the alignment now inside the
  allocation check upstream added; the allocation base is still what gets
  freed. `0023` is re-anchored and retires in M7. Measured: card reads at
  boot went from 42-47 KB/s to **177 KB/s** (`2048 cmds, 16212 KB`). That
  is a single run, but the size of the change is not noise. `sdwrite`
  reads three files back identical.
- **New `0105`.** GCC 6.5 hits an internal error on `con_handler.c` here
  ("unable to find a register to spill in class ADDR_REGS"). Upstream
  grew `CONMain()` past what the m68k address registers hold once `%a6`
  is a frame pointer. Compiling the file both ways confirmed the cause.
  The module now omits the frame pointer on m68k.
- **Stacks (`b76276f7dc`).** Taken: the housekeeper and console.device
  run on 2 KiB. Measured under load with `cpu-usage`: 164 of 2048 bytes
  and 332 of 2052.
- **TLSF O(1) (`6aa1da1cde`).** It applies beside `0007`, `0011` and
  `0045`. No `[Kernel:TLSF]` report appeared in any M6 boot, including
  `gl-info`'s 15 MB library load.
- **Forks.**
  - Ported: `bcm2708.h` (`2fce860abc`) and `videocore.h` (`eb67886d75`);
    `vc4gallium`'s six VC4 commits of 2026-09-27, two hunks placed by hand
    (an ARM `dsb` becomes our `__sync_synchronize()`, and the V3D check
    reads the device tree's `brcm,vc4-v3d`, which the Pi 3 tree has).
  - Deferred, both to be checked on a Pi because QEMU has no VC4 overlay:
    the HVS4 half of `b1a906ce68` (deferred latch with `LatchWait`, which
    our vcgfx lacks, so vc4gallium's no-wait present still waits inside
    `vc4_hvs_overlay()`), and `ced37ca08f` (take a GL overlay down when
    something draws over it, which needs every vcgfx draw path
    instrumented).
  - Rejected: `battclock` `28674a157b` (the Pi 5 RTC, and the DS3231 needs
    an I2C driver this port lacks; the file-backed path is unchanged);
    `ca064a18a5` (CIA E-clock relatch, not our timer); vcgfx's BCM2712
    commits; `cf91862a8d` (Pi 5 SDIO).
- **QEMU.** Desktop, the expected WiFi failures, `gl-info` on Mesa 26.0.0.

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

### M7 — result (2026-10-25): done

Pin `b3404fa127`, 59 numbered patches.

- **Exec scheduler (`ba01955a0b`).** This changes four contracts: the
  scheduler flags move into `SysFlags` at Kickstart's bits (13, 14, 15),
  interrupt handlers leave through the `ExitIntr()` vector with SysBase
  pushed, `Schedule()` decides with interrupts masked, and tasks are
  entered through `ex_LaunchPoint`. Our overrides, which the bump does not
  reach, were brought along:
  - `exec/dispatch.S` clears bit 14 with `bclr.b #6`, because
    `SFF_QuantumClear` is gone. It reloads A3 from `ThisTask` after
    `emu68_DispatchFrame` and enters through `ex_LaunchPoint`, with the
    CPU accounting hook still before it.
  - `Platform_Autovector_Direct` pushes SysBase and jumps through the
    `ExitIntr()` vector, as upstream's `amiga_irq.c` does. Jumping
    straight to `Exec_6_ExitIntr` would now pop a saved register as
    SysBase.
  - `0002` is re-anchored: its `__EMU68__` guard closes before the
    `ex_LaunchPoint` entry, which runs everywhere. `0010` applies
    unchanged.
- **FAT.** `0006`, `0008` and `0023` are retired: upstream has every
  conversion they made, checked site by site (`ops.c` 820, 978, 1115;
  `direntry.c` 241 and 602; `date.c` 30; `volume.c` 672). They no longer
  reverse-apply only because upstream's form is refined.
- **`0039`.** Re-anchored around `7a9809157b`, and a bug of its own fixed:
  the timing wrapper turned "fail if there is no block or relocation
  fails" into `!addr || relocate()`, which let a failed `load_block()`
  through.
- **Forks.** Ported: `h4bthci`'s allocated ready signal, and deciding
  before `Permit()` (`73eba4ee38`). Deferred: `pl011bt` following the
  device tree (`73eba4ee38`, `ee2e5812c1`); our m68k `pl011bt` diverged
  (similarity 0.48) and the change is about which UART and pins the radio
  gets, so it wants a Pi.
- **QEMU.** No Software Failure, alert or TLSF report in four boots, plus
  the following:
  - `sdwrite`: identical read-back.
  - `gl-info`: Mesa 26.0.0.
  - `cpu-usage`: accounting live through the new dispatch path.
  - SD reads: 176 KB/s.
  - Files written under QEMU still get FAT date 2106-01-01 after the date
    series. The likely cause, not confirmed: QEMU has no clock, the system
    starts in 1978, and that is before FAT's 1980 epoch.

## M8 — `7a69e65d61` (#1210, 2026-10-18): the rest of October

- **Upstream:** mostly outside this port: network MIDI, mDNS, per-user
  assigns, Rust std, posixc docs.
- **Patches:** none new.
- **Ports:** `soc/wifi/bwfm` gets `bd0d438f7d` (join the node the
  supplicant picked, hold the link until the keys are in), related to
  [ISSUE-0054](ISSUE-0054.md) and [ISSUE-0065](ISSUE-0065.md). Timer:
  `be37a66e96` (request lists locked, reply outside the lock).

### M8 — result (2026-10-25): done

Pin `7a69e65d61`, and the series applies unchanged (59 patches).

- **Forks.**
  - Ported:
    - `bwfm` `bd0d438f7d`: join the BSSID the supplicant picked, which
      matters in a mesh, and hold the link until the keys are in. One
      hunk landed with fuzz beside our own trace line.
    - `usb2otg` `fd3ad20c3a`: arm direct INT as INT on QEMU's DWC2.
    - `sdcard_bcm2708init.c` `71623f789e`: clear a latched card-insert
      before enabling its signal. It is plain SDHCI, so our Arasan backend
      had the same order, and it is a candidate for ISSUE-0029's boot-time
      timeout, to be confirmed on a Pi.
  - Rejected: `be37a66e96` (timer request lists locked against other
    cores; the guest has one), and two documentation-only commits
    (`595121c85c`, `75659fb32b`).
- **QEMU.** Desktop; the only failures are WiFi's expected ones (AROSTCP's
  Syslog line is gone). SD reads at 204 KB/s. `sdwrite`, `gl-info` and
  `cpu-usage` pass, with no alert or heap report.

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

### M9 — result (2026-10-25): done

Pin `a399fe2fc7`, 59 numbered patches.

- **`0038` refreshed** for `a5e6a6d1c0`, which replaced `RANGE_SHIFT` with a
  per-cache `range_shift`. The write-coalescing `flush_buf` that commit adds
  is not 32-byte aligned, so its writes go through the SDHOST bounce
  buffer. That is correct, only slower.
- **Task exceptions (`4ac3fbc085`), ported to our 68-byte frame.**
  `context.c` keeps the interrupted SR beside the PC past the shifted frame.
  `cpu_Exception` calls `Enable()` to balance the `Disable()` that
  `Exec_Exception()` never did, then restores CCR before returning. Upstream's
  `exceptioncontext` test passes (0 failures, 100 callbacks, 50 M
  comparisons). Its `signalexception` test fails here, but the same failure
  occurs with these changes reverted, so it predates them:
  [ISSUE-0089](ISSUE-0089.md).
- **SDHOST, the rest of `6042b4a8ea`.**
  - The bounce buffer is now aligned to 64 bytes, the A53 line.
  - 127-block commands and equal pieces arrive with the pin in
    `rom/devs/sdcard`. They fit our lite DMA channel's 16-bit length,
    because only ADMA2 buses set `AF_Bus_DMA`.
  - `DMACHF_IRQ` and the DMA sleep stay out on purpose (see M0).
- **QEMU.** Desktop with the expected WiFi failures, SD at 175 KB/s;
  `sdwrite`, `gl-info` and `cpu-usage` pass, with no alert or heap report.

### M10 — `6fe38896f3` and later (added 2026-10-25)

Master moved after this plan was written: 72 commits between `a399fe2fc7`
and `6fe38896f3` (2026-10-24), more by the time M10 starts. They are not an
appendix to M9:

- `b72155e2bb` moves display composition into `gfx.hidd` and adds
  composited layers. This is the compositor that today requires
  `nocomposition` at boot (ISSUE-0048), and it touches files our patches
  touch.
- Two `ram-handler` commits (`df6a9f8e40`, `f44a5135a1`) touch files that
  three of our patches touch.
- There are also m68k-amiga ROM-space and trackdisk changes, and `dosboot`.

M10 is split in two, at the one commit that changes Mesa:

- **M10a — `e9e1f786da`**, the commit before the Mesa update: 98 commits
  past M9, the compositor and the RAM handler among them.
- **M10b — `6f8fbf34ed`** (Mesa 26.2.5), then master as found when it
  starts (`78a6c42c10` at the time of writing). Our Mesa fixes have to be
  ported to a new `mesa-26.2.5-bellatrix.diff` first, so this half is not a
  pin bump.

### M10a — result (2026-10-25): done

Pin `e9e1f786da`, 58 numbered patches.

- **`0004` re-anchored**; upstream's dosboot mmakefile now passes `-pz`
  itself.
- **`0046` re-anchored** on `b72155e2bb`. `display_LoadViewPorts()` is
  still the one place above every driver where a bitmap becomes frontmost,
  so the report stays there: one call in the `ShowViewPorts()` branch, one
  after the compositor returns a framebuffer.
- **`0030` retired.** It was a diagnostic for ISSUE-0037, which is closed
  with its cause found elsewhere (AllocMem(1) under `SIZE_ALIGN=4`), and the
  RAM handler it instrumented was rewritten upstream (`df6a9f8e40`,
  `f44a5135a1`).
- **`nocomposition` is no longer needed.** `b72155e2bb` deleted
  `DEVS:Monitors/Compositor`, which that argument kept from loading and
  which left the screen on the Emu68 logo; `AROSMonDrvs` now ignores the
  word. Booted both ways under QEMU: same desktop. `run.sh`,
  `make-sdcard.sh` and the release bundle stop passing it (`e292636`).
  Not yet checked on a Pi, where `vcgfx` finds an HVS and QEMU does not.
- **Stale output.** The full build left the old `Devs/Monitors/Compositor`
  from M9 in the dist tree; deleted by hand before making the card.
- **Forks.** `scripts/upstream-forks.py` reports `pl011bt_init.c` and
  `sdio_init.c`; both upstream changes are autodoc comments only
  (`0491d0ad4c`, `561f68cfc1`). Nothing to port.
- **QEMU.** Desktop; `sdwrite` (three files identical), `gl-info` (Mesa
  26.0.0, softpipe) and `cpu-usage` pass, no alert or heap report. Free
  memory on the title bar is about 1 MB lower than M8 (759.79M vs 760.85M),
  in line with the compositor moving into `gfx.hidd`.

### M10b — result (2026-10-25): done; the pin is master

Pin `78a6c42c10`, master on 2026-10-25, through `6f8fbf34ed`; 58 numbered
patches, none of them touched.

- **Mesa 26.2.5, the only version upstream now carries.** `e9e1f786da`
  removed `mesa-20.0.8-aros.diff` and `6f8fbf34ed` renamed 26.0.0's, so
  `--with-mesagl` has nothing to choose between today. Our
  `mesa-20.0.8-` and `mesa-26.0.0-bellatrix.diff` are retired with them
  (the decision of 2026-10-20 was to keep a version while upstream does).
- **`mesa-26.2.5-bellatrix.diff`.** The ralloc and `cso_hash_key` fixes
  carry over unchanged; their surroundings differ only in whitespace. One
  new fix: `nir_opt_varyings.c` asserts `sizeof(dedup_entry)` is 28 on
  32-bit hosts, and the m68k ABI makes it 26 (the trailing `bool` pads to
  two bytes). The assert guards zeroed padding, which holds with less of
  it, so it learns the ABI rather than going away.
- **`vc4gallium.conf`** follows upstream to version 1.3. The rest of the
  vc4gallium change is in the mmakefile `0079` patches, and applied.
- **What a Mesa version change leaves behind**, each of which stopped or
  polluted this build, now handled by `build-aros.sh` (stamp
  `.bellatrix-mesa-built`):
  - v3d's generated sources sit in an unversioned directory: wrappers
    that include the old tree by absolute path, and NIR and packet code
    made by the old generators;
  - `.d` files that name the old tree (`No rule to make target` once it is
    deleted, a false "up to date" while it exists);
  - the old `mesa3dgl26-0.library` in the distribution tree, 16 MB that
    `make-sdcard.sh` would copy.
- **The seven commits after the Mesa update** are AutoDocs, a bound on
  `Version`'s Resident scan and two build dependencies. No fork touched.
- **QEMU.** Desktop; `gl-info` reports `Mesa 26.2.5`, softpipe;
  `sdwrite` (three files identical) and `cpu-usage` pass; no alert or heap
  report.

With M10b the refresh reaches master. What stays open is recorded above
and is not part of a pin bump: timer `8a8d180631`, the vcgfx HVS4 latch
and overlay yield, pl011bt DT-following, the FAT 2106 date under QEMU,
`flush_buf` alignment, [ISSUE-0089](ISSUE-0089.md), and `nocomposition`
on a Pi.

# What is still open after M10 (2026-10-25)

The pin is at master and every milestone passed under QEMU. That is not
the exit: ISSUE-0069's exit criteria include its acceptance matrix, and the
hardware column of that matrix has not been run on any milestone. In
order of weight:

1. **The Pi 3 column of ISSUE-0069's acceptance matrix**, on the M10b
   build: repeated cold boots and shutdown/reboot; SD (DMA direct and
   bounced, timeout recovery, stress); timer (wrap-safe long run,
   `timer.device` deadlines); HVS scanout and VC4 GL render/submit/present;
   GPU, SD, USB, audio and framebuffer coherency; Bluetooth and WiFi
   transport, firmware, association and input; FAT/SFS stress and
   power-cycle recovery.
2. **Items deferred to a Pi** during the milestones:
   - boot without `nocomposition` (M10a; `vcgfx` finds an HVS there, QEMU
     has none);
   - the HVS4 half of `b1a906ce68` (deferred latch with `LatchWait`) and
     `ced37ca08f` (take a GL overlay down when something draws over it), M6;
   - `pl011bt` following the device tree (`73eba4ee38`, `ee2e5812c1`), M7;
   - `sdcard_bcm2708init.c` `71623f789e` as a candidate for ISSUE-0029's
     boot-time timeout, M8;
   - the real SD read rate. Every "SD reads at N KB/s" figure in the
     milestone results above is the driver's wall-clock report, which
     divides by the time between reports, idle included: it measures
     demand, not the card. Device-level under QEMU is about 550 KB/s with
     Rigel and 1.1 MB/s without, 95% of it inside the SDCMD write that QEMU
     executes synchronously; the driver's own code is about 30 ms per MB.
     The number that matters needs `rawbench`/`sdbench` on a Pi.
3. **Deferred by design, needing work of their own:**
   - timer `8a8d180631` (arm the tick compare for the head `MICROHZ`
     request): the compare channel belongs to the platform heartbeat here,
     so `timer.device` re-arming it needs a design, not a port (M5);
   - `flush_buf` from `a5e6a6d1c0` is not 32-byte aligned, so FAT's
     coalesced writes go through the SDHOST bounce buffer (M9);
   - FAT date 2106-01-01 on files written under QEMU; the likely cause,
     unconfirmed, is no RTC and a clock before FAT's 1980 epoch (M7);
   - [ISSUE-0089](ISSUE-0089.md): a task's own signal exception waits for
     the next tick (M9).
4. **Housekeeping:**
   - the SD driver's rate report should print the in-command rate beside
     the wall-clock one, so it stops reading as card speed;
   - `tests/gl/sdbench` still reads `mesa3dgl20-0.library`, which no longer
     exists (fixed only in the `bellatrix-m2a` worktree, uncommitted);
   - the main tree's `out/` predates M2: its next build reconfigures and
     rebuilds everything;
   - our Mesa fixes (`mesa-26.2.5-bellatrix.diff`) are candidates to offer
     upstream now that the refresh is done, as the owner planned
     (2026-10-20).

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
