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

## The instrument is not trustworthy yet

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

# Acceptance criteria

- `glinfo` identifies the VC4/Gallium renderer on a real Pi 3.
- `gears` renders continuously and closes cleanly.
- No TLSF corruption, alert, command-submit timeout or stuck V3D wait occurs.

