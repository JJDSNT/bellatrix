# Interrupts

How a host interrupt becomes an m68k interrupt, and who owns the state that
makes that work.

This document currently covers only the standalone Emu68 path, because that is
all that exists. It is the place for the rest of the interrupt story as it
arrives — CIA and chipset sources, Paula's consolidation, and delivery under
the multicore runtime.

> **Correction, 2026-08-16 — "The path" below is no longer what runs.**
> `patches/emu68/0002-deliver-host-interrupts-as-an-ipl-not-through-a-shadow.patch`
> replaced steps 3 to 5 with a single store of level 6 into `INTF.IPL`: no
> `INTENA` test, no `ARMPending`, nothing for the guest to arm and nothing to
> acknowledge on the bridge. That is mechanism 2 below, *IPL injection*: the
> comparison further down still stands as written, but its Status column is out
> of date — the choice it leaves open went to IPL, on the grounds that a machine
> with no Paula has nobody to own INTENA/INTREQ. Note that this is the mechanism
> the document records as having been tried before and not settling delivery;
> what changed is the surrounding delivery path, not the verdict on the
> interface.
> `INT_shadow` still exists in Emu68 and the write handlers still update it
> (`vectors.c:441-466`), but `ARMPending` is now never set, so those updates
> reach nothing. The rest of the document stands: the comparison of the three
> mechanisms is what the decision was made from, and the shadow path is the
> right answer again the moment a real chipset owns those registers.
> Current behaviour is described at
> `aros/arch/m68k-emu68/platform/platform.c:20-44`.
>
> **This is a way station, not the destination.**
> [`New_emu68.md`](New_emu68.md) §3 and §14 describe where it goes: two
> independent interrupt domains — platform interrupts keeping the
> `INTF.ARM` → level 6 path, chipset interrupts belonging to Rigel — and
> `INT_shadow` deleted rather than left dormant, with `$DFF09A` reaching Rigel
> through the generic bus hook because *"there must not be two independent
> owners of the same chipset state."* So the registers stop being emulated by
> Emu68 in both directions: today nobody owns them, afterwards Rigel does.

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
- **Ownership is unresolved.** On a real Amiga these registers belong to Paula.
  Nothing here implements one yet, and no decision has been taken about what
  will. Whenever something does, it and Emu68's shadow become two candidate
  owners of the same two addresses — and the fast path's need to test the
  enable bits from assembly, without calling into anything, does not go away.
  The next section lays out the mechanisms that choice is between.

## Three mechanisms for delivering a host interrupt

Emu68 offers three ways to get a host interrupt into the m68k. One is
implemented here, one is the interface PiStorm itself uses, and one is a
sketch recorded for completeness. This section used to say the choice could not
be made until there was a chipset; **it was made on 2026-08-06 and the reasoning
is below** — for a machine with no chipset at all, the question of who owns the
interrupt state has a trivial answer, and the measurement that settled it is in
ISSUE-0007.

The first two are written up in the AROS/m68k-emu68 port's design note,
`arch/m68k-emu68/doc/host-interrupts.md` on
`JJDSNT/AROS feature/m68k-emu68-baremetal`, which is where the series in
`patches/emu68/` came from.

| | Owner of the interrupt state | What Emu68 is handed | Cost per interrupt | Size | Status |
|---|---|---|---|---|---|
| **Shadow registers** | Emu68 itself | guest writes to `0xdff09a` etc. | page fault + trap | ~100 lines | implemented here |
| **IPL injection** | something outside Emu68 | a level, already decided | a struct field write | ~13 lines | used by PiStorm; tried in the previous incarnation |
| **`HOSTIRQ` MOVEC** | nobody, in Amiga terms | guest `MOVEC` | one `MOVEC` | ~40 lines | sketch only |

### 1. Shadow registers — what this repository does today

Described above. Emu68 impersonates Paula's four interrupt registers, backed by
`INT_shadow`. The guest is unmodified Amiga code: it arms with a write to
`INTENA` and acknowledges with a write to `INTREQ`.

Its costs are the flip side of that: every arm and every acknowledge is a page
fault into `SYSWriteValToAddr()` — irrelevant at timer rates, real at network
rates — and the whole `0xdff000` page traps, so a guest must treat it as MMIO
rather than allocatable memory.

### 2. IPL injection — the interface PiStorm uses

`INTF.IPL` in `struct M68KState` is how an *external* interrupt controller
hands the CPU a level. On a real PiStorm that controller is the Amiga's own
Paula, read off the IPL0-2 lines
(`src/pistorm/ps_protocol.c:2252`, `ps_classic_protocol.c:879`). No register is
emulated, no address space is reserved, and the arbitration in
`ExecutionLoop.c:325` already honours the SR IPL mask like a real 68k.

It does not work on a stock build as shipped. The arbitration takes a
PiStorm-shaped branch:

```c
#if defined(PISTORM)                 /* PiStorm32: use INTF.IPL directly */
    if (ctx->INTF.IPL > level) level = ctx->INTF.IPL;
#else                                /* a stock build lands here */
    if (ctx->INTF.IPL) {
        ipl_level = GetIPLLevel();   /* → return 0 on non-PISTORM_CLASSIC */
        if (ipl_level > level) level = ipl_level;
    }
#endif
```

`GetIPLLevel()` is a `return 0` stub outside `PISTORM_CLASSIC`
(`ExecutionLoop.c:296`), so writing `INTF.IPL` opens the `if` and then loses
the level. Reaching it needs a patch, roughly thirteen lines — far smaller than
the shadow series.

The trade is the mirror of option 1: an IPL is a level, not a register, so
whoever writes it must already have made the INTENA/INTREQ decision. On its
own it gives an unmodified Amiga guest nothing to program — writes to
`0xdff09a` land in RAM.

**It was tried in the previous incarnation of this project and did not settle
the problem.** That build patched the arbitration to accept `INTF.IPL`
directly, and single-core Emu68 still hung in Exec idle at `pc=0xfc0f90`,
polling INTREQ, across 400 chipset frames and ~125M instructions with `ipl=0`
and `int32=0` throughout — VERTB generated underneath and never delivered. The
same symptom is on record from an earlier investigation still, and the
multicore path delivered correctly with the same interface. It was never
closed.

The lesson worth carrying: **choosing the mechanism does not settle delivery.**
The failure was not in `INTF.IPL` as an interface but in when the write happens
relative to the JIT's execution window, and two delivery paths built on the
same interface behaved differently. Whatever is chosen, that question is
separate and has to be answered on its own.

### 3. `HOSTIRQ` MOVEC — for completeness

**Not implemented, and not proposed.** Recorded because it is the third thing
Emu68's shape allows, not because it is a candidate.

Emu68 already puts its non-Amiga controls in m68k `MOVEC` control registers,
which work identically in PiStorm and stock builds: `DBGCTRL` `0x0ed`,
`DBGADDRLO` `0x0ee`, `DBGADDRHI` `0x0ef`, `JITCTRL2` `0x1e0` (read and write
cases in `M68k_LINE4.c`). A `HOSTIRQ` at `0x1e1` would follow that convention:

```
 bit  0   PEND      read: host IRQ pending; write 1: acknowledge
 bit  1   ENA       guest's master enable
 bit 31   PRESENT   reads 1 on any Emu68 implementing this
```

`PRESENT` exists because an unknown control register raises
`VECTOR_ILLEGAL_INSTRUCTION`, so feature detection would otherwise mean probing
around a trap handler. The pending bit has room: the `INTF` union
(`include/M68k.h:164`) declares five of eight bytes, leaving `+5`, `+6` and
`+7`.

Against it: a new ABI someone has to own forever, and a guest-side driver where
today there is none.

## Decided, 2026-08-06: IPL is the target, the shadow is for the chipset era

The choice this section calls open has been made, for the current goal — AROS
booting stably with no chipset at all.

**IPL injection is what this port aims at.** With no Paula and no chipset, there
is nothing for the guest to program: an interrupt is a level, and `INTF.IPL` is
the interface built to carry one. It costs no page fault per access and reserves
no address space.

**The shadow registers stay documented and are not deleted.** They are the right
answer to a different question — the one that arrives with a chipset, when
something really does own INTENA/INTREQ and an unmodified Amiga guest expects to
arm and acknowledge through `0xdff09a`/`0xdff09c`. That work becomes relevant
again then, and the series in `patches/emu68/0001` is where it lives.

What tipped it was not elegance. Measurement (ISSUE-0007) found that the
dominant boot failure is Emu68's ARM stack collapsing from ~700 bytes of normal
use to fully exhausted between two five-second samples — unbounded recursion
through `curr_el_spx_sync`, the synchronous exception vector that every shadow
access goes through. In two of the three first-captured stalls the guest address
in flight was `0xdff01e`, INTREQR. That does not prove the shadow path is what
recurses, and it should not be read as proof; but a mechanism that removes a
page fault from every interrupt-register access is worth having on its own, and
it removes this whole family of accesses from the faulting window.

**Still true, and still the hard part:** choosing the mechanism does not settle
delivery. `INTF.IPL` is a level with nothing to lower it, and the previous
incarnation's attempt hung in Exec idle with the interface working. The
de-assert protocol has to be designed before code is written — see
ISSUE-0010.

## Where the level goes, and what re-enables the ARM IRQ

This has now been derived from source twice, from scratch, because it is not
written anywhere. It is written here.

The full lifecycle of one host interrupt on this port:

```
ARM IRQ fires
   → curr_el_spx_irq (vectors.c)
        SPSR_EL1 |= 0x080          -- PSTATE.I set: IRQ masked after eret
        INTF.IPL  = 6              -- no INTENA test, no shadow
        eret
   → ExecutionLoop
        level 6 > SR IPL mask?  take the m68k autovector exception
        SR.IPL = 6
        INTF.IPL = 0               -- "the level has been consumed, so drop it"
   → guest level-6 handler services the BCM peripheral
   → guest writes SR (RTE, MOVE to SR, ANDI/ORI/EORI to SR)
        the JIT emits, conditionally on the new IPL:
            IPL < 6  →  msr DAIFClr, #7    -- ARM IRQ re-enabled
            else     →  msr DAIFSet, #7
```

**Nothing in the C source ever re-enables the ARM IRQ.** Grepping for `daif`
across `external/emu68` finds exactly one hit, in `PPC_ExecutionLoop.cpp`, which
this port does not run. The enable is emitted *as translated code* by
`M68k_LINE4.c` (RTE, MOVE to SR, STOP — around lines 1176, 1604, 1744) and
`M68k_LINE0.c` (immediate-to-SR, around line 880), through `msr_imm(3, 7, 7)`.
That is why a textual search comes up empty and why the reasoning has to be
redone unless it is recorded.

The consequence is the part worth carrying:

> **ARM interrupts are re-enabled only when the guest executes an instruction
> that lowers its own IPL mask below 6.**

A guest that never gets there — a fault inside the level-6 handler, a handler
that never reaches its RTE, a context switch that restores a high SR — leaves
ARM IRQs masked with nothing to unmask them. The machine then idles forever
with no further host interrupts, which looks exactly like the `logo` stall.

That is a fragility, not a diagnosis. Measured against the pooled log
(`out/boot-timing.jsonl`, ~200 runs carrying `pstate_irq_masked`), the
correlation is weak and not monotonic — mean masked ratio: icons 0.306,
logo 0.420, workbench 0.499 — and exactly **one** run showed IRQs masked in
every sample. That run stalled. So this is a real mechanism that has been
observed to happen once, not the general cause of the intermittency.

### And it is not what the stalls are — checked, 2026-08-13

The obvious follow-up was: in a stalled boot, is `PSTATE.I` masked with
`INTF.IPL` already clear, i.e. did an interrupt arrive and get lost? **No.**

`out/boot-timing/2026-08-13T172322Z/stall.txt`, a `logo` run, CPU#0:

```
PC=fffffff000388140  X12=fffffff000388140  X18=0000000034600a4c
PSTATE=60000205  -ZC- NS EL1h
```

`PSTATE` bit 7 is clear, so ARM interrupts are **enabled**. The PC is inside
the JIT code buffer with `X12` equal to it, so the ARM is executing translated
guest code, not parked in Emu68. Cores 1-3 sit at `0x3c5` with everything
masked, which is what a parked core looks like.

`X18` is the guest PC (`REG_PC` is 18): `0x34600a4c` resolves to
`emu68_trap_report+0xce`. That function ends in

```c
    for (;;)
        ;
```

deliberately. The guest took a fatal CPU exception, our trap probe printed the
register dump, and then parked by design. Everything observed — PC not moving,
interrupts enabled, ARM inside translated code — is that `for(;;)` being
executed.

So the stall is not interrupt delivery, and this whole line of enquiry is
closed. See `AI_context/consolidated/history/ISSUE-0007.md` for where it went instead.

## Both `INT_shadow` and `INTF.ARM` are dead code on this port

Also derived twice; also recorded here.

Since `patches/emu68/0003` made host interrupts arrive as `INTF.IPL`, nothing
writes `INTF.ARM` except the `INTENA`/`INTREQ` interception in
`patches/emu68/0001` — and the guest never touches those registers. That is not
inference: `platform/platform.c` says *"there is nothing for this port to arm
and nothing to acknowledge"*, `exec/dispatch.S` says *"no Paula write here"*,
and the bus observer added in `patches/emu68/0007` recorded **zero** accesses to
the whole classic 24-bit domain across a boot to Workbench.

So `patch 0001` is inert in every build this repository produces. Removing it
changes nothing observable — which makes it safe cleanup, and also means it
cannot be the fix for anything.

## What has to be decided later

Not which mechanism is nicer in the abstract, but:

- **Who owns INTENA/INTREQ** once there is a chipset. Option 1 answers "Emu68
  does"; option 2 answers "something else does, and only hands over a level".
  That is the real fork.
- **How the core-0 assembly fast path learns the enable state.** It reads
  `INT_shadow.INTENA` inline and decides without calling anything. Anything
  that moves ownership out of `INT_shadow` has to answer where that read goes
  instead — or accept that host interrupts stop using the fast path.
- **When the level is published relative to the JIT's quantum**, which is the
  part the previous attempt got wrong and never fixed.

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
