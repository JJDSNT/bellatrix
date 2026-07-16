# Rigel Integration Notes

> **Status (superseded core mapping, kept for the Rigel-integration content
> below):** the `CPU core + chipset core` shape this document recommended has
> been implemented, but the specific core numbers above are stale. The
> current, temporary stabilization placement is Core 0 = CPU (Emu68 or
> Musashi), Core 2 = full Rigel chipset domain, Core 3 = host I/O (USB +
> Bluetooth), Core 1 = auxiliary/parked — see
> [`runtime_and_timing.md`](runtime_and_timing.md) and
> `AI_context/issues/ISSUE-0058.md`. The target architecture keeps Core 0 as
> Control, with the CPU moving elsewhere once Emu68 stabilizes.

## Context

This note summarizes the parts of `external/rigel` that matter most for Bellatrix,
plus an assessment of `external/libamivideo` and where it fits better.

Primary local references reviewed:

- `external/rigel/README.md`
- `external/rigel/docs/architecture.md`
- `external/rigel/docs/integration.md`
- `external/rigel/docs/video_output.md`
- `external/rigel/docs/api_status.md`
- `external/libamivideo/README.md`

## What Rigel is trying to be

Rigel is documented as a deterministic, single-threaded chipset library with a
clean host boundary:

- Host owns CPU, ROM, Fast RAM, global address map, and presentation.
- Rigel owns chipset behavior.
- Host talks to Rigel through a temporal API, MMIO API, IRQ API, and peripheral APIs.

This is a strong fit for Bellatrix if we want an alternative chipset backend
without deleting the current legacy implementation.

## Important clarification: guest bus vs internal emulator boundary

Rigel does not need, and should not expect, a high-level guest-facing service
API in place of Amiga memory accesses.

From the guest point of view:

- demos, games, Kickstart, and drivers still access Amiga addresses directly
- custom registers are still reached through normal MMIO addresses
- CIA, RTC, and Chip RAM still behave like bus-visible hardware

So the external interface remains the Amiga memory map.

The only place where a "service boundary" makes sense is inside Bellatrix if we
choose to split CPU execution and chipset execution across different cores.

In that design:

- the guest still performs bus accesses by address
- the CPU side of Bellatrix decodes those accesses
- the chipset side of Bellatrix owns Rigel state and services the decoded operations

So the protocol is internal to the emulator, not a replacement for guest MMIO.

## Most relevant Rigel concepts for Bellatrix

### 1. Ownership boundary

The intended integration model is:

- Bellatrix CPU decodes the global address map.
- Bellatrix forwards only chipset-local operations to Rigel.
- Bellatrix does not inspect or mutate Rigel internals directly.

This is the right direction for a future multicore design. It is cleaner than
the current legacy Bellatrix path, where many modules still assume direct access
to Agnus, Paula, CIA, and Denise structs.

### 2. Temporal API

The most important Rigel API for long-term integration is not MMIO, but time:

- `rigel_get_time()`
- `rigel_get_next_deadline()`
- `rigel_step()`
- `rigel_step_until()`
- `rigel_get_next_bus_change()`
- `rigel_get_cpu_resume_time()`

For a serious scheduler, this matters more than raw `read16/write16`. It gives
Bellatrix a way to let one execution domain own chipset progression instead of
sprinkling ad-hoc ticks everywhere.

### 3. Bus observation

Rigel explicitly models bus contention and stall advice:

- `rigel_get_bus_state()`
- `rigel_cpu_can_access_chip_ram()`
- `rigel_get_cpu_resume_time()`

This is especially relevant if Bellatrix wants Emu68 or Musashi to respect Chip
RAM access timing and BLTPRI more faithfully.

### 4. Video boundary

Rigel documentation is explicit that:

- planar to chunky is part of Denise behavior
- host presentation is separate
- scaling, aspect correction, overlays, and vsync belong to the host

This is the correct split for Bellatrix too.

### 5. Peripheral-facing API

The current public APIs that matter most for Bellatrix are:

- custom MMIO: `rigel_custom_read16/write16`
- IRQ: `rigel_get_intreq`, `rigel_get_intena`, `rigel_get_ipl`
- CIA: `rigel_cia_read/write`
- serial: `rigel_serial_receive_byte`, `rigel_serial_tx_available`, `rigel_serial_pop_tx_byte`
- keyboard: `rigel_keyboard_inject`
- input: `rigel_input_*`
- floppy: `rigel_floppy_*`
- RTC: `rigel_rtc_*`

This is enough to build a backend, but some Bellatrix glue is still needed
around overlay, memory-map ownership, and host services.

## What still feels missing from Rigel for Bellatrix

### 1. Cleaner host callbacks or adapter points

Rigel exposes polling-style APIs, which are usable, but Bellatrix would benefit
from a slightly more host-oriented integration surface for:

- IRQ/IPL changes
- frame-ready notification
- serial TX drain
- media change completion

Polling works. A more explicit event/callback layer would reduce Bellatrix glue
and make a dedicated chipset core easier to schedule.

### 2. Better documented integration for async or multicore hosts

Rigel is well documented as single-threaded and deterministic, but not yet as a
chipset library hosted behind an internal cross-core boundary.

For Bellatrix multicore, the key missing design doc is:

- who owns the authoritative timebase
- how decoded MMIO/bus requests are queued internally
- how replies are synchronized
- how frame buffers are handed off safely
- what can be observed lock-free and what requires serialization

### 3. More explicit video output options

Today Rigel exposes RGBA8888 frame output. That is workable, but Bellatrix on
bare metal often wants RGB565 or a host-provided target.

From Bellatrix's perspective, the most useful additions would be:

- configurable output format at `rigel_create()` time
- `RGB565` output option
- host-provided target buffer option
- stable double-buffered frame handoff semantics

### 4. Documentation consistency

The Rigel docs are good, but not fully synchronized.

Examples:

- `docs/video_output.md` still describes `flags`, `delta`, and pixel format work
  partly as future extensions
- `docs/api_status.md` marks some of these as already implemented or partially done

For Bellatrix integration work, `docs/api_status.md` appears closer to current
reality than `docs/video_output.md`.

## Video conversion: Bellatrix or Rigel?

Short answer:

- Denise execution stays in Rigel.
- Final host-facing pixel format should ideally be selectable in Rigel.
- Presentation policy stays in Bellatrix.

### What should remain in Rigel

- bitplane fetch interpretation
- palette application
- HAM/EHB behavior
- dual-playfield composition
- sprite/playfield priority

This is not "format conversion". This is the chipset doing real work.

### What should remain in Bellatrix

- scaling
- aspect correction for display target
- overlay/UI/debug rendering
- display backend specifics
- vsync policy

### What is currently suboptimal

The current Bellatrix backend converts Rigel's `RGBA8888` output to `RGB565`
after the fact. That works for bring-up, but it is not ideal because it:

- duplicates memory traffic
- duplicates color packing work
- ties the host to a full-frame copy path
- makes a dedicated chipset core less attractive

### Recommended direction

For Bellatrix, the best medium-term shape is:

- Rigel exposes `RGBA8888` and `RGB565` as selectable output formats.
- Bellatrix chooses whichever fits the active host path.
- Bellatrix still owns presentation and scanout policy.

If we later want a dedicated chipset core, letting Rigel write directly into a
host-provided RGB565 backbuffer may be even better than returning RGBA frames.

## Multicore assessment

The user goal makes architectural sense:

- CPU on one core
- Rigel on another core

This is more natural than splitting Agnus/Paula/CIA into several Bellatrix cores
while the CPU still pokes legacy shared structs.

The key point is that the guest still performs normal Amiga MMIO and RAM
accesses. The cross-core boundary only exists between Bellatrix subsystems.

### Why this fits Rigel

Rigel already thinks in terms of:

- one owner of chipset state
- explicit temporal stepping
- explicit bus/IRQ observation

That maps well to a dedicated chipset-core model.

### What Bellatrix should avoid

Do not build multicore around direct shared access to chipset structs. That is
the legacy shape and it will keep leaking assumptions into the Rigel path.

Instead, Bellatrix should move toward a backend interface where the CPU side
sends internally decoded requests and the chipset side owns state progression.

### A better Bellatrix multicore shape

- Core CPU:
  - runs CPU
  - decodes global map seen by the guest
  - queues internally decoded chipset MMIO/bus requests
  - receives IRQ/IPL updates

- Core Rigel:
  - owns all chipset state
  - advances time with `rigel_step_until()` or equivalent scheduling policy
  - publishes frame-ready and serial/audio events

- Shared mechanism:
  - mailbox or lock-free queue
  - explicit response path for reads
  - double-buffer or fenced frame handoff

That is a better fit for Rigel than trying to spread Rigel itself across several
cores internally.

## Is `libamivideo` useful here?

Yes, but not as the core of Rigel.

`libamivideo` is fundamentally a format conversion and aspect-correction library.
It is useful for:

- planar/chunky/RGB conversions
- palette conversion
- EHB/HAM interpretation as an image conversion problem
- aspect-ratio correction for Amiga display modes

That is valuable, but it solves a different layer than Rigel.

## Where `libamivideo` fits better

### Better fit in Bellatrix

`libamivideo` is more naturally useful in Bellatrix than in Rigel.

Reasons:

- Bellatrix owns presentation
- Bellatrix may want offline conversions, debugging views, screenshots, asset tooling
- Bellatrix may want aspect-correct display helpers independent of the active chipset backend

This matches `libamivideo`'s role as an adapter/conversion library.

### Worse fit in Rigel

`libamivideo` is a worse fit as a dependency of Rigel's core because:

- Rigel is supposed to model chipset behavior, not host display policy
- `libamivideo` thinks in terms of source/target surface conversion
- Rigel's Denise path is temporal and hardware-semantic, not just "convert planar image"

In short:

- Rigel should generate correct video state and pixels.
- Bellatrix may optionally use `libamivideo` for host-side post-processing or tooling.

## Recommendation

### Short term

- Keep the current Bellatrix `legacy` and `rigel` switch.
- Keep the current Bellatrix-side `RGBA8888 -> RGB565` conversion temporarily.
- Treat `libamivideo` as optional host/tooling support, not as a Rigel dependency.

### Medium term

- Add an explicit Bellatrix `ChipsetBackend` abstraction.
- Make Rigel the owner of all chipset-state progression in its backend.
- Add selectable video output formats to Rigel.
- Move Bellatrix multicore toward `CPU core + chipset core`, not `many legacy chipset cores`.

### Long term

- Use Rigel as the dedicated chipset backend owned by one Bellatrix execution domain.
- Let Bellatrix own only:
  - global memory map
  - CPU
  - ROM/Fast RAM
  - presentation
  - platform I/O integration
