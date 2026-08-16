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

## The remaining `#ifdef` sites: two separate fixes, and the first is one line

They answer different halves of the complaint, and the smaller one needs no
agreement about the larger.

### First: `rom/exec/chipset.h` is already the answer, and it has never worked

The design we would propose is in the tree, written in 2011, and dead. This
changes what we are asking for, so it is worth the space.

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
shape the rule wants, and it is the shape we would have proposed.

**`AROS_ARCH_amiga` is defined nowhere.** It appears once in the entire tree, at
`chipset.h:7`, testing itself. We checked every plausible source:

| Candidate | Reality |
|---|---|
| `AROS_ARCH_amiga` | used at `chipset.h:7`; **defined nowhere in the tree** |
| `__AROS_ARCH_<arch>__` | emitted by `config/specs.in:26`; **used nowhere**, and absent from a crosstools compile |
| `AMIGA`, `_AMIGA` | `builtin_define` in **`gcc/config/aros.h:43-44`** — every AROS target on every CPU. They mean "AROS", not "Amiga hardware" |
| `mc68000`, `__mc68000` | every m68k target. What the three sites use today |

Two spellings of the same intent that never met. So the Amiga branch of
`chipset.h` has been dead since it was written, on m68k-amiga as much as
anywhere: nothing overrides `cause.c`, `addintserver.c`, `remintserver.c` or
`reschedule.c` for that target — `arch/m68k-amiga/exec/` supplies only
`coldreboot`, `disable.S`, `enable.S`, `exec_globals`, `moveexecbase`,
`readgayle` and `shutdowna`.

**Please check what that costs m68k-amiga, because we cannot.** `Cause()` says in
its own comment:

> Quick soft int request. For optimal performance m68k-amiga `Enable()` does not
> do any extra `SFF_SoftInt` checks

and `arch/m68k-amiga/exec/enable.S` indeed only writes `#0xc000` to `$DFF09A`,
with no `SFF_SoftInt` test. If `CUSTOM_CAUSE(INTF_SOFTINT)` compiles to nothing,
the INTREQ bit that would make Paula raise the queued interrupt is never set. We
do not have an Amiga to test on and will not assert a bug we cannot reproduce —
but either something else covers this, or software interrupts on m68k-amiga
depend on a macro that expands to nothing.

**The fix is one define.** Our preference, because it costs one parameterised
line rather than an Amiga special case:

```diff
  # configure.in, after aros_target_arch is known
+ aros_config_cppflags="$aros_config_cppflags -D__AROS_ARCH_$aros_target_arch""__"
```

```diff
  # rom/exec/chipset.h
- #ifdef AROS_ARCH_amiga
+ #ifdef __AROS_ARCH_amiga__
```

`__AROS_ARCH_<arch>__` is the spelling `config/specs.in` already chose; this
makes it reach every target through the variable that demonstrably ends up on
the compile line — the same `aros_config_cppflags` the `amiga*` m68k block
already uses for `-DNOLIBINLINE`. Every architecture gets its own, so the next
machine needs no further work. If you would rather keep the bare spelling and
define that instead, the shape is identical and we do not mind which.

### Second: reuse that header for the remaining sites — do not build a second mechanism

With the gate working, the three surviving `#ifdef` sites become uses of a
pattern that already exists, already has consumers, and already passed review.

**`qblit.c` / `qbsblit.c` need no new API at all.** The two stores they open-code
are precisely `CUSTOM_CAUSE` and `CUSTOM_ENABLE`:

```c
CUSTOM_CAUSE(INTF_BLIT);
CUSTOM_ENABLE(INTB_BLIT);
```

This supersedes our own Patch 1 below, which invented an
`arch_CauseBlitterInterrupt()` and two new files to hold it. Reusing the existing
macros is smaller, adds no API, and deletes rather than adds. **Take that version
instead**; Patch 1 is left in this document only so the two can be compared.

**`menu.c` needs two hooks, in the same file, in the same shape:**

| Site | Hook | Non-Amiga | Amiga |
|---|---|---|---|
| `menu.c:553` `toggleMode()` | `CUSTOM_SET_PAL(BOOL pal)` | nothing | the `$DFF1DC` BEAMCON0 write |
| `menu.c:889` `buttonsPressed()` | `CUSTOM_MOUSE_HELD()` | `FALSE` | the `$BFE001` / `$DFF016` read |

The runtime `OpenResource("ciaa.resource")` probes at both sites then become
redundant and can go, which is the point: the machine question moves from boot
time to compile time, and the portable source stops naming a CPU.

**`bestmodeida.c:352` gets the gate and nothing else.** It is not hardware
access — it is a policy decision about whether PAL and NTSC monitor IDs exist —
so wrapping it in a hardware hook would be dressing. Changing `#ifdef __mc68000`
to the machine macro is the whole fix, and it is the one that stops chipset mode
policy running on a chipset-free m68k. Building a seam for a case that does not
need one is how a rule like this becomes expensive to keep.

**`menu.c:62-66` (`INITHIDDS_KLUDGE`, `__ppc__`)** is the same substitution with
a different owner and no hardware in it: it selects a display-driver setup
kludge. Same treatment — a machine gate rather than a CPU gate — once you decide
whether it should still exist, which its own comment ("extremely obsolete")
suggests is the real question.

### Why this and not per-module arch headers

The obvious alternative is a `<module>_arch.h` per module with an
`arch/m68k-amiga/<module>/` override on the include path, the way
`rom/kernel/kernel_arch.h` works. We drafted that and then rejected it: it means
two new headers, a new `arch/m68k-amiga/dosboot/` directory, `%set_archincludes`
in two more mmakefiles, and an include-search-order question against the existing
`arch/m68k-all/dosboot`. All of that to express what one already-present header
expresses.

It would also put `0xdff000` in three files instead of one. The property worth
defending is that the address appears exactly once in portable source; that is
what makes the rule checkable by grep rather than by review.

**The one thing this design costs** is that `chipset.h` stops being private to
`rom/exec`. Two modules would need it on their include path
(`-I$(SRCDIR)/rom/exec` in `rom/graphics` and `rom/dosboot`), or the header moves
somewhere all three see. We would take the `-I`, because a move is a bigger diff
for a reviewer than two lines, and because the file is already conceptually the
tree's machine-hook header rather than exec's. If you prefer it moved, say where.

We have not written any of this. The gate is one line and we would send it
immediately if you name the spelling; the rest is code that only runs on
m68k-amiga, which we cannot boot, so it would be compiled and not tested.

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

1. **Check `chipset.h` first.** `AROS_ARCH_amiga` is defined nowhere, so the
   Amiga branch has been dead since 2011 and `CUSTOM_CAUSE(INTF_SOFTINT)` in
   `Cause()` expands to nothing on m68k-amiga too. That may be a live defect in
   software-interrupt delivery on real hardware. It is the only item here we
   would call urgent, and it is the only one we cannot test.
2. **Name the spelling for the machine macro** — `__AROS_ARCH_<arch>__` reaching
   the compile through `aros_config_cppflags`, or the bare `AROS_ARCH_<arch>`
   that `chipset.h` already expects. One parameterised line either way, and it
   is the prerequisite for everything else in this document.
3. Take Patch 2. **Replace Patch 1** with the `CUSTOM_CAUSE`/`CUSTOM_ENABLE`
   version above, which needs no new API and no new files.
4. Say whether you want `menu.c`'s two sites moved behind `CUSTOM_*` hooks in the
   same header, and whether `bestmodeida.c` and the `__ppc__` kludge should get
   the gate alone.
5. Tell us which shape you want for the six chipset functions, and we will
   write it — or say that the note should record them as a permanent
   exception, which is also an answer, and a better one than a note that has
   said "does not conform" for fifteen years.
