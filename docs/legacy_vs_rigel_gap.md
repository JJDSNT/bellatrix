# Legacy Chipset vs Rigel Gap

## Scope

This document lists what the current Bellatrix legacy chipset path already has
today that the current Rigel path does not yet match in practice.

This is a Bellatrix-oriented parity document, not a generic judgment of Rigel as
a chipset library. Some items below are:

- true chipset gaps
- Bellatrix integration gaps
- validation and tooling gaps

Those three categories matter equally when the question is:

"Can we use Rigel today as a drop-in replacement for the legacy Bellatrix path?"

Current answer: not yet.

## Important framing

The legacy path is not just "chipset logic". It is the combination of:

- chipset implementation
- Bellatrix-specific glue
- harness behavior
- debug facilities
- test coverage
- boot-proven behavior

Rigel can be strong as a chipset core and still be behind in Bellatrix parity if
the surrounding integration surface is missing or immature.

## Short version

Rigel is currently behind the Bellatrix legacy path in:

- Bellatrix-specific integration maturity
- multicore readiness inside Bellatrix
- debug and observability tools
- validation coverage
- host-facing conveniences needed by the harness and SDL path
- feature parity confidence, even where the core capability may exist upstream

## 1. Multicore runtime parity

### Legacy has

The Bellatrix legacy path already has a concrete Bellatrix-side multicore runtime
model:

- CPU core
- GFX/Agnus core
- Paula/audio core
- IO/CIA core

This is wired into Bellatrix runtime code and boot flow.

### Rigel currently lacks

In Bellatrix, Rigel is currently single-core only.

Practical consequences:

- no Bellatrix multicore runtime parity
- no dedicated chipset-core flow implemented end-to-end
- no production Bellatrix scheduler around `rigel_step_until()` yet

Even if Rigel's architecture is cleaner for a future dedicated chipset core,
today the legacy path is materially ahead because it already has a working
Bellatrix multicore story.

## 2. Harness maturity and test parity

### Legacy has

The legacy path has accumulated harness assumptions and tests that directly know
about legacy internals:

- overlay integration tests
- direct Agnus/Paula/CIA readback expectations
- serial injection behavior through legacy Paula
- audio sampling through legacy Paula
- many smoke/debug workflows were originally built around `machine.c`

### Rigel currently lacks

We only just enabled a real Rigel-backed harness path.

Practical consequences:

- parts of the harness test suite are still legacy-only
- integration tests such as `bellatrix_integration_overlay` are not valid as-is
  for Rigel
- many existing debugging workflows still implicitly assume legacy structs and
  legacy helper paths

This is a major reason Rigel feels behind today: the validation ecosystem around
the legacy path is much more mature.

## 3. Debug and observability parity

### Legacy has

The legacy Bellatrix path has a lot of practical observability built around it:

- btrace/probe usage aligned with legacy dispatch
- bus-level tracing and first-N tracing
- watchdog-oriented dumps
- PC trap style instrumentation
- direct inspection of Agnus/Paula/CIA/Denise structs
- tests and diagnostics that read specific legacy fields

### Rigel currently lacks

Rigel has its own internal architecture and event model, but Bellatrix does not
yet have equivalent day-to-day debug visibility for the Rigel path.

Practical consequences:

- harder to confirm whether a glitch is in:
  - Rigel itself
  - Bellatrix integration
  - CPU/memory-map glue

- fewer direct probes aligned with the way Bellatrix developers currently debug

This is one of the biggest practical disadvantages of Rigel right now.

## 4. Bellatrix-specific glue parity

### Legacy has

The legacy path was built together with Bellatrix, so many Bellatrix behaviors
already line up naturally:

- overlay handling through CIA-A conventions used by Bellatrix
- direct host serial bridging through Paula structures
- direct harness audio reads through Paula structures
- direct machine helper assumptions in tests and tooling
- direct compatibility with Bellatrix's existing runtime code

### Rigel currently lacks

Rigel has a cleaner chipset boundary, but Bellatrix still depends on a lot of
legacy-shaped glue.

Examples of what needed extra work already:

- serial RX/TX wrappers
- audio wrappers
- backend-name reporting
- controller/input helper neutrality
- harness-specific path cleanup

This means the legacy path is still ahead in "it already fits Bellatrix the way
Bellatrix currently works".

## 5. Video output parity for Bellatrix hosts

### Legacy has

The legacy path is already aligned with Bellatrix host presentation expectations,
especially in the harness and framebuffer-oriented flows.

### Rigel currently lacks

Rigel currently exports `RGBA8888` frame data and Bellatrix converts it after
the fact for host presentation.

Practical consequences:

- extra conversion step in Bellatrix
- more memory traffic
- less direct fit for RGB565-oriented host paths
- weaker fit for a future dedicated chipset core if full frames are copied back

This does not mean Rigel lacks a video pipeline. It means Bellatrix video
integration parity is still behind the legacy path.

## 6. Feature confidence parity

### Legacy has

The legacy path has the advantage of being the path most Bellatrix debugging and
boot bring-up has already exercised.

Even where it has bugs, the team knows:

- where to instrument it
- how to reproduce problems
- which tests and logs are meaningful

### Rigel currently lacks

Even when Rigel has documentation or upstream claims for a feature, Bellatrix
still lacks enough parity confidence in several areas.

That includes confidence around:

- boot paths
- exact MMIO behavior as Bellatrix expects it
- harness reproducibility
- parity of debug workflows
- cross-checking glitches against the legacy path

This "confidence gap" is one of the real gaps, not just a perception issue.

## 7. Snapshot/state tooling parity

### Legacy has

The legacy path is not perfect here either, but Bellatrix development mostly
operates through known in-memory structures and direct instrumentation.

### Rigel currently lacks

Rigel's snapshot story is still explicitly incomplete.

Practical consequences:

- not suitable as a robust save-state base
- harder to build advanced debugging workflows around frozen/restored state

This is not the biggest parity gap today, but it is still a meaningful one.

## 8. Legacy-specific tests depend on legacy-specific semantics

This is worth calling out separately because it can easily create false signals.

Some Bellatrix tests and harness checks are not merely "chipset tests". They are
tests of Bellatrix's legacy chipset implementation shape.

Examples:

- direct struct reads
- direct Paula/Agnus helper calls
- overlay behavior validated through legacy interfaces
- readback expectations tied to legacy internal state

So when Rigel fails those today, that does not always mean Rigel's chipset core
is wrong. It often means Bellatrix lacks a Rigel-native validation surface.

The legacy path is still ahead because that validation surface already exists.

## 9. What is likely real missing parity vs what is mostly tooling

### Likely real missing parity

- Bellatrix multicore parity
- host-facing video format parity
- Bellatrix-native debug parity
- harness integration parity
- production-quality dedicated chipset-core flow

### Mostly tooling and ecosystem parity

- test coverage
- struct-level debug workflows
- direct instrumentation convenience
- clear regression confidence

Both matter. The second category is exactly why Rigel can feel "well behind"
even if some upstream core features already exist.

## 10. Practical conclusion

From Bellatrix's point of view, the legacy path is still ahead because it has:

- better integration with the rest of Bellatrix
- better validation coverage
- better debugging ergonomics
- an actual Bellatrix multicore story
- fewer unknowns in day-to-day bring-up

Rigel currently looks more like:

- a promising alternative chipset core
- with cleaner long-term architecture
- but still missing a lot of Bellatrix-specific maturity around it

## Recommended next parity targets

If the goal is to make Rigel competitive with the legacy path inside Bellatrix,
these are the highest-value targets:

1. Make the Rigel harness path first-class and reproducible.
2. Build Rigel-native debug/trace workflows comparable to the legacy path.
3. Add Bellatrix-appropriate video output options, especially `RGB565`.
4. Define and implement a Bellatrix dedicated-chipset-core model for Rigel.
5. Expand Bellatrix test coverage specifically for the Rigel path instead of
   relying on legacy-shaped tests.

## Final assessment

Yes: in practical Bellatrix terms, Rigel is still well behind the legacy path.

That does not necessarily mean its core chipset design is weaker. It means the
total Bellatrix-ready package around the legacy path is still substantially more
mature than the current Rigel integration.

