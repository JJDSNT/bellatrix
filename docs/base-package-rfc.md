# The kickstart base package still has machine-specific code in it

Two patches, and one question we would rather ask than answer badly.

`boot/modular_kickstart.txt` has carried a note since 12.09.2011 saying that
the BASE package is not allowed to contain machine-specific code, and that two
ports break the rule anyway. Fifteen years later the rule is still right, the
note is partly wrong about who breaks it, and one of the violations had spread
into portable source.

We went looking because we are bringing AROS/m68k up on a machine that has no
Amiga chipset, which is an unusual environment and exposes things a real Amiga
does not. Where the argument depends on that, it says so. Nothing here is
specific to that port, and none of it mentions it.

## The rule

`boot/modular_kickstart.txt`, describing the BASE package:

> Base package contains machine-independent modules. This package is designed
> to be fully portable and independent of its environment. The same base
> package can run on both hosted and native AROS.
>
> These modules are not allowed to have machine-specific code because it
> violates portability convention. As of 12.09.2011, m68k-amiga
> (chipset-specific code in graphics.library) and PowerPC-native (display
> driver setup kludge in dos.library boot code) ports do not conform to this
> rule.
>
> Of course "machine-specific code" does not include "cpu-specific code". It's
> allowed for any module to have relevant portions in `arch/<cpu>-all`.

That last paragraph is the load-bearing one, and it is the distinction every
violation we found gets wrong in the same way: **the gates are on the CPU.**
`defined(mc68000)` and `__ppc__` say nothing about which machine the module
will run on.

## What is actually non-conforming today

The authoritative BASE list is `rom/mmakefile.src:139-145`, not the prose in
the note. We checked every `%build_archspecific` in `arch/` — 212 of them,
excluding `.unmaintained` — for one whose `maindir` is a BASE module:

| Injection | Into | |
|---|---|---|
| `arch/m68k-amiga/graphics` | `rom/graphics` | violation, and the one the note names |
| `arch/m68k-amiga/hidd/gfx` | `rom/hidds/gfx` | violation, though nothing in it is machine-specific |
| `arch/ppc-chrp/dosboot`, `arch/ppc-sam440/dosboot` | `rom/dosboot` | violation |
| `arch/{i386,x86_64}-all/hidd/gfx` | `rom/hidds/gfx` | allowed, `<cpu>-all` |
| `arch/{i386,x86_64,m68k}-all/utility` | `rom/utility` | allowed |
| `arch/{arm,m68k}-all/dos` | `rom/dos` | allowed |
| `arch/ppc-chrp/dos` | `rom/dos` | allowed in kind — ELF loading, not display setup |
| `arch/m68k-all/dosboot` | `rom/dosboot` | allowed |
| `arch/{i386,m68k}-all/debug`, `arch/ppc-sam440/debug` | `rom/debug` | allowed — `debug` is declared `arch_libs` |

And in BASE *source*, rather than in an injection:

- `rom/graphics/qblit.c:94` and `rom/graphics/qbsblit.c:91` — chipset stores,
  in the portable module. **Patch 1 below.**
- `rom/dosboot/menu.c:555,889` — `$DFF1DC`, `$BFE001`, `$DFF016` under
  `#ifdef mc68000`, but guarded at runtime by `OpenResource("ciaa.resource")`,
  so they are inert on a machine with no CIA. Machine knowledge in portable
  source, with the failure mode already handled.
- `rom/dosboot/menu.c:62-66` — `INITHIDDS_KLUDGE`, gated on `__ppc__`.
- `rom/graphics/bestmodeida.c:352` — `#ifdef __mc68000` retrying the mode
  search with `MONITOR_ID_MASK` so PAL and NTSC IDs resolve. Chipset policy,
  no hardware access. Changing it is an Amiga compatibility question, not a
  portability one, so we have left it alone.

Nothing was found in the other twenty-odd BASE modules.

## Patch 1 — `graphics: keep the Amiga chipset out of QBlit() and QBSBlit()`

`QBlit()` and `QBSBlit()` cause a blitter interrupt by writing INTREQ and
INTENA at `$DFF000`, under

```c
#if (AROS_FLAVOUR & AROS_FLAVOUR_BINCOMPAT) && defined(mc68000)
```

so an m68k machine that is not an Amiga gets `graphics.library` with stores to
`$DFF000` compiled into it, and whether that is harmless depends entirely on
what the machine leaves at that address. This is the one violation that had
escaped the `arch/` tree.

The patch puts the hardware behind a hook. `rom/graphics/graphics_arch.c`
carries an empty `arch_CauseBlitterInterrupt()`; the Amiga one in
`arch/m68k-amiga/graphics/graphics_arch.c` does what the inline code did,
unchanged, still under the `AROS_FLAVOUR_BINCOMPAT` test. That is the shape
`rom/hidds/gfx/rgbconv_arch.c` already uses for the architecture-specific
colour conversions.

No functional change on any target: m68k-amiga issues the same two stores, and
every other target ran nothing before and runs nothing now.

## Patch 2 — `boot: say what actually breaks the base package rule today`

The note's PowerPC half points at the wrong module. The display driver setup
kludge is in `dosboot.resource` now: `rom/dosboot/menu.c` defines
`INITHIDDS_KLUDGE` for `__ppc__` and calls `initHidds()`, and
`arch/ppc-chrp/dosboot` and `arch/ppc-sam440/dosboot` supply the
`InitBootConfig()` that names `radeon.hidd`. Nothing in `dos.library` does this
any more. Its own comment calls it *"an extremely obsolete kludge"*.

Two cases were never listed at all — `gfx.hidd`'s `rgbconv.c` and the
`LockLayerRom()` assembly, both discussed below.

The patch rewrites the note to name the functions rather than the module,
which is what makes the remaining work countable, and records the
CPU-gate-versus-machine-gate observation, since that is the thing that keeps
reintroducing these.

## The question

Six functions in `graphics.library` are replaced from
`arch/m68k-amiga/graphics` and reach the chipset directly:

`VBeamPos()`, `WaitBlit()`, `BltClear()`, `MoveSprite()`,
`ChangeExtSpriteA()`, `SetChipRev()`

They are LVOs of the library, so they cannot be *moved* anywhere. Either the
hardware leaves `graphics.library` or it does not, and we can see two shapes
for that, with a real trade between them.

**Runtime probe.** Merge the Amiga implementation into the portable source,
behind a runtime probe. `rom/dosboot/menu.c` already does exactly this in a
BASE module, so there is precedent for it being acceptable. The binary becomes
portable, which is the rule's stated purpose. Against it: the portable source
gains Amiga code, which is the rule's stated letter, and every target's
`graphics.library` carries six functions it will never call.

**Driver-mediated.** `graphics.library` asks the display driver, which on
m68k-amiga is `amigavideo.hidd` and already owns the chipset. This is what the
commented-out `driver_WaitBlit(GfxBase)` in `rom/graphics/waitblit.c` has been
waiting for, and it is the only shape that makes the generic implementations
mean something — `rom/graphics/vbeampos.c` is currently
`aros_print_not_implemented("VBeamPos"); return 10;`. Against it: new HIDD
methods, and `WaitBlit()` is a hot path that is assembly today for a reason.

We are not offering either one. Both are defensible, they lead to very
different amounts of work, and **we cannot validate either**: we have no
m68k-amiga machine to boot, and a change to those six functions that compiles
is not a change that is known to work. Writing one blind and sending it would
be worse for a reviewer than asking which one you want.

## Two more we deliberately did not touch

Both look mechanical and are not.

**`arch/m68k-amiga/hidd/gfx/rgbconv.c`** replaces `gfx.hidd`'s `rgbconv.c`
with a build registering four colour conversions rather than all of them.
Nothing in the file is machine-specific — it is a ROM size trade, and the
cpu-specific equivalents for i386 and x86_64 are already filed under
`arch/<cpu>-all` as the rule allows. But moving it to `arch/m68k-all` would
give the reduced table to every m68k target, which is a functional reduction
for one whose framebuffer is not planar. If the intent is "small ROM", that
wants to be a build option rather than an architecture directory.

**`attemptlocklayerrom.S`, `locklayerrom.S`, `unlocklayerrom.S`** in
`arch/m68k-amiga/graphics` touch no hardware at all — they call
`ObtainSemaphore()`, `AttemptSemaphore()` and `ReleaseSemaphore()` on
`ly_Lock`, in assembly, to preserve more registers than the C compiler would.
That is cpu-specific code filed under a machine, and it belongs in
`arch/m68k-all`. What stopped us: they reference `SysBase` and `ly_Lock` as
assembler symbols, and neither appears in the `aros/m68k/asm.h` our build
generates. We do not yet know where they come from, and moving the files would
start building them for every m68k target.

## What we are asking

1. Take the two patches, or say what would make them takeable.
2. Tell us which shape you want for the six chipset functions, and we will
   write it — or say that the note should record them as a permanent
   exception, which is also an answer, and a better one than a note that has
   said "does not conform" for fifteen years.
