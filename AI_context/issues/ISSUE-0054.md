---
id: ISSUE-0054
title: "Two WiFi drivers for the same chip: the AROS bwfm and Emu68's WiFiPi"
status: open
priority: medium
type: research
owner: unassigned
created_at: 2026-08-24
updated_at: 2026-08-29
tags:
  - wifi
  - bwfm
  - sdio
  - driver-comparison
  - m68k-emu68
blockers:
  - needs real hardware to measure
related_files:
  - aros/arch/m68k-emu68/soc/wifi/sdio
  - aros/arch/m68k-emu68/soc/wifi/bwfm
  - external/wifipi
  - AI_context/issues/ISSUE-0047.md
---

# Summary

This port now has a WiFi driver -- the AROS `bwfm`, adopted from
`arch/arm-native` and building as three modules. A second driver for the same
chip exists and is vendored as `external/wifipi`: michalsc's `WiFiPi.device`,
written for Emu68. This issue records what separates them, so the choice
between them is made on evidence rather than on which was found first.

**Neither can be measured here.** QEMU's raspi3b emulates no BCM43438, so
every number below is what the code asks for, not what the hardware gives.

# Decision update, 2026-08-28: keep two selectable drivers

`WiFiPi.device` is a viable second driver for this port. Its current build
system and dependencies make it a porting task, not a file-copy task, but
there is no architectural reason to exclude it. The release may carry both:

```text
DEVS:Networks/bwfm.device
DEVS:Networks/WiFiPi.device
```

Only one may own the BCM43438 and Arasan SDIO controller in a given boot. That
is a selection constraint, not a reason to omit either driver. It is the same
model used for the two DWC2 implementations: both remain available for A/B
testing and configuration selects one.

The intended interface is a build/default selector such as:

```text
BELLATRIX_WIFI_DRIVER=bwfm
BELLATRIX_WIFI_DRIVER=wifipi
```

The selected value must consistently control all of the following:

- which driver is named in `ENVARC:AROSTCP/WirelessDevice` and
  `ENVARC:AROSTCP/db/interfaces`;
- whether the AROS `sdio.resource` and `bwfm.resource` residents are linked
  and allowed to probe the controller;
- which device is opened by WirelessManager;
- which firmware layout is installed, if the implementations require
  different names or locations.

Do not implement selection by merely changing the AROSTCP device string while
leaving both low-level implementations active. `bwfm.device` itself is lazy,
but the ownership boundary must be verified at the resource/resident level so
that the unselected stack cannot power, reset or configure the same SDIO bus.

This is now diagnostically useful, not only a feature/performance comparison.
The first-boot WiFi work in ISSUE-0065 loads `bwfm.device` on hardware but has
not produced an AppIcon or a successful SDIO/firmware milestone, while a
concurrent USB recovery storm makes the apparent hang ambiguous. Running
WiFiPi on the same kernel, card and hardware gives an independent path through
GPIO, clocks, SDIO enumeration, firmware and association:

- WiFiPi works and AROS bwfm does not: focus on the adopted AROS SDIO/bwfm
  port;
- neither works with USB held constant: focus on shared platform setup,
  firmware/card contents or hardware;
- both work when `dwc2emu68` is replaced/disabled: the WiFi driver was not the
  owner of the hang;
- both hang only with `dwc2emu68`: investigate the cross-subsystem timing or
  starvation recorded in ISSUE-0064/0065.

The immediate porting goal is therefore modest: build a SANA-II-compatible
`WiFiPi.device`, install it beside `bwfm.device`, add exclusive selection, and
reach scan on hardware. Feature parity, WPA3 and throughput tuning come after
that controlled comparison.

# Why WiFi is possible at all now

On a Pi 3 the BCM43438 sits on the Arasan SDHCI controller. The SD card can
sit there too, and they cannot share it -- a card on Arasan means no WiFi on
this board, full stop. `soc/sdcard` drives the card from SDHOST instead
(`SDCARD_BACKEND := sdhost`), which is what leaves Arasan free. That earlier
decision is the precondition for all of this.

# They are not independent drivers

Both are ports of the same OpenBSD `bwfm`. They share `bwfmreg.h` verbatim;
WiFiPi additionally carries `bwfmvar.h` and `if_bwfm_sdio.h` from the
original, which AROS rewrote. So this is not a choice between two designs, it
is a choice between two ports of one design.

# Structure

| | AROS `bwfm` | Emu68 `WiFiPi` |
|---|---|---|
| modules | 3: `sdio.resource`, `bwfm.resource`, `bwfm.device` | 1 monolithic device |
| source | 183 KB, 7 files | 323 KB, 17 files |
| hardware discovery | `KrnGetSystemAttr(KATTR_PeripheralBase)` | its own `devicetree.resource` |
| SDIO transport | separate, reusable by any SDIO peripheral | `sdio.c` inside the driver |
| SANA-II layer | AROS's generic `workbench/devs/networks/bwfm` | its own `unit.c` + `device.c` |
| build | `%build_module`, integrated | CMake, baremetal `-nostdlib` |
| firmware | expects `DEVS:Firmware/`, ships none | ships 14 blobs |

# Function

| | AROS | WiFiPi |
|---|---|---|
| WPA2-PSK | yes | yes |
| **WPA3 / SAE** | **absent** | `WPA3_AUTH_SAE_PSK`, `RSN_AKM_SAE` |
| **promiscuous mode** | **absent** | implemented |
| multicast | 7 references | 52 |
| scan, RSSI, roaming | yes | yes |
| power save | neither | neither |
| regulatory domain | neither | neither |

WPA3 is the one that bites: on a network configured WPA3-only, the AROS
driver does not associate at all. Without promiscuous mode it cannot be used
for capture or bridging.

# Performance, as asked for rather than as delivered

| | AROS | WiFiPi |
|---|---|---|
| SDIO clock target | **25 MHz** (`SDIO_FULL_CLOCK`) | **41.6 MHz** (`SD_CLOCK_NORMAL`), 52 MHz high |
| bus width | 4-bit | 4-bit |
| CMD53 multi-block | yes | yes |

On WiFi the SDIO bus is the bottleneck rather than the radio, so 25 against
41.6 MHz is a 1.66x difference in the transport ceiling, and `SD_CLOCK_HIGH`
would make it 2x.

Two cautions. The clock is a divider request over a base clock and the
hardware need not grant it. And 25 MHz may be deliberate conservatism rather
than a limit -- it is one constant, and raising it is the cheapest experiment
available once there is hardware to measure on.

# What would decide it

In order, and none of it doable on this machine:

1. does the AROS driver associate on a Pi 3 at all?
2. if yes, what throughput -- and does raising `SDIO_FULL_CLOCK` to 41.6 MHz
   change it? That single constant recovers most of the paper difference
   without porting anything.
3. if it will not associate because the network is WPA3, WPA3 stops being a
   table entry and becomes the argument.

# The cost of porting WiFiPi

Not the copy-and-adapt the USB, audio and VideoCore adoptions were. It is a
CMake baremetal executable linked against `devicetree.resource` (a nested
submodule) and an `amiga` linklib this port does not have, so adopting it
means replacing its build base rather than adapting its registers.

It is vendored at `external/wifipi` and waiting. The 2026-08-28 hardware
ambiguity now supplies the reason that was previously missing: this second
implementation is a control experiment for the AROS driver and the concurrent
USB hang. Porting it is approved as an investigation path, provided selection
is exclusive and the comparison changes one subsystem at a time.

# Three-way comparison, 2026-08-29

With `bwfm` finally reaching `firmware ready` on hardware, the two references
were read against it line by line: `arch/arm-native/soc/broadcom/2708/{sdio,bwfm}`
(what this port was adopted from) and `external/wifipi/src/{sdio,wifipi}.c`.

`bwfm` here is a strict superset of the ARM-native original -- every difference
is something this port added -- so the comparison that matters is against
WiFiPi, which runs on the same board, the same controller and the same
big-endian m68k.

## What WiFiPi was doing better, and is now adopted

| | WiFiPi | this port, before | now |
|---|---|---|---|
| PIO data port | raw 32-bit load/store | `AROS_LE2LONG()` -- reversed every 4-byte group | raw (ISSUE-0065) |
| backplane register | explicit `LE32()` | implicit, cancelled the above | explicit `AROS_LE2LONG()` |
| bus clock target | 41.6 MHz -> divisor 4 -> 31.25 MHz | 25 MHz -> divisor 8 -> **15.6 MHz** | 41.6 MHz -> 31.25 MHz |
| PIO inner loop | unrolled word moves | `CopyMem()` per 4 bytes | unrolled word moves |
| inter-write gap | none | flat 6 us, every register write | 2 SD clocks, clock-scaled |

The clock is the one worth spelling out. The divisor is a power of two, so the
constant is a ceiling, not a rate: 25 MHz selected N=8 and the bus ran at half
the speed anybody thought it did. WiFiPi's 41.6 MHz rounds to the same N=4 this
now selects. High speed stays off in both drivers -- the Arasan HISPD bit is a
known-broken quirk on BCM2835 and neither driver sets it.

The 6 us gap is the BCM2835 erratum requiring two SD-clock cycles between
successive controller writes. Two SD clocks is 5 us at the 400 kHz
identification clock and 64 ns at 31 MHz; charging the identification-clock
figure to every write for the life of the bus cost several tens of
microseconds per command on a driver whose datapath is nothing but commands.
`sdio_setclock()` now recomputes it with the rate. WiFiPi leaves no gap at all;
this keeps the erratum honoured at every clock instead.

The firmware upload also went back to block-mode CMD53 -- one call per 32 KB
backplane window instead of 6250 64-byte byte-mode commands. The 64-byte
chunking was copied from WiFiPi while hunting the corruption that turned out to
be the byte-order defect; it was never related to it, and WiFiPi pays for it
too. A whole-image verification (`bwfm_verify_upload()`) now samples eight words
across the blob, so a short or misaddressed upload is reported as such instead
of arriving as a silent `FWREADY` timeout.

## What was compared and deliberately not taken

- **Frame glomming.** WiFiPi enables `bus:txglom`, `bus:txglomalign=4` and
  `bus:rxglom` at interface-up (`unit.c:1481`), having held `txglom` off during
  bring-up. This port keeps all glomming off, because with `rxglom=1` the
  firmware also aggregates BCDC control *responses* onto the GLOM channel and
  `bwfm_rx_glom()` keeps only EVENT/DATA sub-frames, so every polled `dcmd`
  right after firmware-ready timed out. This is the largest remaining
  throughput item and it is a correctness fix first: route GLOM control
  sub-frames back into the `dcmd` reply path, then enable glomming at
  interface-up rather than at bus start, as WiFiPi does. Not attempted now --
  it cannot be tested without an interactive machine, and its previous failure
  mode was "no control commands at all", which is indistinguishable from a dead
  driver in a serial log.
- **`brcmf_sdio_sr_init()`.** brcmfmac runs it for BCM43430 (WAKEUPCTRL HTWAIT,
  CCCR CMD14 support, then `FORCE_HT`); neither WiFiPi nor this port does. It is
  a power-save path, not a bring-up one, and the firmware starts without it.
- **Waiting loops.** WiFiPi's `TIMEOUT_WAIT` polls at 10 us, exactly what
  `sdio_command()` already does. No difference to take.
- **Block sizes and F2 watermark.** F1 = 64, F2 = 512, watermark = 8 in both.
- **RX delivery.** This port's pump already does adaptive-backoff polling with
  an optional card interrupt and a spin guard that falls back to polling if the
  interrupt does not self-clear. WiFiPi polls unconditionally. Nothing to take.

## What is still unmeasured

Every number above is a command count or a clock divisor, not a throughput
figure. QEMU emulates no BCM43438, so the first real measurement has to come
from hardware: `[WIFI:BWFM] firmware ... uploaded and verified in N ms` is now
printed on every start and is the cheapest available proxy for bus throughput.

# What glomming is, and what it would take here

`bus:rxglom` / `bus:txglom` are SDPCM frame aggregation. Without them each
frame costs its own round on the bus -- header read, body read, per-command
overhead. With them the firmware concatenates several frames into one
superframe on channel 3 (GLOM): a descriptor carrying the sub-frame lengths,
then the sub-frames back to back. On a datapath that is nothing but commands,
this is the difference between paying overhead per frame and per batch, and it
is the largest throughput item left.

One line is the whole obstacle, `bwfm_init.c:1444`:

```c
if (ch != BWFM_SDIO_SWHDR_CHANNEL_EVENT && ch != BWFM_SDIO_SWHDR_CHANNEL_DATA)
    continue;
```

`bwfm_rx_glom()` reads a control sub-frame off F2 -- so the FIFO does not
desync -- and then discards it. With `rxglom=1` the firmware also aggregates
BCDC control *responses* into the superframe, so `bwfm_dcmd()`, which polls
synchronously for its own reply, never sees it. That is why the previous
attempt left every dcmd after firmware-ready timing out: GetMAC, clmload, C_UP
and join alike. The failure mode is indistinguishable from a dead driver, which
is why it is not worth attempting without an interactive machine to test on.

The work, in order:

1. In `bwfm_rx_glom()`, on `CHANNEL_CTRL`, park the sub-frame in a pending
   control-reply slot. One slot suffices: the control channel is synchronous.
2. In `bwfm_dcmd()`, check that slot in the poll loop before reading F2.
3. Only then set `bus:rxglom 1`, and do it at interface-up rather than at bus
   start -- WiFiPi holds `txglom` off through bring-up and enables both in
   `unit.c:1481`.

# Transfer sizes: where this port is smaller than both references

| | this port | WiFiPi | brcmfmac |
|---|---|---|---|
| RX frame buffer | 2048 B (static) | 65536 B (pooled) | skb |
| glom superframe | 32 KB, 16 sub-frames | 64 KB, ~32 sub-frames | dynamic |
| control (dcmd) payload | **484 B** (`frame[512]`, on the stack) | 1400 B per CLM chunk | `BRCMF_DCMD_MAXLEN` 8192 |
| iovar name + data | 300 B (`tmp[300]`) | pooled | 8192 |

The 484-byte ceiling on the control channel is the one with consequences:

- the `cap` iovar does not fit. Its capability list runs to several hundred
  bytes, so the firmware cannot currently be asked what it supports -- which is
  exactly the question WPA3 turns on (below);
- `clmload` goes in ~470-byte chunks where brcmfmac and WiFiPi use 1400: more
  than three times the control round-trips for the same blob;
- escan results are bounded by the same buffer.

The fix is not a bigger array. `frame[512]` is a *stack* buffer in a library
function called from arbitrary tasks, and AROS task stacks are small; it wants
to be allocated once at init and used under `bwfm_Sem`, which the callers
already hold.

# WPA3: the decision procedure, not the code

WiFiPi's WPA3 does not work, and reading it as a reference is misleading.
`WPA3_AUTH_SAE_PSK` appears in three places -- a `#define`, an
`wpa_auth |=` while parsing the AP's RSN IE, and `packet.c:325` -- and beside
it sits:

```c
mfp = BRCMF_MFP_NONE;
/* TODO: handle MFP... */
```

SAE mandates PMF; with `mfp = 0` no real WPA3 AP will associate. There is no
SAE exchange anywhere in that tree: no `sae_password` iovar, no host-side
dragonfly. Adopting it would reproduce a stub.

The correct first step is to ask the firmware, the way brcmfmac does: read the
`cap` iovar and look for a token in its space-separated list.

- **`sae` present** -- the firmware runs SAE itself. Set
  `wpa_auth = WPA3_AUTH_SAE_PSK`, `mfp = MFP_REQUIRED`, `wsec = AES`, and pass
  the passphrase through `sae_password` instead of `wsec_pmk`. Around sixty
  lines and no cryptography of our own.
- **`sae_ext` present** -- external authentication: the firmware raises
  `E_EXT_AUTH_REQ`, the host runs SAE and returns the result. That is real work:
  dragonfly commit/confirm on P-256, HMAC-SHA256, the KDF, then the existing
  host-side four-way handshake with the SAE PMK.
- **neither** -- WPA3 is not reachable on that firmware, and the answer is "no"
  rather than "hard".

Expectation, to be confirmed rather than assumed: BCM43430 with 7.45.98 (2018)
has neither. SAE reached Cypress firmware for the 4373, 43012 and 43439, not
this part. The `cap` read settles it, and it is cheap -- but it needs the dcmd
buffer above first, which is why the two items are ordered together.

# Suggested order

1. dcmd buffer allocated at init, and `cap` reported in the log. Diagnosis,
   cheap, and it unblocks the WPA3 question.
2. Control sub-frames through the glom path, then `rxglom` at interface-up.
   The large throughput item.
3. WPA3, if and only if `cap` says it is reachable.

None of this is in the current image. The four transport and speed changes
already made are unvalidated on hardware, and stacking more onto the same pack
would make a regression unattributable.
