---
id: ISSUE-0014
title: "Mouse input not reaching Amiga under QEMU"
status: backlog
priority: medium
type: bug
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-26
tags:
  - qemu
  - mouse
  - input
  - usb-hid
related_files:
  - src/machine/machine_rigel.c
  - src/machine/machine_rigel_step.c
  - src/io/usb/usb_hid_bellatrix.c
  - run.sh
---

# Issue: Mouse input not reaching the Amiga under QEMU

## Status: NEEDS RE-EVALUATION (2026-06-26)

Não avaliado desde os fixes recentes de sprite/mouse (commits `b69ed3c`,
`4847e26`). Pode ter sido corrigido como efeito colateral do mouse latch e
JOY0DAT fixes. Reavaliar em próxima sessão de desenvolvimento com QEMU.

## Description

The user reported that, while debugging mouse handling in the Musashi
harness (see ISSUE-0008 for that investigation), mouse input stopped
reaching the emulated Amiga when running under QEMU (`./run.sh qemu`). This
needs its own investigation — it is a different runtime path from the harness
(`tools/harness/main.c` + `src/host/posix/`) and was not itself the target
of the harness debugging session.

## Context from the harness session

During the same session, the following mouse-related changes landed in
shared code (used by both the harness and the bare-metal/QEMU build):

- `src/machine/machine_rigel.c` / `machine_rigel_step.c`: added a per-VBL
  rate limiter in `bellatrix_machine_mouse_motion()` (caps net motion
  delivered to the controller port to +-100 per frame, queues the rest via
  `machine_mouse_frame_tick()`), and removed the old `[MOUSE]` kprintf debug
  trace.
- `src/io/usb/usb_hid_bellatrix.c`: the USB HID mouse callback was refactored
  (pre-existing change, not made in this session) to route through
  `bellatrix_machine_mouse_motion()` / `bellatrix_machine_mouse_button()`
  instead of calling `bellatrix_controller_port_*` directly.
- `run.sh` has an uncommitted change flipping `BELLATRIX_USB_POINTER` default
  from `tablet` to `mouse` (QEMU's emulated USB pointer device: absolute vs.
  relative). This predates the harness debugging session and was not
  evaluated for its effect on QEMU input.
- Earlier in the session, debug `fprintf()` calls were temporarily added
  directly to `external/rigel` chipset files (`sprites.c`, `agnus_write.c`,
  `custom_regs.c`) without a platform guard. This broke mouse input on a
  QEMU/bare-metal run (the user reported "now nothing reaches the Amiga").
  Root cause: those files don't have a safe stdio backend on bare metal.
  All of those unguarded traces were removed before this issue was filed;
  any remaining instrumentation in `external/rigel` uses the project's
  existing `RIGEL_ENABLE_STDLIB_ENV` guard (see `copper_exec.c` for the
  established pattern), which compiles out entirely on bare metal.

If the QEMU regression persists after that revert, the rate limiter or the
USB HID refactor are the next things to check — not the now-removed debug
prints.

## Next steps

- Reproduce with a clean `./run.sh qemu` build using the current tree.
- Confirm whether `BELLATRIX_USB_POINTER=mouse` vs `tablet` changes anything
  for QEMU's emulated pointer reaching `usb_hid_bellatrix.c`.
- Confirm HID mouse reports are still arriving at
  `bellatrix_usb_hid_mouse_callback()` (CherryUSB host stack side) and that
  `bellatrix_machine_mouse_motion()` is being called with nonzero `dx`/`dy`.
- Check whether the per-VBL rate limiter (`machine_mouse_frame_tick()`,
  called from `bellatrix_machine_on_frame_ready()`) is actually being invoked
  on the QEMU/bare-metal path — `RIGEL_EVENT_FRAME_READY` must still fire
  there the same way it does in the harness.
