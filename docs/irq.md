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
  design question, not a detail. The next section is the way out of it.

## The alternative: a control register instead of the shadows

The approach above was chosen from two, and the one not taken is worth keeping
on file because it becomes *more* attractive for this project over time. Both
are written up in the AROS/m68k-emu68 port's design note,
`arch/m68k-emu68/doc/host-interrupts.md` on
`JJDSNT/AROS feature/m68k-emu68-baremetal`, which is where the series in
`patches/emu68/` came from.

### What already exists

There is no ready-made shadow-free path — PiStorm does not offer one. What
exists is a **convention**: Emu68 already puts its non-Amiga controls in m68k
`MOVEC` control registers, and those work identically in PiStorm and stock
builds.

| Register | Purpose | Verified |
|---|---|---|
| `0x0ed` | `DBGCTRL` | `M68k_LINE4.c:2031` (write), `:2295` (read) |
| `0x0ee` | `DBGADDRLO` | `:2068`, `:2335` |
| `0x0ef` | `DBGADDRHI` | `:2080`, `:2347` |
| `0x1e0` | `JITCTRL2` | `:2092`, `:2359` |

`MOVEC` is privileged by the architecture, per-CPU, and collides with no memory
map. Building a host-interrupt path on it is additive — it touches neither
`INTF.ARM` nor the PiStorm code.

### The sketch: `HOSTIRQ` at `0x1e1`

**Not implemented.** Estimated at ~40 lines: one field in `struct M68KState`,
one read and one write case in `M68k_LINE4.c`, the two assembly fast paths, one
line in arbitration.

```
 bit  0   PEND      read: host IRQ pending
                    write 1: acknowledge
 bit  1   ENA       guest's master enable
 bit 31   PRESENT   reads 1 on any Emu68 implementing this
```

`PRESENT` exists because an unknown control register currently raises
`VECTOR_ILLEGAL_INSTRUCTION`, so feature detection would otherwise mean probing
around a trap handler. There is deliberately no level field: the contract stays
"autovector level 6", so the SR-write sites that compare against `5 << SRB_IPL`
keep comparing against a constant.

The pending bit has somewhere to go. The `INTF` union at `include/M68k.h:164`
declares five of eight bytes — `ARM`, `ARM_err`, `IPL`, `RESET`, `PPC` —
leaving `+5`, `+6` and `+7` free, and `M68kReportInterrupt()` carries a comment
pointing at exactly them. A new `INTF.HOST` there is read by the `ldr64`/`cbz`
already present in every translated inner loop, and `ExecutionLoop.c`'s
`if (unlikely(ctx->INT64 != 0))` picks it up unchanged.

### Which one this project should want

The AROS port chose the shadow path, and for it that was right: it has no
chipset at all, so serving the two Paula registers means the guest is plain
Amiga code with zero Emu68-specific driver.

**Our situation is different, and the difference cuts the other way.** Bellatrix
has a chipset, and Paula is meant to own INTENA/INTREQ. Keeping the shadow path
means Emu68 emulates two chip registers *while our own Paula also implements
them* — two owners of `0xdff09a` and `0xdff01c`, with the arbitration living
half in hand-written AArch64 assembly. `HOSTIRQ` sidesteps that completely: it
is not in chip register space, so Paula keeps the Amiga registers and the host
interrupt path stops competing for them.

| | Shadow path (current) | `HOSTIRQ` MOVEC |
|---|---|---|
| Emu68 delta | ~100 lines, mostly code motion | ~40 lines, all new |
| Guest-side code | none — plain Amiga idiom | small Emu68-specific driver |
| Cost per interrupt | page fault + trap | one `MOVEC` |
| Reserves guest address space | the `0xdff000` page | nothing |
| Conflicts with a real Paula | yes | no |
| Status | implemented, verified | sketch |

Costs of the current path, for the same reason they are recorded upstream:
every arm and every acknowledge is a page fault into `SYSWriteValToAddr()` —
irrelevant at timer rates, real at network rates — and the whole `0xdff000`
page traps, so a guest must treat it as MMIO rather than allocatable memory.

No decision is being made here. This section exists so that when the Paula
ownership question comes due, the alternative is already costed.

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

## Known defects in the surrounding Emu68 code

Found during the AROS port's investigation, re-verified here against pin
`9b4379a`. None is caused by the series; all three sit next to it.

**`JITCTRL2` bit 29 always reads 0 for fast-path interrupts.** The read path
does `bfi(reg, tmp, 29, 1)` from `INTF.ARM` — a one-bit field insert
(`M68k_LINE4.c:2365`). The assembly fast path stores `6` there
(`vectors.c:165`, and `:190` for the FIQ twin) while `M68kReportInterrupt()`
stores `1` (`ExecutionLoop.c:558`). `6 & 1 == 0`, so a guest asking "is a host
interrupt pending?" via `MOVEC` gets 0 for every interrupt delivered through
the fast path — which is essentially all of them. Delivery itself is
unaffected, because the dispatch path tests the byte for truthiness. Storing
`1` instead of `6` would fix it; the value is never used as a level.

**`STOP` does not consult `INT64` on stock builds.** `EMIT_STOP` waits on
`INT64` only under `PISTORM_ANY_MODEL` (`M68k_LINE4.c:1609`); otherwise it
masks `DAIF` and issues a bare `wfi()` (`:1611`), which happens to work because
`wfi` wakes on a masked IRQ. This one matters to us: a guest calling `STOP` is
an ordinary idle path, not an edge case, and Exec idles that way.

**The core-0 branch of `IRQHandler()` is unreachable.** It acknowledges the GIC
and reports an interrupt with no `INTENA` gate, but its only caller is
`IRQonOtherCores`, reached only for cores 1–3. Harmless today; it reads like an
ungated delivery path and would become a real inconsistency the moment anything
routes a core-0 interrupt through it.

## History

The uninitialised `*value2` was present from the first version of the series
and did not surface for a while: the build that carried it passed
`-Wno-error=maybe-uninitialized`, so the compiler's report was demoted to a
warning in a wall of build output. It was found by building the series clean.

Before the series existed, the AROS port reached `INT_shadow` by scanning
Emu68's compiled instruction stream for the three instructions that touch it,
decoding the preceding `adrp`/`add` pair, and writing the guest's mask straight
into the firmware's global. It worked, and it is why the series was written.
