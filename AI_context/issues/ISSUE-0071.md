---
id: ISSUE-0071
title: "Rigel Paula to AHI: preserve the four channels as far down the pipeline as practical"
status: open
priority: medium
type: research
owner: unassigned
created_at: 2026-08-29
updated_at: 2026-08-29
tags:
  - audio
  - ahi
  - rigel
  - paula
  - integration
blockers:
  - ISSUE-0070
related_files:
  - src/amiga/bus.c
  - external/rigel/include/rigel/rigel_audio.h
  - aros/workbench/devs/AHI/Drivers/hdmiaudio/
---

# Purpose

How Paula in Rigel should feed audio into AHI. It deliberately **does not
require Rigel to know anything about AHI**:

> Rigel owns Paula semantics. Bellatrix/AROS owns the bridge to AHI.

Rigel must remain usable by other hosts and by its standalone harness.

**Prerequisite: ISSUE-0070.** The dependency is intentional. That issue makes
the Bellatrix AHI backend correct without Rigel in the picture; this one decides
how Paula connects to it. Keeping them apart stops decisions taken for Paula
from degrading the HDMI/PWM drivers, and means neither has to wait on a
premature change to Rigel.

# The direction, stated once

Paula is a **source**; AHI is the **output stack**; the bridge is an ordinary
AHI **client**.

```text
guest program -> audio.device / direct AUDx writes -> Rigel's Paula
   -> PCM (ARM side) -> bridge -> AHI channels -> AHI mixer -> AHIsub -> HDMI
```

Two things are the inverse of this and are not part of it:

- `workbench/devs/AHI/Drivers/Paula/` in AROS is an **AHIsub** -- it makes AHI's
  output go *to* Paula hardware. Nothing here wants that, and its buildability
  is not a question worth asking.
- Anything that lets AHI pull on Rigel. See "Clock authority" below: AHI
  consumes, it never drives.

The guest-side producer is the classic `audio.device` in
`arch/m68k-amiga/devs/audio/`, which writes the AUDx registers and therefore
reaches Rigel. It is the audio twin of `amigavideo`, from the same package.

# Paula

Four independent DMA channels -- AUD0..AUD3 -- each with DMA, sample data,
period, volume, interrupts and timing. The classic stereo arrangement combines
them into left and right. Rigel is authoritative for all of it.

# Four possible output boundaries

**A. Mono final output.** Loses stereo. Debugging or fallback only; not the
normal architecture.

**B. Stereo final output.**

```text
AUD0..AUD3 -> Rigel Paula mixer -> stereo PCM -> AHI
```

Simple, host-neutral, easy to validate, preserves the exact Rigel-generated
waveform, needs one AHI stream. The disadvantage is that AHI no longer knows the
audio came from four independent channels -- AUD0..AUD3 have disappeared before
reaching it.

**C. Four independent Paula streams.**

```text
Rigel AUD0 -+
Rigel AUD1 -+-> AHI channels -> AHI mixer -> stereo PCM -> AHIsub
Rigel AUD2 -+
Rigel AUD3 -+
```

Preserves substantially more information, and is the preferred direction of the
three above.

**D. Parameter hand-off -- no PCM crosses the boundary at all.**

```text
Rigel AUDx state (pointer, length, period, volume, DMA)
   -> AHI_SetSound / AHI_SetFreq / AHI_SetVol on channel x
   -> AHI mixer reads the guest's own sample data from memory
   -> AHIsub
```

This one is not a refinement of C -- it is a different boundary, and it is what
a shipping implementation of exactly this problem does. See "Prior art:
NallePuh" below. Rigel never renders an audible sample; AHI's mixer plays the
guest's 8-bit data directly, at the host's rate, at the pitch the period
register asks for.

Its consequences are large enough that it should be evaluated before C:

- **It decouples audio from chipset speed.** AHI plays at the host rate with
  the correct pitch no matter how slowly Rigel is being stepped. This is the
  one part of the machine that the ~3.8x gap does *not* gate.
- **It is far cheaper**: no ring buffer, no ARM-to-m68k sample stream, no
  resampling of our own, and no per-sample synthesis in Rigel on the audible
  path.
- **It does not violate the rule above.** Rigel stays authoritative for what
  the guest observes -- audio interrupts, DMA pointer progress, register
  semantics. Only the audible rendering is delegated, which is precisely what
  "AHI may be used to render the resulting audio efficiently" allows.

What it costs is cycle accuracy of the *sound itself*: a period change part way
through a sample, a volume ramp stepped per scanline, a one-shot to loop
transition timed exactly -- none of those survive being handed to a host mixer
as a set of parameters. NallePuh's own README is explicit that it targets
"system friendly software" and cannot guarantee programs that drive the
hardware directly. That is the right trade for the first target here, since
Demo Reel 3's player is system-friendly, and it is the wrong trade for a
hardware-banging demo.

## D and C are renderers, not architectures

NallePuh cannot serve a program that drives the hardware directly, and that
limitation belongs to *its* approach rather than to boundary D. NallePuh
intercepts register writes with no chipset underneath, so anything the
parameters cannot express is simply lost. Rigel is the chipset: those writes are
modelled whether or not anyone listens.

That splits the problem in two, and only one half is a choice:

- **Rigel is always the authority.** Every AUDx write reaches it, always. DMA
  progress, audio interrupts, pointer and length semantics, cycle-exact
  behaviour -- all of it works, for hardware-banging programs as much as for
  system-friendly ones. Nothing below changes that.
- **The renderer is a policy.** D hands the parameters to AHI's mixer; B and C
  play what Rigel rendered. Both leave the guest-observable state identical,
  because it comes from Rigel either way.

So the D-versus-C decision is not a fork in the architecture. It is which
renderer a voice uses, and it can differ per voice and change at run time. A
program that rewrites `AUDxPER` or `AUDxVOL` many times per frame, or switches
one-shot to loop at a sample-exact moment, is doing something the parameters
cannot express -- that voice falls back to the rendered path while the others
stay cheap. The fallback condition is observable from Rigel's own state, so the
policy can be automatic rather than configured.

This is also why boundary D does not weaken the standing rule. "Rigel owns
Paula semantics" holds in both, and what moves is only how the sound is made
audible.

## Prior art: NallePuh

`https://github.com/khval/NallePuh` -- Paula and CIA emulation for AmigaOS 4.1
that intercepts register accesses and redirects audio to AHI. It answers
several of the questions below with working code:

```c
/* Four channels, low-level API, its own AudioCtrl */
pd->m_AudioCtrl = AHI_AllocAudio( AHIA_AudioID,  audio_mode,
                                  AHIA_MixFreq,   frequency,
                                  AHIA_Channels,  4,
                                  AHIA_Sounds,    1,
                                  AHIA_SoundFunc, (ULONG) &pd->m_SoundFunc,
                                  TAG_DONE );
for (id = 0; id < 4; id++) pd->channels[id].id = id;   /* AUD0..3 -> 0..3 */

/* One dynamic sound spanning the whole address space, so SetSound can point
 * at whatever AUDxLCH/LCL holds. AHIST_M8S is exactly Paula's format. */
struct AHISampleInfo si = { AHIST_M8S, 0, 0xffffffff };
AHI_LoadSound( 0, AHIST_DYNAMICSAMPLE, &si, pd->m_AudioCtrl );

AHI_SetFreq( channel, pd->m_ChipFreq / value, pd->m_AudioCtrl, AHISF_IMM );
AHI_SetVol ( channel, value << 10, 0x10000,   pd->m_AudioCtrl, AHISF_IMM );
```

Two of its decisions differ from the direction sketched below, and both are
worth arguing with rather than copying:

- **It creates its own `AHIAudioCtrl`** rather than reserving channels inside
  the one an `ahi.device` unit owns. That is the route flagged as risky under
  "The low-level API is the relevant one" -- it works on AmigaOS 4.1 because
  its AHIsub drivers cope with several AudioCtrls. Whether `hdmiaudio` copes is
  therefore **the pivotal question of this issue**, not a side task; it is the
  "Multiple AHIAudioCtrl instances" item in ISSUE-0070.
- **It hands AHI a pointer into the guest's own memory.** NallePuh can, because
  it runs in the same address space. Here the AUDx writes land in Rigel on the
  ARM side while AHI runs on the m68k side -- but chip RAM is mapped direct by
  construction, precisely so it does not fault, so the m68k pointer the guest
  wrote is a pointer AHI can also read. The bridge therefore carries channel
  *state*, and no sample data.

# Why preserve four channels

Independent channel activity, independent volume, independent stereo position,
per-channel observability, per-channel diagnostics, visualisation, and a more
faithful representation of the Paula architecture. The difference is whether
channel identity survives to AHI or is lost inside Rigel.

**This is what today's Rigel API cannot do.** `rigel_get_audio_sample()` returns
one `{left, right}` pair with panning already applied -- enough for boundary B,
not for C. The addition is written up as Rigel's `AI_context/issues/ISSUE-0007.md`.

# Rigel must still own Paula

The wrong architecture is `Rigel DMA -> AHI implements Paula timing/volume/period`.
The right one keeps Paula semantics inside Rigel and hands AHI four PCM streams:

```text
Paula registers + DMA -> Rigel -> Paula semantics -> AUD0..AUD3 -> PCM -> AHI
```

AHI is the final mixer and output system, not the Paula emulator.

# Why the ordinary CMD_WRITE path is not enough

`ahi.device`'s high-level path supports multiple simultaneous playback requests,
but a unit's channels are a **dynamic pool**: `CMD_WRITE -> find available
Voice[] -> PlayRequest() -> AHI channel`. A request is not permanently tied to a
particular channel, and AHI also arbitrates by priority. So four ordinary
persistent writes do not guarantee AUD0=channel 0 .. AUD3=channel 3, and stable
Paula channel identity is exactly what boundary C needs.

# The low-level API is the relevant one

```text
AHI_AllocAudio() -> AHIAudioCtrl -> channel 0..3
AHI_SetSound() / AHI_SetVol() / AHI_SetFreq() / AHI_Play()
```

That maps naturally onto channel 0 = AUD0 .. channel 3 = AUD3.

**The limitation:** a separate `AHI_AllocAudio()` normally creates a separate
`AHIAudioCtrl`, while an `ahi.device` unit already owns one. Two of them playing
simultaneously through the same AHIsub depends entirely on the AHIsub
implementation, and `hdmiaudio` must **not** be assumed to combine two
independent `AHIAudioCtrl` instances into one HDMI MAI output safely. ISSUE-0070
has the task that tests this.

# Do not solve this inside hdmiaudio prematurely

`hdmiaudio` is ours, so it could be changed to mix two `AHIAudioCtrl` instances
itself -- but that moves mixing and multiplexing into the AHIsub, duplicating
what AHI already does, and makes the HDMI driver responsible for multiple
clients, differing sample rates, synchronisation, independent buffers, client
lifetime, mixing, hardware ownership and one final DMA stream. Not the first
solution to reach for, and certainly not solely for Paula.

# Preferred architecture to investigate

Make Paula use four channels of the **same** `AHIAudioCtrl` an `ahi.device` unit
already owns:

```text
                ahi.device Unit N
                        |
                  AHIAudioCtrl
                        |
        +---------------+---------------+
        |               |               |
   AUD0..AUD3      normal AHI      other clients
        |            sounds             |
        +---------------+---------------+
                        |
                    AHI mixer -> stereo PCM -> AHIsub -> HDMI / PWM / ...
```

This keeps the ownership boundary intact: **AHI does channels, multiplexing and
software mixing; the AHIsub does the physical output.**

## Reserved channels

Investigate whether AROS's `ahi.device` can provide four persistent channels
inside its existing `AHIAudioCtrl`. Do not assume channels 0-3 permanently; a
reservation mechanism is cleaner -- reserve four, get handles back, and have
normal `CMD_WRITE` allocation exclude them.

`ahi.device` already uses the low-level operations on its own `AHIAudioCtrl`
internally, so the bridge should reuse the same primitives rather than introduce
another audio path. That may need only a localised AROS AHI extension.

## How many channels

Four reserved out of 16, or out of 32 -- the total from performance testing,
configurable where practical. Paula must not consume the unit's whole capacity.

# What Rigel stays authoritative for

- **Stereo position.** The bridge may express the fixed Paula routing through
  AHI channel positioning, but the mapping must be verified against Paula's
  actual behaviour. Do not infer or redefine it from AHI conventions.
- **Volume.** If AHI channel volume represents an AUDx final gain it must be a
  faithful projection of Rigel state. AHI must not become authoritative for the
  emulated register.
- **Period and frequency.** AHI may render the audio efficiently, but its
  playback model must not change the observable timing of the emulated
  hardware -- Paula timing participates in chipset behaviour independently of
  the host audio device.

# Clock authority

```text
Rigel chipset time -> Paula progresses -> audio generated -> AHI consumes
```

never

```text
AHI requests audio -> Rigel advances Paula
```

Rigel stays synchronised with the emulated chipset timeline; AHI is a consumer,
and buffering absorbs the difference. This matters more here than it looks:
Bellatrix already drives Rigel's clock from modelled CPU progress (ISSUE-0068),
so a second clock authority would be a second source of truth for the same
timeline.

# Buffering

```text
Rigel -> AUD0 FIFO / AUD1 FIFO / AUD2 FIFO / AUD3 FIFO -> AHI channels
```

Investigate buffer size, latency, underrun and overflow behaviour,
synchronisation between AUD0..AUD3, the AHI callback/interrupt context and the
Rigel execution context. **An AHI callback must never directly alter Rigel
timing.**

# Harness independence

Rigel must still work without AHI, so its public audio boundary stays
host-neutral -- something like `rigel_audio_render(...)`, or callbacks/events
for independent channel output. The exact API is Rigel's to design (its
ISSUE-0007), not AHI's to dictate, and the standalone harness must be able to
consume the same information.

# Visibility to AHI applications

Preserving four channels is useful, but do not assume arbitrary applications can
inspect them: channel information belongs to a particular `AHIAudioCtrl`.
Whether AUD0..AUD3 can be made visible to diagnostic or mixer tooling is a
separate question, and must not be claimed until the API path is confirmed.
Observability can be added later without changing the architecture.

# Backend independence

The bridge must not know the physical output is HDMI:

```text
Rigel -> AUD0..3 -> Bellatrix/AROS AHI bridge -> AHI Unit N -> HDMI / PWM / I2S
```

Changing the unit's configuration should be enough to redirect Paula audio.

# Initial investigation tasks

```text
[ ] Confirm how ahi.device owns its AHIAudioCtrl
[ ] Confirm the exact Voice[] allocation path
[ ] Confirm CMD_WRITE channel arbitration
[ ] Identify all internal uses of AHI_SetSound/SetVol/SetFreq/Play
[ ] Determine whether ahi.device has any channel reservation concept already
[ ] Determine whether an internal client can access the Unit AHIAudioCtrl safely
[ ] Determine the minimum extension required for persistent channels
[ ] Determine a suitable number of voices per unit
[ ] Verify AHI channel-info visibility
[ ] Verify the Paula stereo mapping against Paula, not against AHI
[ ] Define Rigel's host-neutral audio interface (Rigel ISSUE-0007)
[ ] Define buffering between Rigel and AHI
[ ] Test simultaneous Paula and normal AHI playback
```

# First integration test

```text
AHI Unit N: AUD0 tone, AUD1 tone, AUD2 tone, AUD3 tone
            + a normal AHI application
            -> AHI mixer -> hdmiaudio -> HDMI
```

Verify that all four channels stay independent, normal AHI audio keeps playing,
no channel stealing occurs, left/right placement is correct, a volume change
affects only the intended AUDx, stopping one AUDx does not affect the others,
AHI playback does not affect Rigel timing, and there are no underruns or drift.

# Recommended direction

> Preserve AUD0..AUD3 as four independent sources until the AHI mixer.

That still holds, and boundary D satisfies it -- four channels, four AHI
channels, identity preserved. What D changes is *what travels*: channel state
rather than rendered PCM. Evaluate D first; fall back to C only where parameter
hand-off cannot reproduce what a program does to the hardware.

Prefer four channels on one shared `AHIAudioCtrl` over pre-mixed stereo into
AHI, and prefer it over mixing two `AHIAudioCtrl` instances inside `hdmiaudio` --
unless investigation finds a broader reason for Bellatrix AHIsub drivers to
support multiple independently mixed instances.

> Preserve Paula channel information as long as possible, while keeping mixing
> in AHI and physical-device handling in the AHIsub driver.

# Correction: audio is not gated on the performance work

This issue first said that audio does not degrade gracefully, that Rigel is
~3.8x short of realtime on a Pi 3 (Rigel's ISSUE-0006), and that none of this
produces usable sound until that closes. **That is only true of boundaries A,
B and C**, where Rigel renders the samples and the host consumes a stream at
whatever rate the chipset manages to produce it.

Under boundary D it is false. AHI plays the guest's sample data at the host's
own rate, and the period register only sets the pitch, so the sound is correct
while the chipset around it runs slowly. Audio is then one of the few parts of
this machine that the speed work does not block -- which also makes it a
better early target than the "sound last" ordering in ISSUE-0068 assumed.
