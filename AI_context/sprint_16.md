// AI_context/sprint_16.md

# Sprint 16 — CIA timer, floppy signal corrections, MFM encoder cleanup

## Date
2026-04-27

## Goal
Fix CIA 8520 timer advance algorithm; correct floppy step direction and /WPRO
signal; refactor MFM encoder for correctness; silence spurious TBE interrupts;
improve signal routing and logging in machine.c.

---

## Context

After sprint 15, AROS was executing deep into early init (~0xFE98xx) but still
blocking. The next investigation focused on the CIA timers, which Kickstart uses
for hardware detection and timer-based polling (CheckTimer / ciaint_timer
pattern). Analysis of the CIA code revealed two bugs:

1. `cia_timer_advance` could count multiple underflows in a single step call,
   masking the actual timing of the one underflow that should fire the interrupt.
2. Step direction was inverted: DIR=1 was incrementing the cylinder number but
   hardware convention is DIR=1 = step OUT (toward cylinder 0).

The session also discovered that UART TBE interrupts were being raised from
within the TX path even though the SERDATR register already reports TBE=1
permanently (instant-TX mode). The spurious IRQ was triggering INTREQ bit 0
(TBE), which could corrupt the IPL calculation at boot.

---

## Commits delivered this sprint

### Post-sprint-15 hotfixes (committed before this log)

| Hash | Summary |
|------|---------|
| `0a85a98` | paula_disk: replace printf with kprintf, add support.h include |
| `f4746aa` | machine: fix /CHNG and /RDY logic in machine_sync_floppy_pra |
| `8ff54e1` | paula_disk: fix MFM sector checksum — compute on raw bytes, not encoded |
| `02218ea` | floppy: fix drive ID — id_data=0x00000000 for 3.5" DD drive |
| `8ea65e1` | floppy: fix signal routing, remove TEMP hacks, add CIA-B register logging |

### New changes committed this session

See individual commit messages below.

---

## Key changes

### CIA timer algorithm (cia.c)

`cia_timer_advance` rewritten to eliminate the O(ticks/period) loop:

- If `ticks ≤ counter`: subtract and return 0 underflows (fast path, no IRQ).
- Otherwise: subtract `counter + 1`, set `counter = latch`, consume remaining
  ticks in one pass (capped at 1 additional underflow for one-shot mode).
- Returns exactly 1 when a single underflow occurred; the caller fires the IRQ.
- One-shot mode now stops correctly at underflow without re-arming.

`cia_timer_advance_events` similarly cleaned up; loop now exits with `break`
instead of setting `events = 0`.

**Why this matters**: Kickstart's `CheckTimer` writes TALO/TAHI and expects
exactly one TIMERA interrupt to reach ciaint_timer. The old loop could fire
multiple ICR events per `cia_step` call at high tick rates, causing phantom
interrupt storms.

### Floppy drive — step direction (floppy_drive.c)

Direction was inverted. Amiga hardware:
- DIR=1 (CIAB PRB bit 1 LOW→ bit decoded HIGH in our model): step OUT, cylinder--.
- DIR=0: step IN, cylinder++.

Old code had these backwards. Fixed with explicit comments.

### Floppy drive — /WPRO signal (floppy_drive.c/h)

Added `write_protected` field to `FloppyDrive`. New function `floppy_get_wpro()`:
- No media: returns 1 (line HIGH = no drive).
- Media + `write_protected=1`: returns 0 (active LOW = protected).
- Media + `write_protected=0`: returns 1 (writable).

All ADF images default to write-protected=1 on insert; eject clears it.

### Floppy drive — /DSKCHG on step without media

`floppy_step()` now keeps `disk_changed=1` when the drive is empty and a step
pulse arrives, instead of clearing it. The real hardware latches /DSKCHG LOW
until the first step *with a disk inserted*; without a disk the latch stays
asserted indefinitely.

### Paula — level-sensitive interrupt lines (paula.c/h)

Added `irq_line_level` field to `Paula`. `paula_irq_raise` and `paula_irq_clear`
now maintain this shadow register for the two level-sensitive inputs:
- `PAULA_INT_PORTS` (bit 3) — CIA interrupt line.
- `PAULA_INT_EXTER` (bit 13) — expansion slot interrupt.

All other INTREQ bits remain edge-latched (set by raise, cleared by INTREQ
write-with-clear). The field is not yet wired into `paula_compute_ipl`; this
is infrastructure for the next step where CIA-A ICR must be reflected correctly.

### Paula disk — MFM encoder refactor (paula_disk.c)

`checksum_even_odd()` replaced by `mfm_checksum()` that operates on the raw
(pre-encode) bytes:
- Computes XOR of 32-bit big-endian words from the source data.
- Does not mask with 0x55 (that step belongs to the even/odd encoding, not the
  checksum).
- Result stored via `put_u32be()` into a 4-byte buffer, then passed through
  `encode_even_odd()`.

The sector temp buffer (`uint8_t sector[544]`) was removed; header, label, and
data fields are built directly into the output or from the ADF source pointer.
Trailing gap generation fixed: no longer guards on `s == 0`.

### UART — TBE interrupt suppressed (uart.c)

`uart_raise_irq(u, UART_INTREQ_TBE)` commented out in both `uart_start_tx_shift`
and `uart_write_serdat`. Rationale: in instant-TX mode SERDATR always reads
TBE=1/TSRE=1, so Kickstart/AROS never waits for a TBE interrupt. The IRQ raise
was injecting bit 0 into INTREQ, which could cause spurious IPL=1 events during
boot and confuse the interrupt handler table setup.

### Machine — /WPRO routing + /DSKCHG and /RDY corrections (machine.c)

`machine_sync_floppy_pra`:
- `/WPRO` (bit 3) added: defaults HIGH, pulled LOW by `floppy_get_wpro()` when
  disk is write-protected.
- `/DSKCHG` no longer gated by ID mode: the change latch fires on insert/eject
  regardless of motor state; only cleared by a STEP pulse. Old code was hiding
  /DSKCHG during motor-off probe, which caused AROS disk detection to miss the
  inserted disk.
- `/RDY` (/DKRDY) ID-mode fix: the line should mirror `idbit` during motor-off
  probe (LOW when idbit=0, HIGH when idbit=1). Old code pulled LOW whenever in
  ID mode, giving the wrong drive identity sequence.

CIA-A write path now logs register name + value (same format as CIA-B):
```
[CIAA-W] pc=00fca1e2 reg=13 (ICR) val=7f
```

---

## Build status

- Harness (Musashi, Linux): green after commits
- Emu68 (AArch64 bellatrix): green

---

## Next steps

1. Wire `irq_line_level` into CIA-A ICR propagation — PORTS bit must stay
   asserted while any CIA-A interrupt is pending.
2. Investigate AROS stall beyond 0xFE98xx — capture btrace and identify the
   blocking register or signal.
3. VPOSW (0xDFF02A) / VHPOSW (0xDFF02C) write handlers.
4. CIA timer autostart: writing TAHI with START=0 should auto-start the timer
   (CIA-8520 hardware behavior documented in HRM). Kickstart's `ciaint_timer`
   relies on this.
