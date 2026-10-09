---
id: SPEC-AROS-UPSTREAM-HEAD-UPDATE
title: "Update the AROS submodule and reconcile the Emu68 port with upstream HEAD"
status: draft
created_at: 2026-10-09
updated_at: 2026-10-09
---

# Goal and audited baseline

Update `external/aros` to a recorded upstream HEAD, reconcile the complete
Bellatrix patch series, and carry relevant improvements into the project-owned
port. The deliverable is a working `emu68-m68k` distribution, including its
drivers and accelerated graphics, with the existing compatibility and
diagnostic capabilities retained.

This is an implementation roadmap, not evidence that the update has been
performed. The submodule pin remains unchanged by this document.

| Snapshot | Commit |
|---|---|
| Bellatrix main inspected | `6cbe212c57f14abae932dc70a0af33c4a2d07929` |
| Current `external/aros` pin | `f7b538f8655a1395dc66e46a56814efe1ab27344` (2026-08-31) |
| Upstream master HEAD inspected | `3914027415c7a89c4449ea75bbe8683902538506` (2026-10-09) |
| Commits after the current pin | 814 |

Sources: [upstream comparison](https://github.com/aros-development-team/AROS/compare/f7b538f8655a1395dc66e46a56814efe1ab27344...3914027415c7a89c4449ea75bbe8683902538506),
[port](../../aros/arch/m68k-emu68/), [patches](../../patches/aros/),
[AROS integration](../../docs/aros.md), and [repository rules](../../AGENTS.md).
If master advances before execution, record its new full SHA and audit the
additional range. Do not describe an unrecorded moving branch as the baseline.

## Architectural rule

Bellatrix has the m68k guest ABI, task frames and executable format, with
Raspberry Pi hardware and drivers adapted from ARM/AArch64. Emu68 owns the
host execution environment. It is not an ordinary AArch64 AROS build.

There are two update surfaces:

1. Shared upstream code selected by the new pin and the local patch series.
2. Project-owned code under `aros/arch/m68k-emu68/`, injected by setup.

The second surface does not update when the submodule changes. Review it
explicitly, including USB, graphics, Bluetooth, SD, Wi-Fi, DMA and audio.
Preserve big-endian guest accesses to little-endian peripherals, address/bus
translation, the Emu68 interrupt contract and m68k-specific build selection.
Do not transplant AArch64 instructions or native IRQ assumptions into the port.

# Provenance: Bellatrix contributions already upstream

These are contributions returning through upstream, not newly discovered
external solutions. Commit metadata explicitly establishes the following:

| Bellatrix contribution | Upstream commit | Integration consequence |
|---|---|---|
| FAT directory-entry cluster writes (`0006`) | [`0253a74ec0`](https://github.com/aros-development-team/AROS/commit/0253a74ec0) | Commit cites the Bellatrix patch; its message notes OpWrite/OpSetFileSize were already encoded in that branch. Compare every original hunk before retiring the patch. |
| FAT dates (`0008`) | [`286501265b`](https://github.com/aros-development-team/AROS/commit/286501265b) | Commit cites the Bellatrix patch. Later `9213952b4f` makes date encoding width explicit; retain that refinement. |
| FAT decoded cluster lookup/rename and volume ID (`0023`) | [`1fba5bcd9f`](https://github.com/aros-development-team/AROS/commit/1fba5bcd9f) | Commit cites the Bellatrix patch. Retire only after checking all sites against HEAD. |
| Pi onboard `pl011bt.resource`, `h4bthci.device`, `brcmbt.fwl` | [`17da3e3db8`](https://github.com/aros-development-team/AROS/commit/17da3e3db8) | Authored by Jaime Dias; Bellatrix has preceding transport/firmware history. Reconcile later upstream fixes with the local m68k adaptations. |
| AArch64 Bluetooth patchram packaging | [`bcd86885e7`](https://github.com/aros-development-team/AROS/commit/bcd86885e7) | Authored by Jaime Dias. `e2cb3acaac` adds Pi 400/CM4 firmware and license. Check local packaging before treating it as missing. |

Search authorship, source links, earlier Bellatrix history and actual code when
classifying other shared contributions. Similar subject lines alone do not
establish equivalence. Older statements in `docs/upstream-candidates.md` about
what upstream lacks describe their dated baseline; update them after the
migration is validated.

# Required reconciliation

## 1. Exec, scheduler, context and CPU accounting — boot blocker

[`ba01955a0b`](https://github.com/aros-development-team/AROS/commit/ba01955a0b)
moves pending scheduling state into Kickstart-compatible `SysFlags`, updates
`ExitIntr()`'s calling convention, masks interrupts for scheduling decisions,
and routes dispatch through `ex_LaunchPoint`.

Bellatrix overrides Dispatch/Switch, prepares its own 68-byte frame, and has
its own platform interrupt entry. Port the behavior to these paths; preserve
the format/vector word, voluntary switching, Wait/wakeup, signal delivery,
task launch hooks and CPU accounting. Review `0002` and `0010` alongside the
generic scheduler and assembly includes. Verify every local producer and
consumer of reschedule flags and every call/return through ExitIntr.

**Confirmed source incompatibility:** HEAD's `rom/exec/etask.h` excludes
`iet_private1`, `iet_private2`, `iet_LastBusy` and `iet_LastUsageStamp` on m68k.
Bellatrix's `kernel/cpuusage.c` reads/writes `iet_private2` and `iet_LastBusy`;
patch `0093` adds instruction counters near the changed fields. This must be
resolved before the port can be considered buildable. Retain the required
fields under an appropriate port capability or replace them with an explicit
accounting design; keep `0094`'s query tags consistent. Do not simply delete
the accounting code to make the build pass.

Related context changes: `7c59bc3f94` right-sizes KrnCreateContext allocations,
`38dc95623a` reuses the saved context for alert data, and `0162d11a35` introduces
a sparse m68k exception-handler list. Check actual `AROSCPUContext`/ExceptionContext
allocation, alert storage, FPU flags and the handlers selected by the port.
Respect `765871daef` stack alignment and `a473d5fc64` RunCommand stack sizing.
Do not indiscriminately apply Amiga-specific stack reductions (`b76276f7dc`)
to this port or undo `0005`'s larger default without measured justification.
Include OpenLibrary version handling (`6a327488b5`), semaphore error handling
(`995911ef6b`) and reset-chain changes in the compatibility review.

## 2. Heap, pools and runtime synchronization

Absorb `6aa1da1cde`'s O(1) TLSF validation and `d11990b395`/`03a0533795`'s pool
growth/handle changes while preserving the semantics of local patches `0007`,
`0011`, `0045`, `0055`, guard bytes and mungwall diagnostics. A patch that
still applies is not proof that upstream replaced it.

**Confirmed retained fix:** HEAD's m68k `__m68k_sync_lock_test_and_set()` in
`compiler/pthread/pthread.c` still returns the new value. Local patch `0057`
corrects it to return the old value and still applies individually. Keep it;
the new pthread lifecycle work does not replace this fix.

Absorb the pthread exit/init, cancellation, detach/join and signal fixes
(`d62046e69d`, `c31d3294fb`, `9556dc40d4`, `9ac6dce158`, `b3951a2e03`,
`2bcbac322e`), timed-condition clock handling (`0f9874ae23`) and C++ header
fixes. Include POSIX library lifecycle, openat/filesystem functions, large-file
support and module destructor ordering (`728230df6f`). Review `0058`'s larger
malloc puddles against the improved allocator rather than deleting it by title.

## 3. FAT and storage integrity

Remove redundant FAT patches only after full semantic comparison. Preserve
upstream's subsequent bounds/error handling: failed dirty ranges remain
retryable (`d1c5351401`), ACTION_FLUSH (`86894ee09b`), rename failure preserves
the source (`5314b8a312`), incomplete device transfers fail (`545303c6f8`),
resize recovery (`fe81ecb950`), checked allocation/linking (`002c30bcf6`),
and metadata failures stop destructive follow-up (`647e2472ab`, `c0e0af6d2f`).
Also include DE_MAXTRANSFER, dirty-volume validation, requester suppression
and long-name lookup improvements.

Reconcile `0037`'s fixed cache sizing with `efbf24238c`'s volume-scaled policy.
Retain the DMA-alignment requirement from `0038` in the resulting policy.
Review generic sdcard `0003` separately: it still applies at this HEAD.
Audit all three local SD backends; Pi 5 SD/SDIO changes do not automatically
improve the Pi 3 implementations. Include partition buffer defaults,
storage.hidd exact capacity (`59148a2654`), and relevant AFS/SFS/CDVDFS fixes
if those handlers are shipped. Avoid enabling new disk controllers solely
because they exist in the updated upstream tree.

## 4. USB/Poseidon and controller input

The active adopted engine is local `soc/usb/usb2otg/` (metatarget
`kernel-usb-arosotg`); the separate `dwc2emu68` rewrite is not interchangeable.
Compare local files with upstream `arch/arm-native/.../usb2otg`, then port the
missing changes: NAK backoff (`c600e04074`), SOF gating (`ce1d81a271`), timer
IRQ wakeup (`2b1739fe22`), direct bulk-IN liveness (`6a46956e60`) and truthful
isochronous capability reporting (`5efc6ead54`). Do not keep the local build
comment claiming all four transfer types if the audited capability disagrees.

The SOF wake uses system timer channel 1; check IRQ registration, ownership,
acknowledgement and little-endian accesses against the local channel-3
heartbeat. Do not import native ARM assembly or SMP worker assumptions.

Review Poseidon HID takeover/bootmouse wheel (`555fa3aeab`), USB mass-storage
mount/optical handling and USB Ethernet close/binding fixes. Include
`controller.hidd`/lowlevel input integration (`72a773f2de`), with capability
checks that keep chipset-only paths out of native Emu68 operation.

## 5. Bluetooth transport, firmware and stack

Compare the Bellatrix-origin transport with its later upstream version.
`73eba4ee38` follows the live device tree, avoids claiming a console PL011,
enables RTS/CTS only for the actual pin assignment, allocates a ready signal
and addresses unit lifetime. `ee2e5812c1` gets BT_REG_EN from shutdown-gpios.
Carry applicable fixes into the local modules; keep m68k MMIO and IRQ adaptations.

Retain stack improvements including server classes (`8ff80b6627`), peripheral/
GATT support (`86aab939c4`) and the follow-up review fixes (`32ee68ce33`).
Check USB HCI error rearming and asynchronous commands separately from UART.
Check Broadcom firmware selection, address initialization and actual output
files/licenses, including `e2cb3acaac`; local Pi 3 packaging already exists.
Do not install every upstream firmware loader merely to satisfy a broad
metatarget. Audit metatarget names for collisions with the injected modules.

## 6. Network, Wi-Fi and startup ownership

Retain DHCP on multiple/late interfaces (`2038afc0d6`), lease release on link
loss (`328a617db4`), Wi-Fi telemetry/filtering (`b1a877810c`), per-interface DNS
ownership (`aab4d10135`, `7b9b93dcb1`) and IPv6 DNS (`4e59976285`). Review
`0082` startup sequencing and `0087` GUI wakeup against updated managers;
preserve one clear owner for stack startup.

Compare the local Wi-Fi/SDIO copies as well as shared `bwfm.device`. Include
DHCP stack-buffer reduction (`b987ec2197`). The 64-bit fragment reassembly fix
`4803a70fac` remains in shared upstream but is not evidence that this 32-bit
m68k port suffered that exact failure. Envoy and authentication additions need
packaging/build review if selected by workbench-complete; they need not become
new services enabled by default as part of this migration.

## 7. Mesa 26, VC4 and generic graphics — completion blocker

HEAD defaults `OPT_MESAGL` to 26.0.0. Patches `0061`–`0067`, `0070` and `0071`
modify the Mesa 20.0.8 patch file: successful application to that file does
not mean the active Mesa 26 build receives the fix.

Target the final Mesa 26 integration, including local VC4 glue/build rules;
do not call a migration complete by leaving GL disabled or silently retaining
Mesa 20. Audit ralloc alignment, realloc preservation, big-endian array/pixel
formats, hash/CSO layout, varargs, runtime bases and generated imports against
the active version. Retire only fixes actually superseded by Mesa 26.

**Confirmed retained requirement:** HEAD's `galliumglue.py` still accepts only
arm/aarch64. Local `0036` adds m68k trampolines and still applies individually.
Keep that support and verify regenerated tables, calling convention, ABI hash
inputs and symbol exports (`0069`, `0079`) against Mesa 26.

Include glapi TLS handling (`ae01ce7550`), cross-module hash-set sentinel
(`cdf6ed49fc`), removal of fast-math (`fbbc302a1f`) and missing 64-bit atomics
handling on 32-bit targets (`7756f31db8`). The ppc endianness change alone does
not validate m68k. Verify softpipe and accelerated VC4 separately; llvmpipe,
Rust/NAK and unrelated GPU drivers must be gated by actual target capability.

Compare local VC4 ownership/synchronization with upstream service handoff
(`69129aeab0`), task teardown (`99f1bf1376`), displaced-page lifetime
(`a38dd91547`) and NOWAIT presentation (`10dd77dc47`). Local scanout paths
already contain latch-aware code; compare behavior before importing changes.

Review generic graphics/intuition ABI-width fixes (`93a688b4d8`, `2cb305e297`,
`724a9e700f`), software pointer reshow (`096616b657`), opaque-destination alpha
(`6de49fedc9`), CopyBoxAlpha/compositor changes (`8a16cdb769`) and raw/interleaved
bitmap updates. Revisit local `0090`/`0091` AmigaVideo patches against upstream
beam/vblank evolution without importing chipset assumptions into emu68gfx/vcgfx.

## 8. DMA, mailbox and AHI

Compare local DMA and `hdmiaudio`/`i2saudio`/`pwmaudio` with their upstream
sources. `ec982ac781` changes RPiHDMI's DMA path and hardware interface;
`3ecd43bdfa` changes dma.resource. Port relevant interface fixes together,
preserving local byte order, address conversion and memory barriers. Check
mailbox message alignment (`8397060fe5`), firmware clock queries and cache
maintenance. New Pi 5 addresses are future-platform work, not Pi 3 defaults.
Validate the AHI frequency-query contract documented by `acd9a5fe2a` against
the local driver. `d0d1e15cb0` enabling audio.device on AArch64 does not by
itself enable sound through this m68k port's AHI drivers.

## 9. Loaders, build system, diagnostics and distribution

Include CAMD driver arena-hunk scanning (`cb8c4c5f39`) and va_list cluster
formatting (`28ec43a517`), generic ELF empty-relocation/SHN_XINDEX handling
(`7a9809157b`, `afdf0b7619`), weak undefined symbols (`ce4c1f46ac`) and target-nm
selection (`732a1f0f55`). AArch64 ADRP range checks are shared-source changes,
not a reason to treat the m68k ELF as AArch64.

Review `0001`, `0026`, `0028`, configure/configure.in consistency, archspecific
target IDs (`ba0aed9074`), response-file linking (`3cb1f7aa8f`, `d4a19031d6`),
genmodule hardening and per-task base lifetime (`f83161c21e`). Check SDK headers,
generated includes and toolchain cache inputs before restoring cached tools.

Include debug module lifetime (`dcb694bf68`), fail-safe registration
(`1c25bf6c16`) and kernel symbol lookup (`17f488d158`) while preserving the
local frame backtrace, debug channel and heap inspection tools. Review DOS
buffered I/O, boot ownership, resident locking, shell/console bounds and GUI
changes for shipped components. A full distribution build must actually stage
the new disk modules; relinking only the resident ELF is insufficient.

## 10. SMP and additional boards: preserve boundaries

AArch64 atomics (`33fb2a3300`), SMP (`95f5d15b77`), StackSwap, EClock/IPIs,
secondary-core startup, task affinity inheritance (`b46e7c4c9d`), NP_Affinity
(`f8b9366332`) and SetTaskAffinity (`0a562d1363`) are valuable references.
They do not automatically make the m68k guest multicore-safe. Do not enable
an SMP macro merely because the host CPU is AArch64; separately define guest
atomics, shared state, scheduling and interrupt ownership for Emu68/Musashi.
Keep the migration focused on current supported boards. Pi 4/5 GIC, RP1,
PCIe, SDIO, VC6/VC7 and display work are subsequent port projects.

# Patch audit and execution workflow

The audit checked all 70 numbered patch files individually against pristine
HEAD, including plain unified diffs as well as git-format patches. Of these,
43 pass `git apply --check` and 27 require reconciliation. These are syntax/
context results, not a sequential-series test or proof of semantic correctness.
Some failures are expected dependencies on preceding patches, especially Mesa
follow-ups and Startup-Sequence. Binary artwork also needs separate handling.

The non-applying patch identifiers were:
`0001`, `0002`, `0006`, `0008`, `0010`, `0023`, `0028`, `0037`, `0038`, `0039`,
`0052`, `0053`, `0056`, `0062`–`0067`, `0070`, `0071`, `0079`, `0080`, `0082`,
`0085`, `0090`, `0093`.

There are two `0041` filenames; preserve the actual filename ordering used by
setup. Do not assume the numeric prefix is a unique patch identifier.

1. **Snapshot and isolate.** Record the parent commit, both gitlinks, complete
   patch ordering and injected-tree state. Use an isolated checkout/worktree
   for the migration. Inspect existing submodule working trees with the
   repository's verification mechanism; do not reset a working development tree.
2. **Create the semantic ledger.** Give every numbered patch an explicit
   outcome: retained, refreshed, superseded with evidence, or intentionally
   removed with rationale. Map each source copy to its upstream counterpart
   and record which later changes are present locally, missing or inapplicable.
3. **Select the full target SHA and reconcile the series in order.** Remove
   proven-redundant FAT changes; refresh configure and architecture integration.
   Resolve Exec/accounting/context incompatibilities and retained allocator/
   pthread/glue fixes. Use git apply checks on the accumulated state. Refresh
   authoritative patch files in the isolated migration, never an already-applied
   series in a live development tree. Keep port-specific work in the project
   tree and shared edits in numbered patches, following AGENTS.md.
4. **Reconcile driver copies and Mesa 26.** Address USB, BT, network, SD, DMA,
   AHI and graphics interfaces. Stages may be validated separately, but the
   final update must include the working selected accelerated graphics path.
5. **Build and validate.** Run setup verification, then serial builds of
   affected modules and `kernel-link-emu68-m68k`. Check the toolchain key and
   configure changes before cache reuse. A full distribution build is justified
   for the final migration because libraries, classes and startup packaging
   change; stage a fresh image from that distribution rather than old disk files.
6. **Complete documentation and commit.** Update the pin, patch ledger,
   docs/aros.md, affected driver READMEs and outdated upstream-candidate claims.
   Record exact compiler/toolchain, image source, logs, completed commands and
   hardware limitations. Keep external/emu68 at its existing pin unless a
   separately identified dependency requires changing it.

# Acceptance checklist

- [ ] Recorded target SHA; additional commits since this audit reviewed.
- [ ] All 70 patches have documented semantic outcomes; the complete series
      and injected tree pass `./scripts/setup.sh --verify` on the new pin.
- [ ] Exec frame/ExitIntr/flag/launch contracts checked; preemption, voluntary
      switching, Wait/Signal and task lifecycle work without stack overruns.
- [ ] CPU-time and instruction-share measurements still update correctly;
      alerts and backtraces remain usable with the new context layout.
- [ ] Allocator stress covers split/coalescing, many puddles and rejected
      invalid frees; m68k pthread spinlocks return/acquire correctly; lifecycle
      and timed waits are exercised.
- [ ] FAT create/read/overwrite/resize/rename/date/flush and long-name paths
      verified; big-endian disk data inspected and failures do not discard
      retryable dirty state. DMA cache alignment is preserved.
- [ ] Real Pi USB keyboard/mouse under CPU/GL load and bulk transfers checked;
      interrupt load and timeouts compared; advertised capabilities are accurate.
- [ ] Real Pi BT firmware/address, scan, pair, reconnect and enabled GATT roles
      checked; transport startup/failure/close does not outlive freed state.
- [ ] Wi-Fi startup/link loss/DHCP/DNS/reconnect tested for shipped transports.
- [ ] Mesa 26 softpipe and VC4 tested independently: repeated GL open/close,
      rendering/colors/alpha, buffer lifetime, screen switching and input under load.
- [ ] HDMI/AHI playback works with correct rate reporting and teardown; other
      selected audio backends checked where hardware is available.
- [ ] Fresh distribution reaches Wanderer; resident and disk modules are all
      from the recorded updated build, with no duplicate metatarget ownership.
- [ ] Existing compatibility smoke programs exercise library vectors, raw/
      planar bitmaps, pointer paths, hooks and representative legacy binaries.
- [ ] QEMU checks distinguished from hardware results. The documented QEMU
      SD-card profile omits blocking radio startup; a QEMU desktop alone does
      not validate Bluetooth, Wi-Fi, HDMI audio or physical USB timing.

No build, boot, complete sequential rebase or hardware validation was performed
for this research document. The checks above are requirements for execution.
