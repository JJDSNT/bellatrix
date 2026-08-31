---
id: ISSUE-0060
title: "Bluetooth audio: the stack has no path from an AHI stream to a headset"
status: backlog
priority: low
type: feature
owner: unassigned
created_at: 2026-08-26
updated_at: 2026-08-26
tags:
  - bluetooth
  - audio
  - a2dp
  - ahi
  - backlog
blockers:
  - "the feature freeze of 2026-08-17 — this is new functionality, not a repair"
  - "unmeasured: whether an SBC encoder can keep up on a JIT'd m68k"
related_files:
  - external/aros/rom/bluetooth/stack/core.files
  - external/aros/rom/bluetooth/stack/profiles
  - AI_context/issues/ISSUE-0046.md
  - AI_context/issues/ISSUE-0055.md
---

# Summary

Connecting a Bluetooth headset is not possible today, and not because of a
missing piece — because the audio side of the stack does not exist. This issue
records what would have to be built and, more usefully, the question that has
to be answered *before* any of it is worth building.

**This is new functionality.** Under the standing decision of 2026-08-17 it
stays `backlog`. The issue is the record of what was considered, not a queue
item.

# What is there

From `rom/bluetooth/stack/core.files`, the stack as it stands:

| layer | present |
|---|---|
| transport | H4 |
| protocols | HCI, L2CAP (+ signaling, channel manager), SDP + client, ATT, GATT client, SMP |
| security | bond store, SMP crypto / pairing / manager |
| profiles | HOGP client |
| input | HID report parser, HID input, AROS input bridge |

That is a complete, working *input* stack. Everything an audio device needs
sits beside it, not on top of it.

# What a headset would need

**To play audio to it (A2DP Source — we are the source, the headset is the
sink):**

- **AVDTP** — the Audio/Video Distribution Transport Protocol over L2CAP:
  discovery, capability negotiation, stream configuration, start/stop, and the
  media transport channel. Nothing of it exists.
- **A2DP Source** — the profile on top of AVDTP, including the SDP record.
- **SBC encoder** — mandatory in A2DP. Nothing of it exists.
- **AVRCP** — play/pause/volume from the headset's buttons. Optional, and the
  first thing users ask for once the audio works.

**To use its microphone (HFP / HSP):**

- the profile itself, plus
- **SCO/eSCO routing through the H4 transport** — synchronous packets are a
  separate packet type on the same UART, with their own flow discipline, and
  the transport does not carry them today;
- **CVSD** or **mSBC** for the voice channel.

**On the AROS side:**

- a path from an AHI stream into the A2DP source, and the reverse for the
  microphone. This is a whole design of its own — the existing AHI drivers here
  (`hdmiaudio`, `i2saudio`, `pwmaudio`) all drive local hardware with DMA, and
  a Bluetooth sink is nothing like that.

So it is not one missing piece. It is a second stack the size of the first,
sitting on the L2CAP that already exists.

# The question that comes first

**Can an SBC encoder keep up on this machine?**

A2DP Source has to produce a steady stream: 44.1 kHz stereo, encoded in real
time, forever, on an m68k that is being JIT-translated to AArch64 — on a
machine whose desktop speed is the reason the freeze exists. If the encoder
cannot hold real time, none of the protocol work above matters.

This is measurable long before it is buildable: take a reference SBC encoder,
compile it for the target, and time it against a wall clock on the hardware.
That number decides whether this issue is a project or a note.

The same trap has already been walked here once — see the audio realtime work
under [ISSUE-0055](ISSUE-0055.md), where the blocker turned out to be speed
against real time rather than any protocol or driver defect.

# Notes

- A2DP **Sink** (the Pi receiving audio from a phone) is a different and in
  some ways easier target: the decoder is cheaper than the encoder, and there
  is no microphone path. It is also not what "connect my headset" means.
- The Pi 3's BCM43438 supports BR/EDR, so the bearer is not the obstacle —
  A2DP is classic Bluetooth, not LE.
- Whatever is built here belongs upstream in `rom/bluetooth/stack`, the same
  way the rest of the stack does, not in a fork under this port.
