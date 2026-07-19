# Suggestions For The Rigel Team

## Scope

This document lists improvements that appear valuable on the `rigel` side after
integrating it as an alternative Bellatrix chipset backend.

This is intentionally separate from Bellatrix architecture decisions. The goal
here is to capture suggestions for `rigel` itself:

- API ergonomics
- integration surfaces
- documentation
- video output options
- multicore-host friendliness

## Summary

Rigel already has a strong core model:

- deterministic
- single-threaded
- explicit time ownership
- explicit MMIO surface
- explicit IRQ and bus observation APIs

That foundation is good. The main opportunities are not about changing Rigel
into a different kind of system, but about making it easier to integrate cleanly
into hosts such as Bellatrix.

## 1. Tighten documentation consistency

The documentation is useful, but some files are not fully synchronized.

Examples observed:

- `docs/video_output.md` still describes some frame metadata and output features
  as future work
- `docs/api_status.md` marks part of that work as already done or partially done

Suggested improvement:

- define one file as the canonical integration status document
- keep feature-state claims synchronized between:
  - `README.md`
  - `docs/video_output.md`
  - `docs/api_status.md`
  - `docs/integration.md`

This matters because host integrators use those files to decide whether to build
around a feature or to keep local glue.

## 2. Add a dedicated "host integration patterns" document

Rigel documents the basic host loop well, but there is room for a more explicit
integration guide for different host styles.

Suggested additions:

- minimal host pattern
  - single-threaded
  - poll `RIGEL_EVENT_IRQ_CHANGED`
  - poll `RIGEL_EVENT_FRAME_READY`

- bus-accurate host pattern
  - uses `rigel_get_next_deadline()`
  - uses `rigel_get_next_bus_change()`
  - respects `cpu_would_stall`

- cross-core host pattern
  - CPU side decodes bus accesses
  - chipset side owns Rigel context
  - internal request/reply queue examples
  - frame handoff expectations
  - IRQ publication expectations

This would help hosts like Bellatrix without changing Rigel's public model.

## 3. Clarify the intended contract for cross-core use

Rigel is clearly designed as single-threaded internally, which is good.
What is less explicit is how host authors should treat Rigel when it lives in a
separate execution domain from the CPU.

Suggested documentation improvements:

- explicitly state that Rigel itself is single-owner
- explicitly state that hosts should not call into one context concurrently
- document recommended ownership models:
  - one thread/core owns the context
  - other threads communicate through host-side queues

- document frame lifetime and visibility expectations for async consumers

This is mainly a documentation and guidance problem, not a change in Rigel's
core philosophy.

## 4. Improve video output flexibility

The current video model is workable, but host integration would be easier with a
little more configurability.

Most useful improvements:

- selectable pixel format at `rigel_create()` time
  - `RGBA8888`
  - `RGB565`
  - possibly indexed output for tooling/debug

- optional host-provided framebuffer target
- clearer stable contract for frame buffer lifetime
- eventual internal double buffering for async presentation

Why this matters:

- some hosts want generic RGBA
- some bare-metal hosts want RGB565 directly
- full-frame post-conversion in the host adds extra bandwidth and CPU cost

This does not mean moving scaling or display policy into Rigel. It only means
letting Rigel export pixels in a host-friendly format.

## 5. Document frame semantics more explicitly

The important questions for hosts are:

- exactly when `RIGEL_EVENT_FRAME_READY` fires
- how that relates to VBLANK
- how long `rigel_get_frame()` output stays valid
- whether the host can retain the pointer after stepping again
- whether line 0 may already be overwritten on the next step

Some of this is documented, but it would help to centralize it as a precise
"frame delivery contract".

Suggested additions:

- define `FRAME_READY` as a delivery event, not just a beam milestone
- state whether a host may defer presentation by one or more host frames
- define expectations for future double-buffer support

## 6. Consider explicit host callbacks as an optional layer

This is optional, not required.

Rigel is fine as a polling-oriented library, but an optional callback-friendly
adapter layer could simplify hosts that want lower overhead or cleaner code.

Examples:

- frame-ready callback
- IRQ/IPL changed callback
- serial TX available callback
- audio sample/block ready callback

Important constraint:

- this should be an optional host convenience layer
- it should not replace the existing polling APIs
- the polling APIs should remain the canonical low-level contract

## 7. Expose stronger integration guidance for serial and input

The current serial and input APIs are usable, but host behavior would benefit
from more explicit examples and expectations.

Suggested documentation/examples:

- serial bridging example
  - host RX to `rigel_serial_receive_byte()`
  - host TX drain using `rigel_serial_tx_available()` and `rigel_serial_pop_tx_byte()`

- mouse example
  - how hosts should accumulate quadrature-style movement into `JOYxDAT`

- joystick example
  - how fire and pot button APIs map to typical host inputs

- keyboard example
  - how raw Amiga keycodes should be sourced and injected

This is mainly about reducing guesswork for host implementers.

## 8. Make snapshot status harder to misuse

`docs/api_status.md` already hints that snapshot support is incomplete.
That should be made harder to misunderstand from the public API surface.

Suggested improvements:

- mark snapshot support clearly as incomplete in headers
- document exactly which state is currently preserved
- document that it is not yet suitable for real save-state use

This avoids hosts building accidental dependencies on partial snapshot behavior.

## 9. Add a small "integration checklist" for hosts

A concise checklist would be valuable for host teams.

Example items:

- provide Chip RAM callbacks
- forward only custom-register offsets, not global addresses
- step Rigel using the same timebase as the CPU
- publish IPL on `RIGEL_EVENT_IRQ_CHANGED`
- respect frame pointer lifetime
- decide whether bus-stall observation is required
- drain serial TX if serial is enabled
- define presentation conversion policy explicitly

This would reduce repeated bring-up mistakes.

## 10. Keep the boundary strict

This is less a new feature and more a recommendation to preserve what is
already one of Rigel's strengths.

Rigel should continue to avoid taking ownership of:

- the CPU
- the global memory map
- host display policy
- scaling and overlay logic
- platform-specific I/O policy

That separation is one of the reasons it is attractive as a Bellatrix backend.

## 11. Consider an optional event-skip fast path for the Agnus slot loop

Investigating a bare-metal (Raspberry Pi 3B) performance gap, we read
`rigel_step()` → `rigel_chipset_step()` → `rigel_agnus_step()` →
`agnus_slot_scheduler_step_until()` end to end
(`src/chipset/agnus/timing/slot_scheduler.c`). The loop is:

```c
for (i = 0; i < cycles; i++)
    agnus_slot_scheduler_step(sched, ctx, line_clocks, frame_lines);
```

Every CCK in the step range pays the full cost of
`agnus_slot_scheduler_step()` — slot ownership resolution, slot dispatch,
`rigel_beam_domain_step()`, and `rigel_denise_step()` — even CCKs with no
DMA, no sprite, and no pending Denise/Copper/Blitter state change. This is
a deliberate, documented choice (`AI_context/dma_slot_timing.md`,
"Approach C": cycle-exact by construction), not an oversight — and it is
almost certainly the right default. On our side, hardware measurements
(4.7x below the target frame rate on a Pi 3B, chipset-bound and identical
across two different CPU backends) point at this loop as the largest
single structural cost, consistent with how much cheaper line-based
schedulers (e.g. older UAE forks that decide per-line rather than per-CCK)
report being on comparably weak hardware.

`agnus_slot_scheduler_next_event()` already exists and can answer "how far
to the next non-free/non-CPU slot" — the ingredient for a jump-ahead is
already there, just not used by the step loop itself.

**Important constraint — this must not be the default and must not touch
correctness**: cycle-exact behavior is a real requirement for us (Copper
and sprite-multiplexing tricks used by demoscene-style software we target,
e.g. Battle Squadron, depend on it), and we do not want a speed/accuracy
trade made silently or by default. If this is explored at all, it should
be:

- strictly opt-in (compile-time flag or a runtime mode, analogous to a
  `RIGEL_FAST`/`RIGEL_ACCURATE` switch), defaulting to the current
  cycle-exact behavior;
- scoped to CCKs where no domain has observable state to update (no DMA
  slot claimed, no sprite/bitplane fetch pending, no Copper/Blitter
  activity, no beam milestone in range) — i.e. skip should be provably
  equivalent to stepping one-by-one, not an approximation;
- validated against the existing Musashi harness tests
  (`test_vblank.c`, `test_blitter_stall.c`, `test_audio_period.c`) with
  the fast path enabled, to catch any accuracy regression before it ships.

We are not asking for this to be prioritized — flagging it as the most
concrete lead from our side, for the Rigel team to judge given full
visibility into the domain models.

## Priority Suggestions

If the Rigel team wants a short prioritized list, this is the order I would use:

1. Synchronize docs and define one canonical status view.
2. Add a dedicated host integration guide, including cross-core host patterns.
3. Add configurable video output formats, especially `RGB565`.
4. Clarify frame lifetime and `FRAME_READY` semantics.
5. Mark snapshot support more aggressively as incomplete.

## Final Assessment

Rigel does not appear to need a change in philosophy.

The strongest improvements are:

- better integration guidance
- better video export options
- cleaner host-facing ergonomics
- tighter documentation discipline

The core design already looks like a good chipset backend foundation. The next
step is making host integration more explicit and less guess-heavy.

