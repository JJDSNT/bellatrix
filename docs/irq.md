# Interrupts

How a host interrupt becomes an m68k interrupt, and who owns the state that
makes that work.

This document currently covers only the standalone Emu68 path, because that is
all that exists. It is the place for the rest of the interrupt story as it
arrives — CIA and chipset sources, Paula's consolidation, and delivery under
the multicore runtime.

## The path

A physical ARM IRQ on core 0 lands in `curr_el_spx_irq`
(`external/emu68/src/aarch64/vectors.c`, L148). The handler is written in
assembly and does the whole decision inline:

1. **Core check.** `MPIDR_EL1 & 3` — anything but core 0 branches to
   `IRQonOtherCores`. Only core 0 can raise an m68k interrupt.
2. **Mask IRQ on return** by setting bit 7 in the saved `SPSR_EL1`, so the
   handler is not re-entered before the guest has looked at it.
3. **Read `INT_shadow.INTENA`** and test `(INTENA & 0x6000) == 0x6000` — INTEN
   (bit 14) and EXTER (bit 13) both set, i.e. the guest has enabled the level-6
   external interrupt.
4. **Set `INT_shadow.ARMPending = 1`** — unconditionally, whether or not the
   test passed. The pending flag records that the host has something to say;
   the mask only decides whether the guest is told right now.
5. **If enabled**, write level 6 into the M68K context (`INTF.pint`). The JIT
   picks it up and takes the guest through its level-6 autovector.

The guest's handler then reads `INTREQR`, sees EXTER set, and runs its
interrupt server chain — ordinary Amiga code, unaware that any of this is
emulated. It acknowledges by writing `INTREQ` with clear+EXTER
(`(value & 0xa000) == 0x2000`), which drops both `INTF.ARM` and `ARMPending`.

Step 4 happening unconditionally is what makes late enabling work: a guest that
sets up its handler after the host has already signalled will find
`ARMPending` still set, and the `INTENA` write path re-evaluates `INTF.ARM` on
the spot rather than losing the event.

## `INT_shadow`

```c
struct INT_shadow {
    uint16_t INTENA;
    uint16_t INTREQ;
    uint8_t  ARMPending;
} INT_shadow;
```

Three fields, one global, in `vectors.c` (L98). The IRQ fast path reaches it
from assembly using `__builtin_offsetof` values passed in as operands (L285),
so **the field layout is load-bearing** — reordering the struct changes hand-
written assembly that the compiler will not check for you.

## The shadows are now guest-visible state

This is the part worth being careful about.

On PiStorm the shadow is a **cache**. A real Paula owns INTENA/INTREQ; the
guest's accesses travel over the bus and Paula answers. Emu68 snoops the values
coming back (L670–L699) purely so the assembly fast path can test the enable
bits without a bus round trip. If the shadow were wrong, the guest would still
see the truth — it would only cost a missed or spurious fast-path decision.

Standalone there is no Paula, so the patch series in `patches/emu68/` serves
those registers **from the shadow** (see [`emu68.md`](emu68.md)). The shadow
stops being a cache and becomes the interrupt controller: it is the only copy
of INTENA/INTREQ in the system, and what it holds is what the guest reads.

Three consequences:

- **Every write to `INT_shadow` is now guest-visible.** Code that used to be an
  internal optimisation detail is now programmer-visible hardware behaviour.
- **`INTREQR` reads back a value that was never stored.** Bit 13 is forced on
  whenever `ARMPending`, so the guest can conclude EXTER fired. The bit is
  synthesised at read time, not held in `INTREQ`.
- **Ownership is unresolved.** The architecture says Paula owns INTREQ/INTENA.
  When a Paula exists here, there will be two candidate owners of the same two
  registers, and the fast path's need to test the enable bits from assembly
  without touching a chipset does not go away. Reconciling those is an open
  design question, not a detail.

## Intercepts must respect access size

`SYSReadValFromAddr(value, value2, size, far)` fills `*value2` as well as
`*value` for `size == 16`. Upstream's implementation ends in a fall-through
`switch(size)` where `case 16` writes both, so this is automatic. An intercept
that returns early does not get that for free.

The first version of patch 0001 claimed `INTENAR`/`INTREQR` at **any** size and
filled only `*value`. A 16-byte read of those addresses therefore returned
"handled" with `*value2` untouched, handing the guest a register full of
whatever was on the stack. GCC 13 reports it as
`-Werror=maybe-uninitialized` at the use site in `SYSPageFaultReadHandler`.

Both intercepts now gate on `size <= 2`, which is all a 16-bit chip register
can be read with; wider accesses fall through to memory as before. The
autoconfig read next to them already gated on `size == 1`.

**The rule for anything added here later:** claiming an access is a promise to
satisfy it completely for the size requested. If an intercept only makes sense
at one or two widths, say so in the condition rather than in a comment — the
other widths must fall through.

Two things this rule has not yet been applied to:

- The **write** side (`INTENA`/`INTREQ`, L756 and L771) has no size gate. There
  is no uninitialised-data hazard, since the values are inputs, but a wide
  write to those addresses is currently swallowed as if it were a word write.
- A **32-bit** access spanning `INTENAR`+`INTREQR` (`0xdff01c`–`0xdff01f`) is
  one access to two registers. It currently falls through to memory, which
  reads RAM rather than the shadow — the same as before the series, and wrong
  in the same way.

## History

The uninitialised `*value2` was present from the first version of the series
and did not surface for a while: the build that carried it passed
`-Wno-error=maybe-uninitialized`, so the compiler's report was demoted to a
warning in a wall of build output. It was found by building the series clean.
