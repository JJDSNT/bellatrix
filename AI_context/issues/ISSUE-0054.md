---
id: ISSUE-0054
title: "Two WiFi drivers for the same chip: the AROS bwfm and Emu68's WiFiPi"
status: open
priority: medium
type: research
owner: unassigned
created_at: 2026-08-24
updated_at: 2026-08-24
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

It is vendored at `external/wifipi` and waiting. Porting it before there is a
number to justify it would be building for a path nobody has exercised --
the same mistake [ISSUE-0051](ISSUE-0051.md) records in detail.
