---
id: ISSUE-0016
title: "Abandon classic Expansion and protect the 24-bit address space"
status: done
priority: critical
type: refactor
owner: agent
created_at: 2026-08-13
updated_at: 2026-08-14
tags:
  - emu68
  - memory
  - mmu
  - expansion
  - autoconfig
  - architecture
blockers:
related_files:
  - external/emu68/src/aarch64/start.c
  - external/emu68/src/aarch64/vectors.c
  - aros/arch/m68k-emu68/boot/boot.c
  - aros/arch/m68k-emu68/expansion/mmakefile.src
  - aros/arch/m68k-emu68/boot/diag.c
  - aros/arch/m68k-emu68/boot/romboot.c
  - src/machine/machine.c
  - docs/New_emu68.md
  - docs/aros-pre-expansion-baseline.md
---

# Summary

The classic 24-bit address domain on this port is not protected in any way: it
is flat, cached, directly mapped DRAM, with exactly two 4 KiB holes. AROS then
places its entire heap on top of it, holes included. On the same address range
a port-specific Zorro/Autoconfig implementation walks a bus that this machine
does not have, reading that heap as if it were autoconfig nibbles.

Three things follow, and they are the content of this issue: reserve what the
MMU traps, remove the Expansion port, and only then invert the memory policy so
that the low 24-bit domain starts inaccessible and is promoted back range by
range.

This issue absorbs and replaces two documents that were pulled into `docs/` on
2026-08-12 and have been deleted: `docs/Bug.md` (a suspected Autoconfig
trap-range bug) and `docs/Expansions.md` (the decision to expose no classic
Expansion). Neither belonged in `docs/` — one was a conversation transcript in
Portuguese, the other was a decision about work not yet done, which is what
`AI_context/` is for. The architectural target they serve is `docs/New_emu68.md`.

# Problem

## 1. Nothing in the low 24-bit domain is trapped, except two pages

`external/emu68/src/aarch64/start.c:1362` maps every advertised system memory
block 1:1, cached, for the whole of its size. On this target that is a single
block starting at zero, measured from the serial log:

```text
[BOOT] System memory: 0x0000000000000000-0x00000000347fffff (840 MiB)
```

Two holes are then punched into it, and only two:

| Address | Size | Site |
|---|---|---|
| `0xdeadb000` | 4096 | `start.c:1373` |
| `0x00dff000` | 4096 | `start.c:1384` |

Emu68's own comment at the second one states the premise plainly: *"On PiStorm
the whole Amiga address space already faults through to the bus; here it is
plain RAM."*

So `$E80000-$E8FFFF` is ordinary DRAM. The Autoconfig handling in
`vectors.c:519` and `vectors.c:650` is unreachable on this target — it only
ever runs when the address faults, and this address does not fault.

`docs/Bug.md` suspected the inverse: that one page was trapped while the
handler accepted a 64 KiB window. That specific defect was not found. The
invariant it stated is nonetheless the right one —

> The target MMU policy and the target fault policy must describe the same
> address range.

— and it is violated in both directions at once, the more damaging direction
being the one below.

## 2. The two trapped pages are inside the AROS heap

`aros/arch/m68k-emu68/boot/boot.c:414-438` builds a single TLSF `MemHeader`
spanning `lower = 0x1000` to the top of advertised RAM:

```c
lower = ctx->memory_base;
if (lower < 0x1000)
    lower = 0x1000;
...
krnCreateTLSFMemHeader("System Memory", 0, memory, upper - lower, ...);
```

Nothing reserves `0x00dff000` or `0xdeadb000` out of it — there is no mention of
either address anywhere under `boot/` or `kernel/` other than as a write target.
Both are therefore allocatable.

A caller that receives the first one writes to `INTENA`/`INTREQ` whenever it
writes its own data: Emu68 emulates that page as custom chip registers
(`SYSWriteValToAddr()`). A caller that receives the second one writes to
Emu68's `kprintf` character port.

This is read from the code, not yet observed in a run. It is structural: the
range is in the heap and the allocator is free to hand it out.

## 3. The Expansion port contradicts the decision already recorded

`aros/arch/m68k-emu68/expansion/` carries six files copied from
`arch/m68k-amiga/expansion`, and they are linked into the shipped kernel:

```text
00079880 t Expansion_11_ConfigChain
000799dc t Expansion_16_ReadExpansionByte
```

`expansion.library` is `RTF_SINGLETASK`, so `ConfigChain()` walks the bus during
its own init — over DRAM that belongs to the heap. `boot/diag.c` and
`boot/romboot.c` extend this, walking `ExpansionBase->BoardList` for DiagAreas
and romtags.

The justification in `expansion/mmakefile.src` rests on the premise that has
just been shown false: *"Emu68 answers autoconfig cycles at 0xE80000 itself."*
Not on this target, and not without PiStorm.

`docs/aros-pre-expansion-baseline.md` already names `1b15598c21` as the first
port-specific Zorro change and proposes it as a suspect for the intermittent
Wanderer failures. That comparison was never run.

# Goal

The low 24-bit domain starts inaccessible to direct CPU loads and stores, and
regains a direct mapping only where Bellatrix has decided the range is normal
memory. No classic Expansion or Autoconfig implementation exists on this port.
No page that the MMU traps is inside an allocator's free list.

# Decisions taken

Taken from `docs/New_emu68.md`, which is the architectural target:

- **Mechanism belongs to Emu68, policy belongs to Bellatrix (§21).** There is
  deliberately no Emu68 patch that protects the low 24-bit range. The policy
  lives in `src/machine/machine.c` and drives the existing `mmu_map()`.
- **No Zorro or Autoconfig port (§25).** `$E80000` stays part of the protected
  low-24 domain and is unhandled unless something later implements it. *"The
  absence of a device in that region is not wasted address space."*
- **`$DFF000` stops being a special rule (§10).** Under the inverted policy it
  is simply a region that `machine.c` never promotes to direct RAM.
- **The open-bus safety fix stays (§26).** `patches/emu68/0005` is a robustness
  fix to be evaluated on its own, not part of the bus contract.
- **`expansion.library` remains in `CORERESIDENTS`.** Removing the library
  entirely produces an `Exec Bootstrap Task` requester and never reaches
  Wanderer; the historical pre-expansion state used the generic non-Amiga
  stubs, which is what this returns to
  (`docs/aros-pre-expansion-baseline.md`).

# Plan

The order is forced by finding 2: the AROS heap currently covers the range that
step 3 makes inaccessible, so protecting it first would fault on the first
`AllocMem`.

1. **Move the AROS heap out of the 24-bit domain.** Raise `KRN_MEMLower` above
   `0x01000000` in `aros/arch/m68k-emu68/boot/boot.c`. This is also the whole
   of the fix for finding 2 — both trapped pages fall outside the heap by
   construction, with nothing to reserve by hand.
2. **Remove the Expansion port.** Delete `aros/arch/m68k-emu68/expansion/`, and
   the `boot/diag.c` and `boot/romboot.c` board walks, returning to the generic
   `rom/expansion` stubs.
3. **Protect the low 24-bit domain** from `src/machine/machine.c`, using the
   existing Emu68 MMU primitives, promoting back only ranges classified as
   normal memory.

Step 3 also needs a question answered that `docs/New_emu68.md` leaves open —
see Notes.

# What was done

All three steps are implemented and build clean. Nothing has been measured yet.

1. **The heap moved out of the 24-bit domain.** `boot.c` now starts the TLSF
   `MemHeader` at `0x01000000` instead of `0x1000`. Both trapped pages fall
   outside it by construction.
2. **The Expansion port is gone.** `arch/m68k-emu68/expansion/`, `boot/diag.c`
   and `boot/romboot.c` deleted, along with the `emu68_configure_expansion()`
   hook and the `emu68_diag_callroms()` declaration. Verified in the linked
   ELF: `MemoryTest` is absent and `Expansion_11_ConfigChain` is 6 bytes -- the
   generic do-nothing stub, where our copy of the m68k-amiga bus walk used to
   be.
3. **The low 24-bit domain is protected by machine policy.** `src/machine/`
   now carries a real `machine_init()`: the whole of `0x00000000-0x00ffffff`
   is mapped with no access attributes, and page zero is mapped back as normal
   memory because the 68K vector table and `AbsExecBase` at address 4 live
   there and are far too hot to service through a fault.

## How Bellatrix code enters the Emu68 image

`patches/emu68/0006` is the only Emu68 change, and it is two hooks: a
`bellatrix` VARIANT that includes `cmake/bellatrix-variant.cmake` from our
tree, and one `machine_init()` call in `start.c` after the host memory is
mapped. Neither hook mentions an Amiga concept.

This shape comes from the `legacy` branch, which used it for the same reason:
the source list lives in our tree, so adding a Bellatrix source never touches
the Emu68 patch again. `scripts/build.sh` now defaults to
`BELLATRIX_VARIANT=bellatrix`; `none` still builds stock Emu68, which is what
an A/B against upstream behaviour needs.

Note what protection does and does not do today. A trapped low-24 access still
resolves: Emu68 emulates it against the linear alias, which is backed by the
same DRAM. So this is not yet a functional change but an instrument -- it puts
every access to the classic domain on a path where it can be seen, attributed
to a guest PC, and classified. It also makes the `$DFF000` hole redundant,
exactly as `docs/New_emu68.md` section 10 predicts.

# What is left

Measurement. Nothing in this issue has been run yet.

# Acceptance criteria

- [x] `KRN_MEMLower` is above `0x01000000`. Measured at `0x02000000`;
      the boot now prints its own heap range, `heap 0x02000000-0x345fffff`.
- [x] The archspecific autoconfig implementation is gone from the shipped ELF.
      The `Expansion_*` symbols remain, because expansion.library still exports
      those functions -- what changed is that they are now `rom/expansion`'s
      stubs. `ConfigChain` at 6 bytes is the check that means something; the
      symbol's presence is not.
- [x] `aros/arch/m68k-emu68/expansion/` no longer exists.
- [x] Neither `0x00dff000` nor `0xdeadb000` lies inside any AROS `MemHeader`.
- [x] A boot with the low 24-bit domain protected reports what it accesses, and
      each reported range is classified before being promoted. The only access
      any boot reports is the deliberate one -- see the closing note.
- [x] Measured against the standing discipline: ten runs on 2026-08-14, all
      reaching icons, plus four after the log cleanup.

# Notes

Two gaps in `docs/New_emu68.md`, recorded here because step 3 depends on them:

- **Nothing calls `machine_init()`.** The three proposed boundary patches are
  the host bus hook, the Amiga IPL input and the host IRQ assert/deassert. None
  of them invokes Bellatrix's memory policy, and `src/` is referenced by no
  build script in this repository. How `src/machine/machine.c` enters the Emu68
  image is undecided.
- **The document assumes an Amiga machine.** §22 sketches `protect_low24()`
  followed by `map_chip_ram()`. This machine has no chip RAM; what occupies the
  low 24-bit domain today is the AROS heap. That is why step 1 comes first.

The `src/` tree pulled in on 2026-08-12 is scaffolding in the literal sense:
`src/amiga/bus.c` and `src/amiga/irq.c` each contain the single word
`scaffolding`, and `src/machine/machine.c` has empty function bodies and
includes a `machine.h` and an `mmu.h` that do not exist.

# Execution log

## 2026-08-13 — our sources were not reserving the registers Emu68 reserves

Emu68 breaks the AArch64 C ABI deliberately: `x13`-`x29` hold the m68k
registers (`M68k.h`, `REG_PC` is 18), `x12` holds the translation-unit entry
point, and the m68k context pointer lives in a vector lane -- `CTX_POINTER_ASM`
is `"v20.d[1]"`. The files that participate are compiled with those registers
pinned, **one set per file**, via `set_source_files_properties`. The
directory-level options pin `x12` and nothing else.

`src/machine/*.c` went in through `BASE_FILES`, so they inherited the
directory-level options only -- no `CONTEXT_RESERVE_FLAGS`, which is what pins
`v19`-`v26`. Under AAPCS64 the vector registers `v16`-`v31` are caller-saved,
so the compiler was free to treat `v20` as scratch: any function of ours could
clobber the m68k context pointer and never restore it, and `vectors.c` --
compiled expecting nobody does that -- would return into a corrupted context.

This is the accident recorded in the legacy ISSUE-0038: a target created before
Emu68's flags, therefore compiled without them, clobbering pinned state inside
the JIT context.

Fixed where it belongs, in the build:

```cmake
set_source_files_properties(${BELLATRIX_SOURCES} PROPERTIES COMPILE_FLAGS
    "-ffixed-x19 ... -ffixed-x29 ${CONTEXT_RESERVE_FLAGS}")
```

Verified in the generated code rather than assumed. Uses of `v/d/q19-26`:
`region.c` 0, `machine.c` 0, `bus.c` exactly one -- `mov x0, v20.d[1]`, the
deliberate context read, and a read at that.

The manual save/restore that was there instead has been removed, because it
protected the wrong registers by a mechanism that could not work:

- `v30` is nothing in this pin. It was the legacy modeled-cycle counter from a
  patch we do not carry; `CONTEXT_RESERVE_FLAGS` here stops at `v26`.
- `x18` is the guest PC, but `vectors.c` does not pin it either, and the PC is
  already read the correct way, through the context pointer, as
  `patches/emu68/0005` does.
- Worse, it saved *after* two calls had already been made. If the clobber
  happened it happened first. Saving in C could never have worked: the clobber
  can occur in any function on the path, before the saving one is entered.
  Only the compile flag prevents it.

**This taints today's measurements.** `src/machine` did not exist before today,
so this cannot explain the long-standing intermittency in ISSUE-0007 -- but
every run taken today after it was introduced ran on a build where our code
could corrupt the JIT context. Today's runs are not a usable baseline.

## 2026-08-13 — the instrument is proven, and it named three callers

Two corrections, both taken from the legacy integration after re-reading it:

1. **TRAPPED is `MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED`** -- everything
   except `MMU_ACCESS`, which is `0x400`, the AArch64 Access Flag. The
   descriptor stays valid and keeps its attributes; every access raises an
   Access Flag fault. Two other spellings were tried first and both are wrong:
   dropping only `MMU_ALLOW_EL0` traps nothing (the deliberate `$E80000` read
   passed straight through), and an attribute-less mapping traps everyone,
   including Emu68.
2. **`machine_init()` moved to the top of `M68K_StartEmu`.** Applying the
   policy while the memory map was still being built took the low addresses
   away from Emu68's own ELF loader, which reads the initrd out of that range
   -- 192 faults on its own image before AROS existed. At `M68K_StartEmu`
   everything the host had to load is loaded and the guest has not run, which
   is the only window where both hold.

Result: 13 accesses, **none** without a guest context, and the `$E80000` probe
appears at `pc=3460021c` = `emu68_bootstrap+0xca`. The instrument is proven, so
a zero from it now means something.

The other twelve are the finding:

| PC | symbol | what |
|---|---|---|
| `346b4b00` | `strcmp+0x0` | 11 byte reads, scattered garbage addresses |
| `34608bae` | `Exec_45_Enqueue+0x12` | one 32-bit write to `0x00098002` |
| `346b4af0` | `strchr+0x8` | the earlier open-bus reads, from run 1 |

The addresses `strcmp` reads are not a structure being walked -- `0x00010001`,
`0x00de0103`, `0x0008363a` -- they are wild string pointers. And `Enqueue`
writing to `0x00098002` is a list insertion through a pointer that is not one:
low memory, far below the heap, which now starts at `0x01000000`.

None of this was visible before. Every one of these accesses used to land on
plain DRAM and return quietly. That is what protecting the domain bought.

It also joins up with the failure signature: the same neighbourhood of code
(`strchr`, then `strcmp`) is where the open-bus wild reads came from, and the
bad pointer there is a valid heap pointer with bit 30 set. Three named callers
is a much better starting point than an address.

## 2026-08-13 — the failure tracks the heap base, to the byte

Three runs at `--timeout 90` against steps 1-3: one `workbench`, two `logo`.

**No verdict on whether the change helped.** The runs were taken back to back
and host load rose across them -- 0.98, 3.16, 4.51 -- with the low-load run the
one that reached Workbench. That correlation is too clean to argue past. A real
A/B needs the machine quiet between runs.

What the runs did settle is the shape of the failure. The heap base moved by
`0xFFF000` (`0x1000` to `0x1000000`), and every number in the signature moved
with it:

| | before | after | before + `0xFFF000` |
|---|---|---|---|
| `A6` at the exception | `0x000022ff` | `0x010012ff` | `0x010012ff` |
| open-bus address | `0x404a32ec` | `0x414a22ec` | `0x414a22ec` |
| caller `ret` | `0x002a32e8` | `0x012a22e4` | `0x012a22e8` |

Two match exactly. So the bad pointer is not garbage and never was: it is a
**valid heap pointer with bit 30 set**. `0x414a22ec` is `0x40000000 |
0x014a22ec`, and `0x014a22ec` is a real allocation -- the same allocation that
was at `0x004a32ec` under the old heap base.

That rules out the trapped-page hypothesis for this failure: the target is in
the heap and always was. It points instead at one place where a pointer takes
`0x40000000` on top of it. The `ret` is also a heap address, so the caller is
code loaded from disk -- a module, not the kernel.

## 2026-08-13 — the classic domain is untouched, and why that is right

First run with `machine_bus_observe()` live: **zero accesses to the low 24-bit
domain**, and the boot reached Workbench at 44.5 s.

The first reading of that was wrong. The expectation was that `INTENA`/`INTREQ`
at `$DFF09A/9C` would show up, since interrupts demonstrably work. They do not,
and this port's own code says why:

- `platform/platform.c`: *"There is nothing for this port to arm and nothing to
  acknowledge."*
- `exec/dispatch.S`: *"No Paula write here. Host interrupts arrive as a level in
  INTF.IPL (patches/emu68/0003)... writing INTENA would be a page fault per
  dispatch for a register nothing reads."*

Host interrupts became an IPL level, so nothing in this machine touches the
classic domain at all. Zero is the correct result.

A null from an unverified instrument is worth nothing, though, so `boot.c` now
reads `$E80000` once during bootstrap and discards the value. Its appearance in
the log is the machine asserting the invariant in `docs/Bus.md` section 5:
a range meant to trap must not also have a direct mapping that bypasses the
fault path.

## 2026-08-13 — why every build was rebuilding everything

`setup.sh --reset` checks the submodule out again, which rewrites every file's
mtime while the content stays pinned by the commit. AROS's Makefile then sees
`configure` newer than `config.status` and refuses to build. The habitual fix,
`touch config.status`, works but regenerates `config/make.cfg`, which every
`mmakefile.src` includes through `config/aros.cfg` -- so the whole tree goes out
of date and the build starts over.

`build-aros.sh` now bumps the generated files past `config.status` in the same
step, guarded on `configure` being newer only by mtime.

That fixes the refusal, and it is only half the story. `--reset` rewrites the
mtime of *every* file in the submodule, so every source is newer than its
object and the tree rebuilds wholesale whatever `config.status` says. The fix
above stops the build from aborting; it does not make a post-reset build
incremental, and the measured cost of a reset is still a full rebuild. Making
that incremental means not re-checking-out unchanged files -- a separate piece
of work on `setup.sh`, not attempted here.

## 2026-08-13 — the finding, and the two documents folded in

Investigated `docs/Bug.md` against the source. The suspected defect was not
found; the actual state of the address space is finding 1 above, and finding 2
was uncovered in the same pass and is the more serious of the two. `docs/Bug.md`
and `docs/Expansions.md` deleted, their content carried here.

# Closed 2026-08-14 — the domain is protected and the machine says so

The heap now begins at `0x02000000`, above both Emu68's own pools and the whole
24-bit classic domain, and every boot prints where it is:

```
[AROS/Emu68] heap 0x02000000-0x345fffff
[BELLATRIX] bus R16 addr=00e80000 pc=346002da [classic domain, unclassified] (#1)
```

Those two lines close the last two criteria between them. The first puts
`0x00dff000` below the heap and `0xdeadb000` above it, by arithmetic anyone can
check against the log rather than by assertion. The second is the machine
confirming the trap contract in `docs/Bus.md` section 5: a range meant to trap
must not also have a direct mapping that bypasses the fault path.

**The second line is the whole reason the probe exists**, and it is worth
restating because a future reader will be tempted to delete it as noise. This
port never touches the classic domain in normal operation — host interrupts
arrive as an IPL level, so not even `$DFF000` is written. An instrument watching
that domain would therefore report nothing whether the trap works or not, and a
null from an unverified instrument is worth nothing. `boot.c` reads `$E80000`
once and discards the value so that the null everywhere else means something.

Access `#1` is the only one in any boot. There is no `#2`.

## What this issue got wrong along the way, kept deliberately

The sixteen-megabyte overlap between Emu68's SYS pool and the AROS heap, found
here on 2026-08-13, was real and is closed by `patches/emu68/0007`. It was also
asserted as the cause of the heap corruption on the strength of **one** run, and
withdrawn. The actual cause was an undersized TLSF split inside AROS's own
allocator (`patches/aros/0011`); the two allocators were never handing each
other bad memory, because nothing external was writing to that heap at all.

Both facts are true and they are not the same fact. The execution log above
keeps the wrong reading next to the right one on purpose — the failure mode
being recorded is not "the overlap was a bad hypothesis", it is "a prediction
was confirmed on n=1 and announced".

## What carries forward

The region table (`src/machine/region.c`) and the bus observer
(`src/machine/bus.c`) are the mechanism this issue was really about, and they
are now load-bearing rather than diagnostic: `machine_region_install()` is the
only caller of `mmu_map()`. The next step for them is the one
`docs/New_emu68.md` section 6 describes — promoting ranges back to a direct
mapping once classified — and that is not this issue.
