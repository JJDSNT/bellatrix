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
  no hardware access.

**The CPU macro is not a machine test, and on m68k that now matters.** All three
of those sites are selected by `mc68000` / `__mc68000`, which gcc defines for
every m68k target regardless of what the machine is:

```
$ m68k-aros-gcc -march=68040 -dM -E - </dev/null | grep mc68000
#define __mc68000 1
#define __mc68000__ 1
#define mc68000 1
```

So on an m68k machine with no Amiga chipset they are all compiled in. Two of
them survive that because a runtime probe stops them —
`OpenResource("ciaa.resource")` in both `menu.c` sites. `bestmodeida.c:352` has
no such probe: the PAL/NTSC retry is simply active. It is a fallback, reached
only when the exact search already failed, and we have not seen it return
anything harmful — but it is chipset policy deciding mode IDs on a machine with
no chipset, and nothing in the source says it was meant to be.

That is the argument for treating all three the same way, rather than only the
one that stores to hardware: the guard that makes two of them safe is a runtime
accident of what resources happen to be open, not a statement about the machine.

Nothing was found in the other twenty-odd BASE modules.

## Patch 1 — `graphics: keep the Amiga chipset out of QBlit() and QBSBlit()`

> **Superseded — see "reuse that header" above.** This patch invents an
> `arch_CauseBlitterInterrupt()` and two files to hold it. Once `chipset.h`'s
> gate works, the same job is `CUSTOM_CAUSE(INTF_BLIT); CUSTOM_ENABLE(INTB_BLIT);`
> with no new API and no new files. We are leaving the patch here because the
> problem statement below is unchanged and because the two are worth comparing;
> take the shorter one.

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

## The remaining `#ifdef` sites: reuse the header AROS already has

### `rom/exec/chipset.h` is the answer, and it works

`rom/exec/chipset.h` is a header in a portable module that gates Amiga chipset
access on the machine and defines every hook away to nothing elsewhere:

```c
#ifdef AROS_ARCH_amiga
static inline void CUSTOM_CAUSE(UWORD intBit)
{
    volatile struct Custom *custom = (struct Custom*)0xdff000;
    custom->intreq = INTF_SETCLR | intBit;
}
/* CUSTOM_ENABLE, CUSTOM_DISABLE, CUSTOM_ACK likewise */
#else
#define CUSTOM_ENABLE(intNumber)
#define CUSTOM_DISABLE(intNumber, list)
#define CUSTOM_ACK(intBit)
#define CUSTOM_CAUSE(intBit)
#endif
```

Call sites carry no `#ifdef` at all — `rom/exec/cause.c:104,137,189`,
`addintserver.c:73`, `remintserver.c:61`, `reschedule.c:73`. That is exactly the
shape the rule wants.

The gate comes from the module's own mmakefile, not from `configure`:

```make
rom/exec/mmakefile.src:58:  USER_CPPFLAGS := -DAROS_ARCH_$(ARCH)
```

We verified both sides by compiling `rom/exec/cause.c` for two targets and
disassembling. On `amiga-m68k`:

```
4e: 33fc 8004 00df   movew #0x8004,dff09c    ← CUSTOM_CAUSE(INTF_SOFTINT), INTREQ
6c: 33fc 0004 00df   movew #4,dff09a         ← CUSTOM_ACK, INTENA
74: 33fc 0004 00df   movew #4,dff09c         ← CUSTOM_ACK, INTREQ
f2: 33fc 8004 00df   movew #0x8004,dff09a    ← CUSTOM_ENABLE(INTB_SOFTINT)
```

On a chipset-free m68k target the same file emits none of those. The mechanism
is sound and in service.

### So the fix for the three remaining sites is two lines per module

They are outside `rom/exec`, which is the only module that defines the macro. Any
other module opts in by copying one line and adding the header's directory:

```make
USER_CPPFLAGS := -DAROS_ARCH_$(ARCH)
USER_INCLUDES := ... -I$(SRCDIR)/rom/exec
```

That is the entire build-system cost, in `rom/graphics` and `rom/dosboot`. No
new macro, no `configure` change, no new files, no new directories.

**`qblit.c` / `qbsblit.c` then need no new API.** The two stores they open-code
are precisely `CUSTOM_CAUSE` and `CUSTOM_ENABLE`:

```c
CUSTOM_CAUSE(INTF_BLIT);
CUSTOM_ENABLE(INTB_BLIT);
```

This supersedes our own Patch 1 below, which invented an
`arch_CauseBlitterInterrupt()` and two files to hold it. **Take this version
instead**; Patch 1 is left in the document only so the two can be compared.

**`menu.c` needs two hooks, in the same header, in the same shape:**

| Site | Hook | Non-Amiga | Amiga |
|---|---|---|---|
| `menu.c:553` `toggleMode()` | `CUSTOM_SET_PAL(BOOL pal)` | nothing | the `$DFF1DC` BEAMCON0 write |
| `menu.c:889` `buttonsPressed()` | `CUSTOM_MOUSE_HELD()` | `FALSE` | the `$BFE001` / `$DFF016` read |

The runtime `OpenResource("ciaa.resource")` probes at both sites then become
redundant and can go: the machine question moves from boot time to compile time
and the portable source stops naming a CPU.

`CUSTOM_MOUSE_HELD()` reads CIA-A rather than the custom chips, so if you would
rather it did not live in a header called `chipset.h`, that is a naming call we
are happy to take either way.

**`bestmodeida.c:352` gets the gate and nothing else.** It is not hardware
access — it is a policy decision about whether PAL and NTSC monitor IDs exist —
so wrapping it in a hardware hook would be dressing. Changing `#ifdef __mc68000`
to `#ifdef AROS_ARCH_amiga` is the whole fix, and it is the one that stops
chipset mode policy running on a chipset-free m68k. Building a seam for a case
that does not need one is how a rule like this becomes expensive to keep.

**`menu.c:62-66` (`INITHIDDS_KLUDGE`, `__ppc__`)** is the same substitution with
a different owner and no hardware in it: it selects a display-driver setup
kludge. Same treatment — `AROS_ARCH_<machine>` rather than a CPU — once you
decide whether it should still exist, which its own comment ("extremely
obsolete") suggests is the real question.

### Why this and not per-module arch headers

The obvious alternative is a `<module>_arch.h` per module with an
`arch/m68k-amiga/<module>/` override on the include path, the way
`rom/kernel/kernel_arch.h` works. We drafted that and rejected it: it means two
new headers, a new `arch/m68k-amiga/dosboot/` directory, `%set_archincludes` in
two more mmakefiles, and an include-search-order question against the existing
`arch/m68k-all/dosboot`. All of that to express what one already-working header
expresses in two lines of mmakefile.

It would also put `0xdff000` in three files instead of one. The property worth
defending is that the address appears exactly once in portable source; that is
what makes the rule checkable by grep rather than by review.

**The one thing this design costs** is that `chipset.h` stops being private to
`rom/exec`. If you would rather it moved somewhere all three modules see rather
than being reached with an `-I`, say where and we will do that instead.

We have not written any of this. It is code that only runs on m68k-amiga, which
we cannot boot, so it would be compiled and not tested — but the build-system
half is two lines per module and we will send it on a word from you.


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

1. Take Patch 2. **Replace Patch 1** with the `CUSTOM_CAUSE`/`CUSTOM_ENABLE`
   version above, which needs no new API and no new files.
2. **Say whether `rom/graphics` and `rom/dosboot` may opt into
   `-DAROS_ARCH_$(ARCH)` and `-I$(SRCDIR)/rom/exec`**, copying `rom/exec`'s own
   two lines. That is the whole build-system cost of everything below, and it is
   the only decision that is really yours rather than ours.
3. Say whether you want `menu.c`'s two sites moved behind new `CUSTOM_*` hooks in
   that header, and whether `bestmodeida.c` and the `__ppc__` kludge should get
   `AROS_ARCH_<machine>` in place of their CPU gate and nothing else.
4. Tell us which shape you want for the six chipset functions, and we will
   write it — or say that the note should record them as a permanent
   exception, which is also an answer, and a better one than a note that has
   said "does not conform" for fifteen years.
