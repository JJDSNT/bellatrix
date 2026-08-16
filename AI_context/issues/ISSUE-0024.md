---
id: ISSUE-0024
title: "Clear the modular-kickstart BASE package of machine-specific code"
status: backlog
priority: medium
type: refactor
owner: agent
created_at: 2026-08-16
updated_at: 2026-08-16
tags:
  - aros
  - m68k
  - graphics
  - portability
  - upstream
  - kickstart
blockers:
  -
related_files:
  - external/aros/boot/modular_kickstart.txt
  - external/aros/rom/graphics/qblit.c
  - external/aros/rom/graphics/qbsblit.c
  - external/aros/rom/exec/chipset.h
  - external/aros/arch/m68k-amiga/graphics/mmakefile.src
  - external/aros/arch/m68k-amiga/graphics/vbeampos.c
  - external/aros/arch/m68k-amiga/hidd/gfx/mmakefile.src
  - docs/aros_port_contract.md
  - docs/New_emu68.md
  - AI_context/issues/ISSUE-0023.md
---

# Summary

AROS declares one of its kickstart packages portable and then names the ports
that break the rule. The note is fifteen years old and still accurate. Close it:
take the chipset out of `graphics.library`, and fix whatever else the same
inventory turns up on the way.

This is upstream work, not port work. It is opened here because
[`ISSUE-0023`](ISSUE-0023.md) is about putting *our* port on the right side of
the same boundary, and because one of the violations compiles into our own
build.

# Problem

`boot/modular_kickstart.txt` describes four kickstart packages. BASE is defined
as *"machine-independent modules […] designed to be fully portable and
independent of its environment. The same base package can run on both hosted and
native AROS."* It lists `graphics.library`, `gfx.hidd`, `intuition.library`,
`dos.library`, `oop.library`, `console.device` and the rest. Then, at
`modular_kickstart.txt:49-52`:

> These modules are not allowed to have machine-specific code because it
> violates portability convention. As of 12.09.2011, **m68k-amiga
> (chipset-specific code in graphics.library)** and PowerPC-native (display
> driver setup kludge in dos.library boot code) ports do not conform to this
> rule.

The same file draws the distinction that makes the rule workable: *"Of course
'machine-specific code' does not include 'cpu-specific code'. It's allowed for
any module to have relevant portions in `arch/<cpu>-all`."* Machine, not CPU, is
the axis.

Two mechanisms produce violations, and they need different fixes.

## A. Machine files injected into a BASE module

`%build_archspecific` compiles files from `arch/<cpu>-<machine>/` into a module
that lives in `rom/`. `m68k-amiga` uses it in eleven places; nine target BSP
modules (`exec`, `kernel`, `timer`, `battclock`, `expansion`, `ata`, `disk`,
`card`, `lowlevel`) and are legitimate by the same document. Two target BASE:

- `arch/m68k-amiga/graphics/mmakefile.src` → `maindir=rom/graphics`, injecting
  `vbeampos.c`, `coppersupport.c`, `bltclear.c`, `movesprite.c`,
  `changeextspritea.c`, `setchiprev.c`, `waitblit.S`, `locklayerrom.S`,
  `unlocklayerrom.S`, `attemptlocklayerrom.S`. This is the violation the note
  names. `arch/m68k-amiga/graphics/vbeampos.c:20` is the plainest example:
  `volatile struct Custom *custom = (struct Custom*)0xdff000;` inside
  `graphics.library`.
- `arch/m68k-amiga/hidd/gfx/mmakefile.src` → `maindir=rom/hidds/gfx`, injecting
  `rgbconv.c`. This one is *CPU*-specific code (colour conversion) sitting in a
  machine directory. Formally a violation, no runtime cost; it belongs in
  `m68k-all` and the note predates it.

## B. Chipset access in portable source, gated on the CPU

`rom/graphics/qblit.c:94` and `rom/graphics/qbsblit.c:91`, identical:

```c
#if (AROS_FLAVOUR & AROS_FLAVOUR_BINCOMPAT) && defined(mc68000)
    {
        /* Trigger blitter interrupt */
        volatile struct Custom *custom = (struct Custom *)(void **)0xdff000;
        custom->intreq = INTF_SETCLR | INTF_BLIT;
        custom->intena = INTF_SETCLR | INTF_BLIT;
    }
#endif
```

That is not in `arch/` at all — it is in the portable module, and the gate is
`mc68000`, which means *any* m68k, not *an Amiga*. Both conditions hold on
`m68k-emu68`:

- `out/build/aros/bin/emu68-m68k/gen/include/aros/config.h:21` →
  `AROS_FLAVOUR (AROS_FLAVOUR_STANDALONE|AROS_FLAVOUR_BINCOMPAT)`, from
  `configure.in:2245` (`emu68* → aros_flavour="standcompat"`)
- the cross compiler predefines bare `mc68000` (verified with
  `m68k-aros-gcc -dM -E`)

So `graphics.library` on our target contains stores to `0xdff000`.

AROS already has both correct patterns, in the same tree:

- **compile-time, gated on the machine**: `rom/exec/chipset.h:7`,
  `#ifdef AROS_ARCH_amiga`, with `CUSTOM_ENABLE`/`CUSTOM_DISABLE`/`CUSTOM_ACK`/
  `CUSTOM_CAUSE` defined away to nothing on every other machine. `CUSTOM_CAUSE`
  is *literally* the `intreq = INTF_SETCLR | bit` that `qblit.c` open-codes.
- **runtime probe**: `rom/dosboot/menu.c:555,889` gate on `mc68000` too, but
  then ask `OpenResource("ciaa.resource")` before touching anything, so the
  code is inert on a machine that has no chipset.

# What this costs us today

Nothing at runtime, and the issue should not claim otherwise.

The stores land on Emu68's emulated custom-register page, reach
`SYSWriteValToAddr` (`external/emu68/src/aarch64/vectors.c:441-466`), update
`INT_shadow.INTENA` / `INT_shadow.INTREQ`, and stop there: the shadow only
touches `ctx->INTF.ARM` when `INT_shadow.ARMPending` is set, and nothing sets it
any more. `patches/emu68/0002-deliver-host-interrupts-as-an-ipl-not-through-a-shadow.patch`
replaced that whole path with a direct `INTF.IPL` store in `curr_el_spx_irq`,
which is what `aros/arch/m68k-native/platform/platform.c:20-44` describes: *"There
is nothing for this port to arm and nothing to acknowledge."* The shadow code is
still in Emu68 and is the right answer once a machine really owns those
registers — it is simply not wired to anything here.

Reachability is also low: only `QBlit()` and `QBSBlit()` reach it, and nothing
queues blits without a chipset display driver.

**It stops being inert when we move to the New_emu68 model.**
[`docs/New_emu68.md`](../../docs/New_emu68.md) §14 deletes `INT_shadow` instead
of leaving it dormant, and routes `$DFF09A` through the generic bus hook to
Rigel, *"there must not be two independent owners of the same chipset state."*
At that point the two stores in `qblit.c` no longer land on a dead shadow: they
reach a chipset that really owns INTENA and INTREQ, from a module that is
supposed to be machine-independent, on a machine whose display driver is not the
one whose blitter interrupt is being caused. The window in which this is
harmless is the window we are currently in.

What it costs is exactly what `ISSUE-0023` is about. `boot/boot.c:450-465` goes
out of its way to keep the classic 24-bit domain out of the allocator *because*
those pages are devices, and then a portable module writes to one of them
anyway. The defect is that a module declared machine-independent names a
machine's device address; that it currently lands somewhere harmless is a
property of this machine and of one patch, not of the code.

# Goal

`modular_kickstart.txt` can drop the m68k-amiga clause because it stopped being
true.

Concretely: no module in the BASE package contains machine-specific code on any
target, whether by `%build_archspecific` injection or by a CPU-gated chipset
access in portable source.

# The inventory

The authoritative BASE list is `rom/mmakefile.src:139-145`, not the prose in
`modular_kickstart.txt`. Against it, every `%build_archspecific` in `arch/`
(212 of them, excluding `.unmaintained`) whose `maindir` is a BASE module:

| Injection | Into | Verdict |
|---|---|---|
| `arch/m68k-amiga/graphics` | `rom/graphics` | **violation** — the one the note names |
| `arch/m68k-amiga/hidd/gfx` | `rom/hidds/gfx` | **violation** — machine dir, nothing machine-specific in it |
| `arch/ppc-chrp/dosboot`, `arch/ppc-sam440/dosboot` | `rom/dosboot` | **violation** — `InitBootConfig()` naming `radeon.hidd` |
| `arch/i386-all/hidd/gfx`, `arch/x86_64-all/hidd/gfx` | `rom/hidds/gfx` | allowed — `<cpu>-all` |
| `arch/{i386,x86_64,m68k}-all/utility` | `rom/utility` | allowed |
| `arch/{arm,m68k}-all/dos` | `rom/dos` | allowed |
| `arch/ppc-chrp/dos` | `rom/dos` | allowed in kind — ELF loading, not display setup |
| `arch/m68k-all/dosboot` | `rom/dosboot` | allowed |
| `arch/{i386,m68k}-all/debug`, `arch/ppc-sam440/debug` | `rom/debug` | allowed — `debug` is declared `arch_libs` |

And chipset-shaped access in BASE *source*:

- `rom/graphics/qblit.c:94`, `qbsblit.c:91` — **fixed**, see below
- `rom/dosboot/menu.c:555,889` — `0xdff1dc`, `0xbfe001`, `0xdff016` under
  `#ifdef mc68000`, but guarded at runtime by `OpenResource("ciaa.resource")`.
  Inert off-Amiga; still machine knowledge in portable source.
- `rom/dosboot/menu.c:62-66` — `INITHIDDS_KLUDGE`, gated on `__ppc__`
- `rom/graphics/bestmodeida.c:352` — `#ifdef __mc68000` retrying the mode search
  with `MONITOR_ID_MASK` so PAL/NTSC IDs resolve. Chipset *policy*, no hardware
  access; changing it is an Amiga compatibility question, not a portability one.

Nothing was found in the other twenty-odd BASE modules.

# What is left

1. ~~Inventory.~~ Done, above.
2. ~~`qblit.c` / `qbsblit.c`.~~ Done — `patches/aros/0023`.
3. ~~Say what actually breaks the rule.~~ Done — `patches/aros/0024`.
4. **`graphics.library`: the six chipset functions.** `VBeamPos`, `WaitBlit`,
   `BltClear`, `MoveSprite`, `ChangeExtSpriteA`, `SetChipRev`. These are LVOs of
   the library, so they cannot be *moved* anywhere — either the hardware leaves
   graphics.library or it does not. Two shapes, and the choice needs upstream:
   - **runtime probe**, the pattern `rom/dosboot/menu.c` already uses in a BASE
     module: merge the Amiga implementation into the portable source behind a
     probe. The binary becomes portable, which is the rule's stated purpose;
     the portable source gains Amiga code, which is the rule's stated letter.
   - **driver-mediated**: graphics.library asks `amigavideo.hidd`, which already
     owns the chipset. Architecturally right, and what the dead
     `driver_WaitBlit(GfxBase)` comment in `rom/graphics/waitblit.c` always
     wanted. Needs new HIDD methods, and `WaitBlit` is a hot path currently
     written in assembly.
5. **`rgbconv.c`** and **the three `locklayerrom` assembly files**. Both are
   misfiled rather than machine-specific, so both want `arch/m68k-all/`. Held
   back deliberately: moving them changes what every *other* m68k target builds,
   `m68k-emu68` included, and neither has been shown to be safe there. See
   **Open questions**.
6. **`dosboot.resource` on PowerPC.** The kludge calls itself "extremely
   obsolete" in its own comment. Not ours to fix blind; recorded so the note
   stops pointing at `dos.library`.

# Decisions taken

**Machine, not CPU, is the axis.** The document says so, and the two correct
patterns already in the tree (`AROS_ARCH_amiga`, `OpenResource("ciaa.resource")`)
both use it. `defined(mc68000)` is never the right gate for a chipset access,
and every violation found so far is that same substitution.

**This is upstream-facing work.** Nothing here is specific to `m68k-emu68`, none
of it mentions our port, and each piece applies to upstream HEAD on its own —
the same standard as `docs/upstream-candidates.md`. Item 2 is a candidate for
that document as soon as it is written.

**Fix what the inventory turns up, opportunistically.** The named target is
`graphics.library`. Anything else the sweep finds gets fixed here rather than
spawning an issue per file, unless it turns out to have real design in it.

**The six chipset functions go to upstream as a question, not as a patch.**
Decided 2026-08-16. Both shapes in step 4 are defensible, they lead to very
different amounts of work, and neither can be validated here: there is no
m68k-amiga build or boot in this repository, and a change to those six
functions that compiles is not a change that is known to work. Writing either
one blind and offering it would be worse for a reviewer than asking.

**Do not claim the qblit poke is a live bug.** It is inert on our build, for
reasons that are recorded above and that could change. Overstating it would make
the writeup easy to dismiss, and the real argument — a portable module names a
machine's device address — does not need it.

# Acceptance criteria

- [x] An inventory exists: every BASE-module injection and every chipset access
      in BASE source, across all architectures
- [x] `rom/graphics/qblit.c` and `qbsblit.c` contain no `0xdff000` and no
      `mc68000` gate
- [x] `boot/modular_kickstart.txt` names what is non-conforming today, by
      function, and points at the right module for PowerPC
- [ ] `grep -rn '0xdff\|struct Custom' rom/graphics rom/hidds/gfx` is empty
- [ ] No `%build_archspecific` in `arch/` targets a BASE module
- [ ] `rgbconv.c` lives under `arch/m68k-all/`
- [ ] m68k-amiga still boots to the desktop, with the beam position, blitter
      wait and sprite support reaching the chipset through the gfx HIDD
- [ ] `m68k-emu68` still boots to the desktop
- [ ] The m68k-amiga entry is removed from `boot/modular_kickstart.txt`

# Notes

**The BASE/FS/Poseidon/BSP division is a second axis, orthogonal to
`<cpu>-all` / `<cpu>-native` / `<cpu>-<machine>`.** `ISSUE-0023` works the
directory axis; this one works the package axis. They agree on where things go,
and the package axis has the advantage of being checkable by grep over a built
tree rather than by reading.

**Our port is clean on axis A.** `aros/arch/m68k-emu68/` injects only into
`rom/exec`, `rom/kernel`, `rom/battclock` and `rom/devs/sdcard` — all BSP. It
inherits the axis-B violation because that one is in portable source and gated
on the CPU.

**`aros/arch/m68k-emu68/boot/selftest.c:269-277` still describes the INTENA
shadow as the live gate**, contradicting `platform/platform.c:20-44`. Stale
comment, unrelated to this issue, noted here so it is not lost.

# Open questions

- **Do the three `locklayerrom` assembly files even assemble outside amiga?**
  They reference `SysBase` and `ly_Lock` as assembler symbols, and neither
  appears in the generated `aros/m68k/asm.h` this build produces. Either they
  come from somewhere else or those files only assemble because of something in
  the m68k-amiga build. Answer this before moving them to `m68k-all`, where they
  would start being built for every m68k target.
- **What does `m68k-emu68` lose if `rgbconv.c` moves to `m68k-all`?** The Amiga
  build registers four colour conversions; the generic one registers all of
  them. Moving the file gives every m68k target the reduced table, which is a
  functional reduction for a target whose framebuffer is not planar. Measure
  before moving.
- **Where does `VBeamPos()` get its answer from on a machine with no beam?** The
  generic implementation is a stub returning 10. Routing it through the gfx HIDD
  needs a method that most drivers cannot answer, so "not implemented" may stay
  the right answer for everyone but Amiga — in which case the question is only
  how Amiga reaches its driver, not what the API means.
- **Does anything outside `graphics.library` depend on those symbols being
  linked into it?** `locklayerrom.S` and friends are `layers.library` support
  functions living in the graphics injection; moving them may move a link edge.

# Execution log

- 2026-08-16 — Opened. Survey done: `modular_kickstart.txt:49-52` located and
  confirmed accurate; the two BASE injections from `m68k-amiga` identified; the
  CPU-gated chipset access in portable `rom/graphics` found and confirmed to
  compile on `m68k-emu68`; established that it is inert on the current build
  because `patches/emu68/0002` removed the shadow from the delivery path.
- 2026-08-16 — Inventory completed against `rom/mmakefile.src`'s BASE list: three
  violating injections (two m68k-amiga, one PowerPC across two machines) and four
  chipset-shaped sites in BASE source. `patches/aros/0023` puts the QBlit/QBSBlit
  chipset access behind `arch_CauseBlitterInterrupt()`; `patches/aros/0024`
  rewrites the note to name what is non-conforming today. Both apply to the
  pinned commit and `setup.sh --verify` reports `applied`; `make
  kernel-graphics-kobj` builds clean. The six chipset LVOs are held for an
  upstream RFC.
- 2026-08-16 — Recorded that the inertness is temporary: `New_emu68.md` §14 hands
  INTENA/INTREQ to Rigel through the bus hook, at which point the stores reach a
  real owner. Stale IRQ descriptions corrected in `CLAUDE.md`, `docs/irq.md`
  (correction header) and `aros/arch/m68k-emu68/boot/selftest.c`.
