---
id: ISSUE-0080
title: "A workload that needs no display driver: the Hannibals demo"
status: open
priority: medium
type: research
owner: unassigned
created_at: 2026-08-30
updated_at: 2026-08-30
tags:
  - rigel
  - workload
  - fat
blockers: []
related_files:
  - tools/hannibals/install.sh
  - tests/gl/hannibals
---

# Why another workload

Demo Reel 3 draws through `graphics.library`, so on this target its bitplanes
go to the VideoCore and Rigel's Denise is handed nothing -- every census on
hardware has said `non-bg=0, sum=00000000` for the whole of a boot. Making it
draw means supplying `amigavideo`, which hangs (ISSUE-0081).

"The evil Hannibals from Mars" (`3ddemo2.adf`) needs none of that:

- its startup-sequence is `type s:text / add21k / hannidemo2.EXE` -- one disk,
  no icon, no Workbench;
- `Hannidemo2.EXE` opens **`dos.library` and nothing else**, and its only
  absolute chipset references are `$dff09a` (INTENA) and `$dff096` (DMACON),
  which is how a program says the operating system is off now;
- so it programs Denise itself. No producer to supply, no ownership to
  arbitrate between `vcgfx` and `amigavideo`.

A second workload, DPaint IV (`wb13.adf`, volume `DPaintIV`, `dpaint` 375 KB
at the root), is the counterpart: a custom screen through
intuition/graphics.library and heavy blitter use. The pair splits the machine
in half -- if Hannibals runs and DPaint does not, the fault is in the OS path
and not the chipset. DPaint needs ISSUE-0081 closed first.

# Two things found staging it

**`add21k` holds the task.** The demo's own startup-sequence runs it first. It
is a 512 KB-era helper that hands the top of chip RAM back to the system; here
it computed an address that exists nowhere and asked Exec to free a megabyte
at it:

```text
[MM] Attempt to free 1000064 bytes at 0x3061eea4
[MM] The block does not belong to any MemHeader
Recoverable Alert! Task: add21k -- Error: 0x0100000F
```

A recoverable alert holds the task, so the script never reached the demo. This
machine has 2 MB of chip RAM and nothing to reclaim, so it is skipped.

**FAT cannot spell the data file, and said nothing.** The demo's second half is
406 KB called `Har vi røget hash?` -- `LOAD` and m68k code at its head, so
program rather than decoration -- and `Hannidemo2.EXE` opens it by that name,
the string at offset `0x292`. The boot volume is FAT32 and FAT forbids `?`;
mtools dropped the file silently and the drawer looked correct in every
listing except the card's own.

`tools/hannibals/install.sh` now renames it to `hannidata` and patches the
name the binary asks for, both on the user's own extracted copy under `out/`.
`make-sdcard.sh` warns for the whole class (`? * : < > | " \`), which had never
come up because nothing else on the card used those characters. **It is a
standing limitation of booting from FAT32**, not a one-off.

# Current state

With the data file present the demo runs and still draws nothing:

```text
[hannibals] --- handing the machine over
   ... no "gave the machine back", so it has not returned
[CENSUS] frame=2000 ... bg=00000000 non-bg=0/1369 sum=00000000
```

AROS keeps running throughout -- the sampled PCs are ordinary Exec addresses
-- so the demo has not taken the machine over either. The liveness probe also
catches `pc=00001512` repeatedly, a low chip-RAM address that is neither the
kernel ELF (based at `0x30600000`) nor the heap, and which has not been
identified.

Nothing reaches Rigel: no census change, and the chipset event log -- capped
at 64 and therefore not truncated here -- reports nothing.

So the demo is alive, not drawing, and not in control. The next step is to
find out where it is: the same treatment that worked elsewhere today, which is
to get an address and resolve it, rather than to reason about what it ought to
be doing.
