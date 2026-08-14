---
id: ISSUE-0021
title: "Keep BootUI visible until Wanderer has produced useful output"
status: in_progress
priority: medium
type: research
owner: agent
created_at: 2026-08-14
updated_at: 2026-08-14
tags:
  - boot
  - graphics
  - bootui
  - wanderer
blockers:
related_files:
  - aros/arch/m68k-emu68/boot/bootui.c
  - aros/arch/m68k-emu68/hidd/emu68gfx/emu68gfx_hiddclass.c
  - aros/arch/m68k-emu68/c/BootProgress.c
  - patches/aros/0015-s-publish-late-emu68-boot-progress.patch
  - scripts/boot-timing.py
---

# Summary

Eliminate the visually blank interval between BootUI handing the display to
the first Workbench Screen and Wanderer drawing useful desktop content such as
its icons, and show an elapsed-time counter while BootUI is visible.

# Problem

BootUI currently calls `emu68_bootui_takeover()` when the Emu68 display HIDD
receives its first `Show()` with a bitmap. This ownership boundary is safe: it
prevents BootUI and the graphics stack from writing the physical framebuffer
concurrently. It is not, however, the right visual completion boundary.

The first Workbench Screen can be installed before Wanderer has populated it.
BootUI therefore disappears, a mostly empty Screen is displayed, and the user
waits without useful feedback until the icons and final title appear. A measured
2026-08-14 QEMU run opened the first Screen at 38.8 s and detected Wanderer's
icons at 55.5 s. The 5 s sampling interval makes the exact gap approximate, but
the distinct empty-Screen phase is directly observed in the captured frames.

# Goal

Keep useful visual feedback on screen until the first AROS display is ready to
show meaningful content, including a live monotonic elapsed-time counter, then
perform a race-free, visually clean handoff.

# What was done

- BootUI draws from early 68k entry through graphics and Startup-Sequence
  milestones.
- `bootui.resource` and `C:BootProgress` allow late boot code to publish stages.
- The display HIDD currently stops BootUI immediately before forwarding its
  first real bitmap through `Show()`.
- The current ownership rule and the remaining empty-Screen gap were reproduced
  under QEMU and recorded by `scripts/boot-timing.py`.

# What is left

- Identify a reliable definition of "visually ready" that does not depend on a
  fixed delay.
- Determine whether readiness should be published explicitly by Wanderer or
  inferred at the display HIDD from rendering/display activity.
- Prototype the framebuffer ownership mechanism needed to retain BootUI after
  Intuition creates its first Screen.
- Add a compact `MM:SS` elapsed-time counter to BootUI. Its epoch must be the
  earliest monotonic timestamp available to BootUI, and it must continue across
  milestone updates without resetting.
- Select an update path that does not busy-wait or make normal boot progress
  depend on drawing the counter. A one-second visible cadence is sufficient.
- Measure the final handoff and ensure that it does not regress boot time or
  expose partially drawn frames.

# Candidate designs

## Delay physical takeover with an off-screen AROS display

Let graphics/Intuition/Wanderer render into a separate bitmap while BootUI
retains the physical framebuffer. When readiness is published, copy or flip the
completed bitmap to the display and disable BootUI. This gives the cleanest
handoff but needs additional memory and careful integration with the display
HIDD.

## Keep a BootUI overlay after the first Screen

Allow the first Screen to own the framebuffer but make the graphics backend
redraw or preserve a BootUI overlay until a late readiness milestone. This may
be simpler, but it couples BootUI to normal graphics operations and risks
flicker or competing writes unless ownership is explicit.

An arbitrary timer is not a candidate for deciding the handoff: boot duration
varies and a time-based bar or handoff would only hide the synchronization
problem. The visible elapsed-time counter is informational only and must not
participate in readiness or framebuffer-ownership decisions.

# Decisions taken

- The first `Show()` remains the safe handoff boundary for the current proof of
  concept; it must not simply be delayed while both producers can write the
  same framebuffer.
- A durable solution must use a real readiness signal and a single framebuffer
  owner at every instant.
- The elapsed-time counter uses a monotonic clock, is displayed as `MM:SS`, and
  never controls the handoff.
- This issue is kept separate from visual styling and milestone calibration.

# Acceptance criteria

- [ ] BootUI remains visible while the initial Workbench Screen is empty.
- [ ] BootUI displays an elapsed-time counter in `MM:SS` while it owns the
      framebuffer.
- [ ] The counter advances approximately once per second, never moves
      backwards, and is not reset by milestone changes.
- [ ] Counter updates do not busy-wait, delay boot, or determine when handoff
      occurs.
- [ ] The transition occurs only after useful Wanderer content is ready.
- [ ] No partially drawn Screen, flicker, tearing, or concurrent framebuffer
      writes are visible.
- [ ] The handoff does not depend on a fixed elapsed-time delay.
- [ ] QEMU timing shows no relevant boot-time regression.
- [ ] The behavior is verified on the GUI path and documented with captures.

# Execution log

- 2026-08-14 — issue opened after validating the first BootUI proof of concept.
  The loader-to-empty-Screen transition remains safe but does not cover the
  final wait for Wanderer content.
- 2026-08-14 — expanded the scope to include an informational monotonic
  `MM:SS` counter for the entire interval in which BootUI remains visible.
- 2026-08-14 — implemented the `MM:SS` counter using the BCM2835 system
  timer's free-running microsecond counter. The existing heartbeat only
  triggers a redraw when the displayed second changes; elapsed time is not
  derived from interrupt counts, so delayed interrupts do not hide a slow
  boot. The counter clamps at `99:59`, is redrawn after every milestone, and
  stops naturally when framebuffer ownership is transferred.
