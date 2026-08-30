# The Demo Reel 3 oracle

Running the same demo on the same chipset, with a real Kickstart, on the host.
This is what tells us whether a difference on hardware is Bellatrix's or
Rigel's -- and it works.

## Why a Kickstart here is not a contradiction

Having no Kickstart is a property of the *port*. The harness is a host-side
test tool: a Kickstart in it is an oracle, not a dependency, in the same way
`tests/` uses reference images it does not ship.

## The recipe

```bash
# One-off: a copy of disk 1 whose startup-sequence runs the player itself,
# because the harness can click but cannot move the pointer, so the demo's
# icon cannot be reached.
cp "<Disk 1 of 2>.adf" out/oracle/dr3-auto.adf
printf 'FastMemFirst\nassign T: RAM:\nexecute ToRAM\nDemoReelData:Slish\nEndCLI >nil:\n' > /tmp/ss
PYTHONPATH=external/amitools python3 external/amitools/bin/xdftool \
    out/oracle/dr3-auto.adf delete s/startup-sequence + write /tmp/ss s/startup-sequence

out/rigel-harness/rigel-harness ~/bellatrix-legacy/src/roms/KS13.rom \
    --adf out/oracle/dr3-auto.adf --df1 "<Disk 2 of 2>.adf" \
    --headless --frames 4000 --cpu 68000 --chip 512 --slow 512 \
    --status 1000 --screenshot-every 800 --screenshot-dir out/oracle \
    --audio-out out/oracle/demoreel.wav
```

4000 frames is about 45 s of host time. The disk's own startup-sequence is
`FastMemFirst / assign T: RAM: / execute ToRAM / LoadWB / EndCLI`; only
`LoadWB` is replaced.

## Two things it established immediately

**512 KB is not enough, and the demo says so itself.** With `--chip 512` and
no slow RAM the first screenshot is a Workbench requester: *"Volume RAM is
full"*. `ToRAM` copies about 300 KB and `DoWeHaveMem` exists to print "This
Demo Requires a 1 Meg Machine". `--slow 512` -- an A500 with the trapdoor
expansion -- is the configuration.

**Rigel draws it.** At frame 2400 the display drops to 384x256 and the title
screen appears, red and blue on a speckled ground. `out/oracle/frame_004000.png`.

```text
frame 2000  DMACON=03f0 INTENA=602c  pc=00fc0f94   (Kickstart idle)
frame 3000  DMACON=03f0 INTENA=602e
frame 4000  DMACON=03f0 INTENA=61ac  pc=00c11456   (demo code in slow RAM)
```

`INTENA=61ac` has AUD0 and AUD1 set: the demo has enabled audio interrupts.
`DMACON=03f0` does not yet have the audio DMA bits, and the WAV is silent at
4000 frames -- the music starts later, or does not start.

## What it is for

- **A reference picture.** `--screenshot-every` against the same frame on
  hardware, which is what ISSUE-0071 already does for bus timing.
- **A reference sound.** `--audio-out` writes the Paula mix as a WAV, so
  "Bellatrix is silent" becomes answerable: silent here too means Rigel or the
  demo, silent only there means us.
- **A reference trace.** `--trace-cpu N`, `--trace-pc LO:HI`, `--break ADDR`
  and `--log regs,dma,irq,audio` say what the demo *does*, which is the
  question ISSUE-0079 is stuck on: `Slish` calls through a null base on
  Bellatrix and the harness can show the call succeeding.

## What the harness is missing

`rigel_input.h` has `JOY0DAT` -- Rigel can move a pointer -- but the harness
CLI only exposes `--lmb FRAME[:HOLD]`, a click where the pointer already is.
A `--mouse FRAME:X,Y` would remove the need to patch a disk to reach an icon.
