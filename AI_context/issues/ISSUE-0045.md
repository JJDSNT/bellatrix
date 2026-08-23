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

## Two defects found on the way, both ours

**1. Our display driver hands back a VC4 Gallium object without checking there
is a V3D.**

`createpipe.c:98-111` asks the *display* to create the Gallium object and only
falls back to `softpipe.hidd` if that returns NULL. Our
`vcgfx_hiddclass.c:760-774` answers by opening `vc4gallium.hidd` and creating
`hidd.gallium.vc4` whenever the library opens -- which it always does, because
it is on the card. The comment there says "else: object stays NULL; CreatePipe
will use its softpipe fallback", but that branch is only reached when
`OpenLibrary` *fails*.

Upstream's driver does have the probe -- `vc4_v3d_init()`
(`vc4_v3d.c:236-266`) reads V3D IDENT0, reports `V3D not found`, sets
`v3d_available = FALSE`, and `CreatePipeScreen` then returns NULL
(`vc4_galliumclass.c:278`). So the intended fallback exists end to end. Ours
just never lets it decide, because we commit to VC4 one level higher.

This is the same discipline as ISSUE-0049's release hook: claim a capability
only where it is real. It is not established that this *causes* the hang -- we
never get far enough to know -- but a machine with no V3D should be on softpipe
and is not.

**2. `BELLATRIX_BOOT_TEST` without `_LATE` runs the probe too early to speak.**

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
2. Make `vcgfx` probe V3D before answering with a VC4 object, so a machine
   without one lands on softpipe by design rather than by accident.

# Acceptance criteria

- `glinfo` identifies the VC4/Gallium renderer on a real Pi 3.
- `gears` renders continuously and closes cleanly.
- No TLSF corruption, alert, command-submit timeout or stuck V3D wait occurs.

