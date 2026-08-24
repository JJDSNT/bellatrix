---
id: ISSUE-0045
title: "VC4 Gallium hangs when the first MesaGL workload starts"
status: investigating
priority: high
type: bug
owner: unassigned
created_at: 2026-08-21
updated_at: 2026-08-23
tags:
  - vc4
  - gallium
  - mesa
  - raspberry-pi-3
  - graphics
related_files:
  - aros/arch/m68k-emu68/hidd/vc4gallium
  - external/aros/workbench/libs/mesa
  - external/aros-contrib/Demo/GL/MesaGL
---

# Summary

## Resolved on hardware: Mesa needs a very large stack (2026-08-24)

On a Raspberry Pi 3, with `stack 100000000` -- one hundred megabytes -- the
demos work:

    3.Work:> stack 100000000
    3.Work:> gl_test
    DEBUG: Window created successfully.
    DEBUG: glAmigaCreateContext returned
    DEBUG: Calling glAmigaMakeCurrent...
    DEBUG: glAmigaMakeCurrent finished.
    GL_VENDOR:   Broadcom
    GL_RENDERER: VC4 V3D 2.1
    GL_VERSION:  2.1 Mesa 20.0.8
    DEBUG: Entering render loop.

The shell gives a command `AROS_STACKSIZE`, which is `0xA000` -- **40 KB** --
on m68k. Mesa's initialisation is nowhere near that shallow.

**This was reported here as excluded, and that was wrong.** The stack was
tested at 1 MB, found to change nothing, and written off. 1 MB is two orders of
magnitude below what the demos need, so the test proved nothing and the
conclusion drawn from it was false. `S:gl-run` now sets the stack before
launching a demo.

## And the QEMU failure is a different thing

With the same 100 MB stack under QEMU, `OpenLibrary("mesa3dgl20-0.library")`
still does not return in 400 seconds. So the two failures this issue has been
treating as one are not:

- **on hardware**, the demos were blocked by the stack, and are not any more;
- **under QEMU**, the 10 MB library still cannot be loaded, and that is an
  emulation problem -- see ISSUE-0051, where device-level throughput and the
  I/O amplification are measured, both of which are far worse under QEMU than
  on silicon because every MMIO access costs a host round trip.

Everything below this line was written while those two were conflated. It is
kept because the measurements are real and ISSUE-0051 depends on them, but the
framing -- "the demos do not work" -- was only ever true of QEMU.

The native VC4 2D/HVS path passes its hardware validation on a Raspberry Pi 3,
but the first 3D smoke test does not. Running the official MesaGL `gears`
example from `SYS:Extras/Demos/GL` hangs the machine.

The distribution and pack contain the complete expected runtime chain:
`gl.library`, `mesa3dgl20-0.library`, `gallium.library`, `gallium.hidd`,
`vc4gallium.hidd`, the GLUT link library used by the examples, and all fourteen
official MesaGL example executables. They are m68k/68020 AROS binaries and the
pack passes its archive-integrity and contents checks. This establishes that
the failure is after packaging, but does not yet identify whether it occurs
while opening the GL context, binding the GalliumCoreAPI table, initialising
V3D, submitting the first command list, or waiting for completion.

# First hardware result

- Platform: Raspberry Pi 3, native Bellatrix VC4 display path.
- 2D state before the test: HVS takeover active, PixelValve vsync alive, and
  Wanderer reached.
- Action: launch `SYS:Extras/Demos/GL/gears`.
- Result: machine hang.
- Missing evidence: serial output immediately before/after the launch and
  `glinfo` output were not captured with this report.

# Next evidence

1. Boot with serial capture active and run `glinfo` before `gears`.
2. Add narrowly placed diagnostics around context creation, `CreatePipeScreen`,
   GalliumCoreAPI binding, VC4 screen/context creation, command submission and
   waits, stopping after the last message reached on hardware.
3. Distinguish a dead wait from an exception or memory corruption using the
   serial log; do not infer a V3D interrupt problem merely from the visible
   freeze.
4. Re-run `gears`, preserving the complete serial log and HDMI state.

# It reproduces without V3D (2026-08-22)

`glinfo` was run automatically at boot under QEMU, with all output on
`DEBUG:` so that it survives a machine that stops responding
(`tests/gl/gl-probe`, run by `BELLATRIX_BOOT_TEST=gl-probe`). The result:

    [gl-probe] libraries present:
    gl.library                  9552
    gallium.library            18168
    mesa3dgl20-0.library     10456700
    [gl-probe] drivers present:
    gallium.hidd                9620
    vc4gallium.hidd           444700
    softpipe.hidd            1016912
    [gl-probe] running glinfo
    <nothing further>

Two things follow.

**The chain is complete and the failure is not packaging.** Every library and
driver the runtime needs is present and openable from the running system, not
merely present in the archive.

**QEMU has no V3D, and it still does not come back.** vc4gallium should fail
its probe there and softpipe should take over, so whatever this is, it is not
specific to the 3D hardware. That matters more for the investigation than for
the diagnosis: it means this can be worked on without a Pi.

The caveat is real: it is not established that the QEMU stop and the Pi 3 hang
are the same fault. What is established is that a machine with no V3D at all
also fails to return from the first GL program.

**Second hypothesis, not yet excluded: it is not hung, it is crawling.**
`mesa3dgl20-0.library` is 10 MB. Loading and relocating that on m68k, through
a JIT that has to translate every basic block it touches for the first time,
is not obviously a bounded wait. A longer run distinguishes the two, and until
one has been done, "hang" is an interpretation rather than an observation.

# Timed, link by link (2026-08-23)

`tests/gl/gl-probe` now opens each link of the chain on its own with `Date`
either side, because "running glinfo" and then silence cannot tell a stop from
a crawl. Run **late** -- `BELLATRIX_BOOT_TEST_LATE=1` -- and the early anchor
turns out to matter (see below). Guest clock:

| step | elapsed |
|---|---|
| `Version gallium.library` | 00:36 -> 00:36 (instant) |
| `Version mesa3dgl20-0.library` (10 MB) | 00:36 -> **00:55 (19 s)** |
| `Version gl.library` | 00:55 -> 00:55 (instant) |
| `glinfo` | 00:55 -> never returned (5+ minutes of wall clock) |

What this settles:

- **The whole chain opens.** Every library loads, relocates and initialises.
- **"It is crawling through the 10 MB library" is real, and is not the fault.**
  That load costs 19 seconds and then *finishes*. The second hypothesis in the
  section above is answered: measured, bounded, and not where the boot stops.
- **The machine is executing, not deadlocked at the CPU level.** QEMU sits at
  380% host CPU for the whole time. That does not distinguish progress from a
  busy-wait, but it rules out a stopped CPU.
- **It never reaches the VC4 Gallium driver.** `HiddVC4Gallium_InitLib`
  (`vc4_init.c:373`) has an unconditional `bug("[VC4Gallium] V3D clock: ...")`
  near its start, and there is not one `[VC4Gallium]` line in the log. So the
  stop is *before* `OpenLibrary("vc4gallium.hidd")` completes -- inside GLUT,
  Mesa's own init, or `CreatePipe` before it reaches a driver.

  (`glinfo`'s own stdout is buffered and proves nothing either way. `bug()`
  goes straight to the serial line and is what carries the argument.)

## One defect found on the way, and one wrong accusation

**Corrected the same day: the V3D probe and the softpipe fallback are present
and correct, in ours exactly as upstream.**

I first read `vcgfx_hiddclass.c:760-774` -- which opens `vc4gallium.hidd` and
creates `hidd.gallium.vc4` whenever the library opens -- as our own deviation,
committing to VC4 one level above the probe. Two checks say otherwise:

- Our `CreateObject` is **byte-identical** to upstream's
  (`diff` of the two function bodies is empty).
- The decision is not made there. It is made inside the class:
  `HiddVC4Gallium::New` (`vc4_galliumclass.c:278-284`) tests `v3d_available`,
  prints `[VC4Gallium] V3D hardware not available`, disposes the object and
  returns NULL -- at which point `object` stays NULL in `CreateObject` and
  `createpipe.c` takes its softpipe fallback exactly as designed.

So the chain that should land a V3D-less machine on softpipe is complete end to
end and needs no change. It simply never runs, which is what the missing
`[VC4Gallium]` lines were already saying -- and now say twice over, because
there are two unconditional `bug()` calls on that path (`vc4_init.c:373` and
`vc4_galliumclass.c:280`) and neither appears.

**The real defect: `BELLATRIX_BOOT_TEST` without `_LATE` runs the probe too
early to speak.**

At the default anchor (`Assign "IMAGES:"`, line 30 of the Startup-Sequence) the
boot enters the script and never leaves: `STARTING DOS` is reached,
`STARTING SERVICES` is not, and **not one line of the probe's output appears**
-- not even the first `Echo`. A control run of the same build with no boot test
reaches the icons in 01:00. Whatever `>DEBUG:` needs is not up at line 30.

The comment in `make-sdcard.sh:288-297` argues for the early anchor to avoid
waiting out the boot. For anything that reports through `DEBUG:` that trade is
not available. This cost two runs that were read as evidence about GL and were
evidence about the anchor.

## Next

1. Turn on the `D()` tracing already written into
   `workbench/libs/gallium/createpipe.c` and rerun. Every branch of the driver
   selection prints there. That is a small rebuild of one library, not of Mesa,
   and it should say whether `CreatePipe` is even reached.
2. Make the boot test refuse the early anchor, or say why its output vanishes
   there. A probe that cannot report is worse than no probe: it produced two
   confident readings that were about the harness.

# Instrumented, and mostly instrumenting the harness (2026-08-23, later)

`glinfo` was instrumented (`patches/aros-contrib/0001`, a new series -- the
first this repository has for contrib) with markers through two routes: `bug()`
to the raw serial and `Printf()`+`Flush()` to the program's own output. A
control program built for the purpose, `C:SerialSay`, prints through **both**
from a shell command on this machine.

## What has to be retracted

**"glinfo produces no output" was largely the harness muting the machine.**
Redirecting a long-running program's output to `DEBUG:` holds the serial for as
long as the handle is open: `bug()`, the boot presentation and the display
driver all go quiet behind it. Several runs were read as evidence about GL and
were evidence about the redirect. That is the same shape of mistake as the boot
anchor above, in the same session.

## What is real, and reproducible

**Synchronous and asynchronous do not behave the same, with the same binary
and the same stack setting.**

| launched as | mode change | machine afterwards |
|---|---|---|
| `SYS:Extras/Demos/GL/glinfo` (in the shell's process) | none | serial stops for everyone |
| `Run ... glinfo` (own process) | **yes, at ~1 s** | answers for as long as you wait |

Launched into its own process, the display driver programs a mode right after
the launch -- `retargeted to RGB32 framebuffer`, `assembling off-scanout`,
`display takeover` -- so the program reaches the point where a GL context wants
a screen. Then it spins, with QEMU at 380% host CPU, and the shell that
launched it goes on printing `20s`, `60s`, `120s` beside it.

Run synchronously it never gets that far: no mode change at all, and nothing
further on the serial.

That difference is the closest thing to a cause this issue has, and it is
exactly the situation the machine is reported to hang in.

**Stack is not it, at least not at the shell's level.** The default is
`AROS_STACKSIZE` = `0xA000` = 40 KB on m68k
(`arch/m68k-all/include/aros/cpu.h:146`), confirmed live by `Stack` reporting
40960 bytes. Raising it to 1 MB before launching changes nothing: same silence,
same absence of a mode change.

## The instrument is sound; the silence is real (settled)

`C:SerialSayP` -- the same program as `C:SerialSay` but in its own directory so
that it links the full C runtime, the way a contrib demo does -- prints through
**all three** routes on this machine:

    [serialsayp] via bug()
    [serialsayp] via Printf()
    [serialsayp] via printf()

So the C runtime linkage is not what silences glinfo, and the difference that
had been suspected turns out to be on our side anyway: the `-noposixc` in
`arch/m68k-emu68/c/mmakefile.src` is ours, set for `BootProgress`, and every
program in that directory inherits it. The contrib demos link the full runtime
and are otherwise ordinary.

**Therefore glinfo really does not reach its constructors or its `main()`.**
Markers at `ADD2INIT` priority 110 and -110 and on the first line of `main()`
have never printed, and now that is a fact about glinfo rather than about the
instrument.

That places the failure between the library autoinit -- which the `RamLib`
trace shows completing, down to `stdcio`, `z1` and `posixc` -- and the first
init-set call. It is a startup failure, not a GL failure, and the GL chain is
merely what is being started.

## The instrument was not trustworthy (superseded above)

Not one `[glinfo]` marker has ever appeared, in any configuration, including
those where the program demonstrably runs far enough to program a display mode.
The binary on the card carries all nine marker strings, `kprintf` is linked into
it, and `C:SerialSay` -- same `bug()`, same machine, same shell -- is heard.

The one structural difference found so far: `SerialSay` is built `-static
-noposixc`, `glinfo` links posixc and stdc. Until that is understood, no
reading taken with these markers means anything.

## Next

1. Find why `kprintf` is silent from a posixc-linked contrib program. Compare
   `SerialSay` (`-noposixc`) against `glinfo` for what happens to the debug
   output on the way. Without this there is no instrument.
2. Then bracket `glutCreateWindow`, which is where the mode change comes from
   and therefore where a GL context is being built.
3. Explain the synchronous/asynchronous difference. Same binary, same stack,
   different process, and one of them takes the serial down.

# gears does not hang -- it crashes, and differently (2026-08-23)

`glinfo` spins. `gears` does not. Launched the same way (own process, output to
`NIL:`) it produces, within a second of the launch:

    Undefined Line0 000d
    Undefined Line0 000d
    [JIT:SYS] open bus read:  guest 0xffffdb87 m68kPC 02003d40 A7 02003d34
    [JIT:SYS] open bus write: guest 0xffffdb87 m68kPC 02003d40 A7 02003d34
    ... alternating, eight times, then suppressed
    [BELLATRIX] bus R8 addr=00003462 pc=0202013a [classic domain, unclassified]
    [BELLATRIX] bus W8 addr=00003462 pc=0202013a

`Undefined Line0` means the CPU is already executing data before any of this.
The wild address is sign-extended from 16 bits (`0xffffdb87` is `0xdb87`
widened), so something loaded a word and used it as a pointer.

## The reporter was eating the evidence

The first run of this ended:

    [JIT:SYS] RE-ENTERED at depth 1 on core 0
    [JIT:SYS] runaway exception recursion, halting core 0

with **no register dump at all**. The guest return address on the faulting
access resolves to `emu68_trap_report` -- our own trap reporter was on the
stack when the second fault arrived.

It never returns: it ends in a halt loop. So a trap taken while it runs does
not unwind, it re-enters, and everything in it runs again on a machine that is
already broken -- it walks two stacks and follows A6 as a library base, and any
one of those reads can be the fault that brings it back. The crash that
mattered was replaced by the crash the reporter caused.

Fixed: the second entry prints one line and stops. The same run then produced
75 lines of bus trace instead of nothing, which is the whole finding below.

## What gears actually does

With the reporter no longer destroying it, gears does not take a CPU exception
at all. It walks low memory:

    R8/W8 at 0x3462, 0x346e, 0x347a, 0x3486, 0x3492 ... 0x35fa

**A read-modify-write of one byte, striding 12, climbing steadily**, all from
`pc=0202012e`, with occasional `R16` reads at 0x204e and 0x6d77. Our bus
classifier calls it `[classic domain, unclassified]` -- the guest believes it
is writing to chip RAM.

**Correction: "the guest thinks it is writing to chip RAM" was my reading of
our own label, not a fact about the program.** `classic domain, unclassified`
is what this port's memory map calls that range, and the map says why:

    $00000000-$00000fff DIRECT   vectors + AbsExecBase
    $00001000-$00ffffff UNMAPPED classic domain, unclassified

`0x3462` is simply **unmapped**. Reads return open bus, writes go nowhere, and
that is why the machine survives instead of faulting. Nothing here is aimed at
a chipset, and nothing is built for the wrong port: there is no `MEMF_CHIP` in
mesa, glut, gallium or the gallium hidd, and the only AROS target built in this
tree is `emu68-m68k`.

What the two anomalies share is more useful than the label. `0xffffdb87` is
`0xdb87` widened with sign, and `0x3462` is a small number -- **both are
pointers that do not carry 32 bits of address.**

## The Gallium trampolines, checked and cleared

Suspicion fell on `patches/aros/0036`, which teaches the GalliumCoreAPI glue
generator to emit m68k trampolines:

    move.l __gca_local+N,%a0
    jmp (%a0)

`move.l <abs>,%a0` can assemble absolute-short, which would truncate to 16 bits
and sign-extend -- precisely the observed shape. It does not:

- the encoding is `2079`, `movea.l (xxx).L` -- absolute **long**;
- there are **167** `R_68K_32` relocations against `__gca_local` in the built
  `vc4gallium.hidd`, one per trampoline;
- `__gca_local` is defined, at `0xc4` in **BSS**.

So the mechanism is sound and the loader will fix the addresses up.

That last point is where the suspicion moves rather than ends. A table in BSS
starts as **zeros**, and it is filled when the driver binds to mesa3dgl's half
at `CreatePipeScreen`. Any Mesa-core call made through a trampoline *before*
that binding jumps through a null entry -- into the vector page, which this
port maps DIRECT, so it executes whatever is there rather than faulting.
`Undefined Line0` is what executing that looks like.

Not established: that this is what happens. What is established is that the
table is zero until bound, that nothing stops a trampoline being called before
then, and that the failure mode of doing so matches what gears does.

## The driver is not involved, and gears is executing its own stack

Two facts from the same run, with the serial free and the trap reporter fixed:

**`vc4gallium.hidd` is never loaded.** Not one `[VC4Gallium]` line appears,
and its `InitLib` opens with an unconditional `bug()`. So `CreatePipe` is never
reached, no Gallium driver is chosen, and the GalliumCoreAPI trampolines --
which live in the driver -- cannot be involved in this crash at all. gears
fails before it asks for a GL context.

**The CPU is executing the stack.** In the open-bus report:

    m68kPC 02003d40   A7 02003d34

The program counter is **twelve bytes above the stack pointer**. That is not a
wild pointer into unrelated memory; it is the machine running code out of its
own stack, which is what a smashed return address looks like.

**And it is not the stack's size.** `CreateNewProc` inherits
`cli_DefaultStack` whenever it exceeds `AROS_STACKSIZE`
(`rom/dos/createnewproc.c:139-145`), so the `Stack 1048576` set before the
launch does reach a `Run`-launched process. gears runs with a megabyte and
smashes it anyway. Something writes past a buffer, or through a bad pointer,
onto the stack.

## Worth doing regardless, though it is not this bug

`gca_bind()` fills the consumer table only after every check passes and returns
without filling anything if one fails, so until then every slot is whatever the
table started as -- and it lives in BSS, so that is **zero**. The trampolines
then tail-jump through a null pointer.

On ARM and aarch64 that faults at once. Here it does not: this port maps the
low page -- 68k vectors and AbsExecBase -- DIRECT, so the CPU executes the
vector table instead of faulting. A design that relies on null faulting stops
working, silently, on a target where null is readable.

Pointing every slot at a stub that reports would cost one relocation per slot
at load time and nothing at call time. Not carried as a patch: it is unrelated
to what gears actually does, and the Mesa rebuild it needs is not worth
spending on a safety net while the real fault is open.

## Reading the source instead of running it

The build succeeding says nothing about whether anyone has ever run this
combination. Big-endian m68k is a target Mesa's AROS port was not written for,
so the plumbing was read rather than assumed.

**Endianness is correctly detected, and this was checked rather than trusted.**
`src/util/u_endian.h` has an `__AROS__` branch that keys off `AROS_BIG_ENDIAN`,
which is `1` for this target (`arch/m68k-all/include/aros/cpu.h:17`), and the
header ends in a `#error` if neither macro ends up defined -- so a target it
did not recognise could not have built at all. `HAVE_ENDIAN_H`, which would
shadow that branch, is not defined for this build. Gallium's `p_config.h` does
no detection of its own; it includes `u_endian.h`.

**But m68k is four lines of the whole port.** The AROS Mesa patch
(`Ports/mesa/mesa-20.0.8-aros.diff`, 1197 lines, dated 2021) mentions m68k
exactly once:

    +#if defined(__mc68000__)
    +#define PIPE_ARCH_M68K
    +#endif

That is the entire m68k adaptation. Everything else in that patch is about
AROS as an OS -- and it was written for AROS on little-endian hosts.

**And some of it is a stub, not a port.** Two worth naming, found while
reading:

    #define fpclassify(x) FP_NORMAL      /* include/c99_math.h */
    #define pthread_sigmask(a,b,c)  (1)  /* include/c11/threads_posix.h */

On AROS, Mesa can never detect a NaN or an infinity: `fpclassify` answers
"normal" for every value. That is not an endianness problem and not obviously
this bug, but it is the shape of thing this port is made of, and it is the
answer to "it built, so it works".

So: the macro-level plumbing is right, and the target has simply never been
exercised. That is a reason to keep reading the source at the point of failure
rather than a reason to suspect any particular line.

# Located: OpenLibrary("mesa3dgl20-0.library") from inside gl.library (2026-08-23)

Instrumented downward, each step narrowing the last, with `bug()` on the raw
serial so nothing depends on DOS or on redirection:

1. **`PROGRAM_ENTRIES`** (`patches/aros/0037`, new trace). glinfo climbs to
   entry 3 and never returns. Resolved by symbol spacing against the binary
   (`fromwb`->`stdiowin` = 0x18A, `fromwb`->`initexit` = 0xBC): entry 3 is
   **`__startup_initexit`**.
2. **Inside it**, `set_call_funcs()` is never reached -- so it stops in
   `set_open_libraries()`, the first thing it does. That also explains why
   constructors added at ADD2INIT priority 110 and -110 never printed: the INIT
   set is never called at all.
3. **`libraries.c`'s own trace**, switched on, names the library:

        [Autoinit] Opening libraries...
        [Autoinit] gl.library version 0...

   and never the address that follows. `OpenLibrary("gl.library", 0)` does not
   return.
4. **gl.library's own trace**, switched on, goes all the way through its
   init -- `dos.library`, the ENV: notification process, `StartNotify` on
   `RAM Disk:ENV/sys/GL`, reading the variable -- and stops on the last line
   it prints:

        [GL] GetGLVar: using 'mesa3dgl20-0.library' for mesa3dgl20-0
        [GL] GL_1_Gl_LibOpen()
        [GL] GL_1_Gl_LibOpen: Attempting to use 'mesa3dgl20-0.library' version 0

So the whole failure is one call: **`OpenLibrary("mesa3dgl20-0.library", 0)`,
made from inside gl.library's LibOpen, which is itself inside
`OpenLibrary("gl.library")`, which is inside the program's autoinit.**

## It is not GL at all: a bare OpenLibrary of mesa3dgl never returns

`C:OpenMesa` (`aros/arch/m68k-emu68/c-posixc/OpenMesa.c`) does one thing: takes
a library name, calls `OpenLibrary(name, 0)`, and says what came back. No GL,
no gl.library, no nesting -- its own startup completes visibly, INIT sets and
all, before it makes the call.

    [openmesa] opening 'gallium.library'         18 KB  -> 0x02540e34, v4.0
    [openmesa] opening 'freetype2.library'      574 KB  -> 0x02587e04, v6.10
    [openmesa] opening 'tiff.library'           496 KB  -> 0x028c1c2c, v50.4
    [openmesa] opening 'mesa3dgl20-0.library'    10 MB  -> never returns

So the demos do not fail because of GL, or Gallium, or a driver, or nesting, or
the boot presentation. **A 10 MB disk library cannot be opened on this port**,
and everything above it in this issue is downstream of that.

Both demos reach exactly this call and no further: glinfo and gears are the
same failure, which had not been established until now.

### What this retires

- **The nesting theory**, and the measurement it rested on. `Version
  mesa3dgl20-0.library` costing 19 seconds was read as "the same open works
  plainly". `Version` does not have to open a library to answer -- it can read
  the version string out of the file, and 19 seconds is about what reading
  10 MB costs. That comparison was never an `OpenLibrary`.
- **The notification livelock.** gl.library arms `StartNotify` with
  `NRF_NOTIFY_INITIAL` and its notify process re-reads the variable it watches,
  which looked like a loop that could spin. Counting the rounds settles it: the
  process prints its entry and then sits in `WaitPort` for ever. Zero rounds.
- **glu.library as a size control.** It hangs too, but it depends on
  gl.library, so it went straight back to the same call. freetype2 and tiff are
  the same order of size, depend on nothing here, and both open.

### It is stuck inside a 180-byte read

The ELF loader was traced (`patches/aros/0037`, `DEBUG 1` in
`rom/dos/internalloadseg_elf.c`). Loading mesa3dgl means **6844
`elf_read_block` calls and 4145 hunks** -- Mesa is built with
`-ffunction-sections`, so the library has thousands of sections and most of the
reads are tiny (12, 24, 36, 180 bytes).

That looked at first like a duration problem: 21455 trace lines in the first
45 seconds, and the file offsets climbing to 96%. It is not. Given 25 minutes:

- the log stops growing entirely at **26167 lines**;
- `elf_read_block` calls stop at **8363**, and stay there -- zero in a
  measured 121-second window;
- QEMU sits at **391% host CPU** for the whole time, so it is executing, not
  waiting;
- the last line is `elf_read_block (offset=8928, size=180)`, entered and never
  returned.

**A 180-byte read at a low offset does not come back.** Everything above this
in the issue -- GL, Gallium, nesting, the boot presentation, the stack -- is
downstream of a read that wedges. Small libraries load because they make few
enough reads not to reach it; freetype2 at 574 KB is fine, and mesa3dgl at
10 MB is not.

That moves this out of graphics entirely and into the storage path: DOS Read,
the FAT handler, the SD driver. It is also, unlike everything else tried here,
territory this repository patches -- `0003` and `0024` on sdcard, `0027` on
sdhost, `0006`/`0008`/`0023` on FAT.

Checked and cleared so far: `0024` reports the PIO data loop's cost **per
megabyte, not per transfer**, precisely so the instrument does not change what
it measures, and it prints nothing in these runs. The FAT handler does cache
FAT blocks (`fat_cache_block` in `rom/filesys/fat/fat.c`), so a seek is not
re-reading the table from the card.

### What is still open

Whether the 10 MB open is stuck or merely far slower than anything else has
been given ten minutes at 390% host CPU, against 574 KB opening promptly. It is
not an idle wait: something is executing.

The next place to look is the loader itself -- `rom/dos/internalloadseg_elf.c`
-- which already carries `D()` traces through section reading and relocation.
Whether it is crawling through relocations or looping in one is one build away,
and the reproducer is now a single command with no graphics in it.

## Why the nesting looked interesting (superseded)

The same open, made plainly from the shell, **works**: `Version
mesa3dgl20-0.library` in `gl-probe` costs 19 seconds -- the 10 MB load -- and
completes. Nested inside another library's open it does not, and QEMU sits at
380% host CPU throughout, so whatever this is, it is not an idle wait on a
semaphore.

Not yet distinguished: hung, or merely far slower in this context. The plain
open is bounded and measured; this one has been given five minutes.

## Next

1. Give the nested open a much longer window and find out whether it ever
   returns. Everything else depends on the answer, and it is one run.
2. If it returns: this is a performance problem in a nested disk-library open,
   not a GL problem, and the demos work by waiting.
3. If it does not: compare what is different about the nested path -- ramlib
   re-entry while the outer open is in progress is the obvious candidate, and
   it would be a defect that has nothing to do with GL.
4. Retire the earlier "find what writes onto the stack" line for glinfo: it
   never reaches its own code, so nothing of glinfo writes anything. That
   observation belongs to gears, which is a separate failure and still open.

## Superseded

1. Find what writes onto the stack. This is now a memory-corruption hunt in
   GLUT/Mesa startup, not a GL driver problem: `mungwall` and `stacksnoop`
   both exist and neither has been tried on this.
2. Resolve `pc=0202012e` to a module. It is in the heap, so it needs the load
   address of whatever is mapped there -- the `RamLib`/`LoadSeg` trace can
   give it.
2. Find where the render target's base comes from and why it is not the
   framebuffer.
3. `glinfo` and `gears` are still not established as the same fault. One spins
   without faulting; the other executes data within a second. Do not merge them
   until something ties them together.

# Acceptance criteria

- `glinfo` identifies the VC4/Gallium renderer on a real Pi 3.
- `gears` renders continuously and closes cleanly.
- No TLSF corruption, alert, command-submit timeout or stuck V3D wait occurs.

