---
id: ISSUE-0086
title: "The three SD backends are named on three different axes"
status: open
priority: low
type: cleanup
owner: unassigned
created_at: 2026-09-01
updated_at: 2026-09-01
tags:
  - sdcard
  - naming
blockers: []
related_files:
  - aros/arch/m68k-emu68/soc/sdcard/mmakefile.src
  - aros/arch/m68k-emu68/soc/sdcard/README-emu68sd.md
  - aros/arch/m68k-emu68/soc/sdcard/sdcard_sdhost_bus.c
  - aros/arch/m68k-emu68/soc/sdcard/sdcard_emu68sd_bus.c
---

# What is wrong

`SDCARD_BACKEND` selects one of three drivers for the card slot, and the three
are named after three different things:

| value | named after | what it actually is |
|---|---|---|
| `arasan` | the IP block | the Arasan SDHCI at peripheral offset `0x300000`, PIO |
| `sdhost` | the IP block | the Broadcom SDHOST at `0x202000`, DMA through `dma.resource` |
| `emu68sd` | **whose code it is** | the *same* SDHOST block, PIO, from Emu68's `brcm-sdhc.device` |

Two of them drive the same controller and neither name says which is which.
The axis that actually separates them -- DMA against PIO -- appears in no
name, only in `README-emu68sd.md` and in a comment at
`mmakefile.src:99-107`.

The two files say so themselves. `sdcard_sdhost_bus.c` and
`sdcard_emu68sd_bus.c` open with the **identical** header:

```c
/*-
 * BCM2835 SDHOST controller bus implementation.
 * Derived from NetBSD's sys/arch/arm/broadcom/bcm2835_sdhost.c.
```

They have since diverged by roughly 780 diff lines -- the SDHOST copy grew
`sdhost_bus_addr()` and the cache maintenance, the Emu68 copy has none of it
-- so the shared provenance line is now the least interesting thing about
either.

The first row has a third label on top of that: the documented value is
`arasan`, the files are `sdcard_bcm2708init`/`sdcard_bcm2708bus` (the SoC
family), and the block is the Arasan SDHCI. It is also not selected by name
at all -- it is the `else` branch, so any typo in `SDCARD_BACKEND` silently
builds it.

# Why it is worth fixing

`make-sdcard.sh` and every boot log name a driver, and the name is what a
reader has to map back to a data path when a card fails to come up. Getting
`emu68sd` from a log tells you who wrote it, not whether DMA was involved,
which is the first question ISSUE-0013 and ISSUE-0065 both start from.

The reference for the shape is Emu68 itself: it ships these same two blocks as
`brcm-emmc.device` and `brcm-sdhc.device` -- one axis, both distinguishable.

# Suggested shape

Name every backend after the block, and put the transfer method in the name
where two backends share a block:

    arasan     -> arasan       (unchanged in meaning; rename the files off
                                the bcm2708 prefix, and make it a named value
                                rather than the else branch)
    sdhost     -> sdhost-dma
    emu68sd    -> sdhost-pio

This is naming only -- no behaviour changes, and the `%build_archspecific`
file lists move with the names. The `else` branch becoming an explicit value
plus an error on an unknown one is the only functional part, and it is what
stops a typo from quietly changing which controller owns the slot.

Raised alongside the Bluetooth rename, which fixed the same class of problem
in `soc/bluetooth/`.
