# Running the chipset integration on a Raspberry Pi 3

What to take, what to look at, and what to bring back. Written for the run that
answers the one question QEMU cannot: **how much does a colour clock cost on
real hardware?**

Everything here is `ISSUE-0068`. The measurement matters because the number it
replaces was withdrawn -- see the correction in that issue -- and because the
whole shape of the plan depends on the answer.

## Why this run exists

| | ns per colour clock | share of realtime |
|---|---|---|
| optimised native x86, idle | 35 | 800% |
| optimised native x86, Demo Reel 3 | 61 | 460% |
| QEMU on that x86, measured in the machine | 1365 rising to 1727 | 20% falling to 16% |
| **Raspberry Pi 3** | **unknown** | **unknown** |

1365 against 35 is a 37x penalty, which is ordinary for interpreted AArch64. If
the Pi is the usual 8x or so slower than a modern x86 rather than 37x, the
chipset lands near realtime and the wall that stops phase 3 under QEMU is an
emulator artefact. If it does not, Rigel's `ISSUE-0006` becomes the critical
path with a real target. **Nothing else about the plan can be decided first.**

## The packs are built

Two card images in `out/packs/`, with `LEIA-ME.txt` beside them saying what to
pull out of each log. Write either with `dd`; both carry Demo Reel 3 already,
and both need `nocomposition` on the kernel command line.

| pack | chipset display driver | boots at |
|---|---|---|
| `pack-A-render-path.img` | left off the card | normal speed |
| `pack-B-chipset-live.img` | on the card | unknown -- that is the question |

The difference is one file: `DEVS:Monitors/AmigaVideo`. `amigavideo.hidd` is
inert on its own; the monitor is what `AROSMonDrvs` runs at boot, and from that
point `cia.resource`'s init starts a CIA timer and the chipset runs for the
rest of the boot. `BELLATRIX_CHIPSET_DISPLAY=0` is what leaves it off.

## Rebuilding them

Each overwrites `out/images/`, so one at a time.

**1. The cheap one: does the render path work on real silicon?**

```bash
CONFIG_RIGEL=1 CONFIG_RIGEL_SELFTEST=1 ./scripts/build.sh
BELLATRIX_CHIPSET_DISPLAY=0 BELLATRIX_DEMOREEL=out/demoreel3 ./scripts/make-sdcard.sh
cp out/aros/sd.img out/packs/pack-A-render-path.img
```

This composes a known frame -- one bitplane of vertical stripes, two colours,
a copper list -- then **parks the clock**, so the boot finishes at normal speed.
It proves phases 1 and 2 on hardware and costs nothing.

**2. The real one: the chipset live for the whole boot.**

```bash
CONFIG_RIGEL=1 ./scripts/build.sh
BELLATRIX_DEMOREEL=out/demoreel3 ./scripts/make-sdcard.sh
cp out/aros/sd.img out/packs/pack-B-chipset-live.img
```

`amigavideo.hidd` and `DEVS:Monitors/AmigaVideo` are on the card, so
`graphics.library` has a second display driver and the chipset runs from
`cia.resource`'s init onward. This is the one that produces numbers under load.

`nocomposition` is still required on the kernel command line for anything to
appear on the framebuffer. `make-sdcard.sh` also puts `rigel` there, which is
what switches the classic chipset on: delete the word from `cmdline.txt` on the
card and the same files boot the chipset-less machine, which is the cheapest
way to ask whether a symptom is the chipset's at all.

## What to look for, image 1

```text
[BELLATRIX:RIGEL:SELFTEST] PASS time=1000 beam=... cia_ta=... intreq=0020 ipl=3
[BELLATRIX:RIGEL:DISPLAY] programming one bitplane
[BELLATRIX:RIGEL:FRAME] publishing 352x256 pitch=4096 at $01000000, ...
[BELLATRIX:RIGEL:CENSUS] frame=1 352x256 ... non-bg=704/1887 sum=eca14000 flags=08
[BELLATRIX:RIGEL:DISPLAY] clock parked; frame kept for the guest
```

Six census lines, all identical, is the pass. Then from a Shell:

```
DeniseView
```

It prints the descriptor and its own census, and **the two censuses must
match** -- that is the claim, and it held under QEMU with
`non-bg=704/1887 sum=ECA14000` on both sides. Without `NOWINDOW` it opens a
window and blits the frame into it, which is the first time any of this is
visible as a picture.

## What to look for, image 2

The only line that matters:

```text
[BELLATRIX:RIGEL:PERF] <CCK> CCK in <ms> ms over <calls> calls -> <ns> ns/CCK, <rate> CCK/s (<pct>% of realtime), <n> CCK/call
```

Twenty of them, every four million colour clocks. **Bring back all twenty**,
not just one: under QEMU the cost per colour clock grew through the boot, 1365
to 1727, as more chipset was programmed, and that trend is as informative as
the absolute.

Also worth noting: whether the boot reaches `STARTING SERVICES` at all, and how
long it takes. Under QEMU it does not, inside three minutes.

## If image 2 boots

Then phase 3 works on hardware and phase 4 is one Shell away:

```
CD DemoReel3
Execute ToRAM
Slish
```

The demo assigns `DemoReel3:` and `DemoReelData:` itself -- both disks are
merged into that one drawer precisely so its own `ToRAM` can. It wants 1 MB,
which `DoWeHaveMem` checks and reports.

Put the files there first, from your own ADFs:

```bash
tools/demoreel/install.sh "<disk 1.adf>" "<disk 2.adf>"
```

They are not in the repository and will not be: the demo is commercial software
from 1989.

## What is not worth doing on this run

Raising the Rigel event log past its 64-line bound. It is bounded because an
unbounded one emitted 170274 lines through the serial port and made a working
machine look hung. If you need to see what `amigavideo` programs, filter by
event type rather than lift the bound.


# 2026-08-30: ship the kernel, not the card

Several rounds of this were shipped as full 47 MB packs when only
`Bellatrix.img` had changed -- 4 MB -- and each one cost a card rewrite. The
AROS side has been unchanged since `amigavideo`, `cia.resource` and
`DeniseView` went onto it.

`docs/release.md` already says this and it was ignored:

> `Bellatrix.img.gz` the aarch64 kernel, to update in place
> ... The two loose files keep the exact names `config.txt` declares, because
> updating a card has to be a copy and never a rename.

So: `out/kernels/` holds the loose kernels, and updating is

```bash
cp Bellatrix-<variant>.img /media/you/BOOT/Bellatrix.img
```

A full pack is for a first installation, or when the AROS side changes -- and
when it does, that is worth saying out loud rather than leaving the recipient
to guess which of the two files matters.
