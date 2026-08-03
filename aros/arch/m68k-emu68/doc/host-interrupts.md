# Host interrupts for a guest with no chipset

*Two options, for discussion. From the AROS/m68k-emu68 port.*

Line references are against Emu68 `9b4379a5c5`, which is current
`upstream/master` at the time of writing. Every reference below was checked
against a clean checkout of that commit.

## Who is asking, and why

We are porting AROS (m68k) to Emu68 on a Raspberry Pi 3, with no PiStorm and
no Amiga hardware anywhere. The guiding idea is "the Pi is an Amiga without
the chipsets": an m68k CPU with real RAM, and Pi peripherals reached
directly rather than Paula/Agnus/Denise.

That makes us, as far as we know, the second real consumer of Emu68 after
PiStorm, and the first for which there is no Amiga chipset behind any of it.

We need one thing that is not currently possible: **a guest with no Paula
has no way to receive a host interrupt.** This document records the problem,
the two ways we found to solve it, and which one we implemented.

We are not asking anyone else to do the work. Option A below is implemented,
builds, and is verified end to end; we are happy to submit it, adjust it, or
drop it in favour of Option B.

## The problem

On a stock build the core-0 IRQ fast path (`src/aarch64/vectors.c:156-166`,
and the FIQ twin at `:181-191`) reads:

```
adrp x1, INT_shadow
add  x1, x1, :lo12:INT_shadow
ldrh w0, [x1, #INTENA]
and  w0, w0, #0x6000        ; INTEN | EXTER
cmp  w0, #0x6000
mov  w0, #1
strb w0, [x1, #ARMPending]  ; unconditional
b.ne 1f                     ; no INTEN|EXTER -> no level 6
mov  x1, CTX
mov  w0, #6
strb w0, [x1, #INTF.ARM]
```

This block compiles into every build. But **every writer of
`INT_shadow.INTENA` is inside `#ifdef PISTORM_ANY_MODEL`**:

```
src/aarch64/vectors.c:377   INT_shadow.INTENA |= value & 0x7fff;
src/aarch64/vectors.c:380   INT_shadow.INTENA &= ~(value & 0x7fff);
src/aarch64/vectors.c:660   INT_shadow.INTENA = *value;
src/aarch64/vectors.c:663   INT_shadow.INTENA = (INT_shadow.INTENA & 0xff00) | ...
src/aarch64/vectors.c:666   INT_shadow.INTENA = (INT_shadow.INTENA & 0x00ff) | ...
```

against the `#ifdef` at `:314` and its `#else` at `:724`. On `VARIANT=none`
the field is zero from reset and stays zero. The gate is not merely closed by
default — no code path in the firmware can open it.

The interrupt does arrive: `ARMPending` is stored two instructions before the
gate, and we observed it set in a running image. It is then dropped.

**This also means nothing can regress.** Since no stock-build guest can be
receiving host interrupts today, adding a mechanism has no existing
behaviour to preserve.

### What we had to do without one

Our port previously located `INT_shadow` by scanning Emu68's compiled
instruction stream for the three instructions above, decoding the preceding
`adrp`/`add` pair, and writing the guest's mask directly into the firmware's
global. It worked. It was never shippable, and deleting it is the entire
motivation for this document.

## What the guest side looks like

Upstream's stated contract is that the CPU enters the level-6 autovector and
the OS talks to the interrupt controller itself to find the source. We agree,
and both options below preserve it exactly. For reference, this is already
how AROS is built on other targets:

| Target | Delivery | Decode and dispatch |
|---|---|---|
| `aarch64-native` | ARM vectors via `VBAR_EL1` | ARMCTRL banks → `krnRunIRQHandlers()` |
| `m68k-amiga` | level-N autovector | `INTENAR & INTREQR` → per-Paula-bit |
| `m68k-emu68` (ours) | level-6 autovector | ARMCTRL banks → `krnRunIRQHandlers()` |

Our port takes the decode layer from the AArch64 port unchanged — same
controller model, same `krnRunIRQHandlers()`, same `ictl_enable_irq()` /
`ictl_disable_irq()` hooks — and only the delivery layer is m68k-shaped.

That last part is not a preference. The 68000 interrupt model *is* levels 1-7
with autovectors; there is no `VBAR_EL1` equivalent to reach for. So the
question is not whether the guest should look like an Amiga. It is only
**where the interrupt mask and pending registers live.**

Both options below are the same two registers. They differ in address and in
how they are reached.

`rondoval/emu68-gic400-library`, which upstream offered as the behavioural
reference, confirms the model: it discovers the GIC-400 from the device tree,
drives it itself, EOIs at the GIC, and contains no Emu68-specific code. Its
entire hook is one line (`src/gic400_api.c:178`):

```c
AddIntServer(INTB_EXTER, &gicBase->dispatcher_interrupt);
```

That works because Emu68 impersonates Paula around it — the `INTENA` write
opens the gate, the `INTREQR` read has EXTER ORed in when `ARMPending` is
set, and the `INTREQ` write clears it. All three are PiStorm-only, which is
why the library cannot be followed on a stock build as things stand.

---

## Option A — emulate the interrupt registers on stock builds

**Implemented and verified.** Branch:
[`JJDSNT/Emu68 feature/host-irq-abi`](https://github.com/JJDSNT/Emu68/tree/feature/host-irq-abi)
(the branch name predates the change of approach).

Serve the registers the guest already knows, instead of inventing an
interface. Two files, +102/-14.

**`src/aarch64/vectors.c`** — `INTENA`/`INTREQ` writes and
`INTENAR`/`INTREQR` reads are emulated for non-PiStorm builds too, backed by
the same `INT_shadow`. The Amiga register enum moves above the `#ifdef` so
both branches share one definition.

The read side deliberately differs from the PiStorm one. There, `INTENAR`
and `INTREQR` *snoop* values coming back from a real Paula on the bus. With
no Paula present there is nothing to snoop, so they are **served** from the
shadow, and `INTREQR` reports EXTER whenever `ARMPending` is set. That is
what lets an unmodified Amiga level-6 handler conclude EXTER fired and run
its server chain.

**`src/aarch64/start.c`** — the custom register page is ordinary RAM on these
builds, so it is carved out of the guest mapping:

```c
mmu_map(0x00dff000, 0x00dff000, 4096, 0, 0);
```

This is the same one-line technique already used for the `0xdeadbeef` debug
page at `start.c:1373`, and it is what makes the emulation reachable at all —
without it the writes land in RAM and never fault. Everything else in the
page still behaves as memory.

PiStorm builds are untouched.

### Guest side

Arming is a write to `INTENA` with `SETCLR|INTEN|EXTER`. The acknowledge is a
write to `INTREQ` clearing `EXTER` — exactly what
`arch/m68k-amiga/kernel/amiga_irq.c`'s `PAULA_IRQ_ACK` does after running a
server chain. **The port contains no Emu68-specific interrupt code at all.**

### Verification

On `qemu-system-aarch64 -M raspi3b`, AROS/m68k driving a BCM283x System
Timer through the legacy ARMCTRL controller:

```
[exter] INTENAR    0x00006000
[exter] LEVEL6 entry 0x00000001 / [systimer] IRQ tick 0x00000001
[exter] LEVEL6 entry 0x00000002 / [systimer] IRQ tick 0x00000002
[exter] LEVEL6 entry 0x00000003 / [systimer] IRQ tick 0x00000003
```

`INTENAR` reading back `0x6000` is the guest's own `INTENA` write served from
the shadow. Ticks past the first prove the acknowledge works — without it the
level-6 line stays asserted and nothing further arrives.

### Costs, honestly

- **A page fault per interrupt-register access.** Every arm and every
  acknowledge is a trap into `SYSWriteValToAddr()`. At timer rates this is
  irrelevant; at network rates it is real. PiStorm already pays far more per
  bus access, so this is only a new cost for stock builds.
- **The whole `0xdff000` page traps.** Other accesses fall through to the
  normal memory path and behave as RAM, but the guest must treat the page as
  MMIO rather than allocatable memory. On a real Amiga it never was RAM, so
  this is natural — but it is a constraint a chipsetless guest has to be told
  about.
- **It puts chipset-shaped emulation into a chipsetless build.** Aesthetically
  odd. Against that: it is two registers, not a chipset, and it is code that
  already exists and is already maintained.

---

## Option B — a MOVEC control register

**Not implemented.** Described here because it is the natural alternative if
Option A is unwanted, and because the reasoning is worth keeping.

`MOVEC` is where Emu68 already puts everything non-Amiga (`JITCTRL` `0x1e0`,
`DBGCTRL`/`DBGADDRLO`/`DBGADDRHI` at `0x0ed`-`0x0ef`). It is privileged by
the architecture, per-CPU, collides with no memory map, and behaves
identically in PiStorm and stock builds.

### `HOSTIRQ` — control register `0x1e1`

```
 bit  0     PEND     read: host IRQ pending
                     write 1: acknowledge (clears PEND and INTF.HOST)
 bit  1     ENA      guest's master enable for host interrupt delivery
 bit  31    PRESENT  reads 1 on any Emu68 implementing this
```

There is deliberately no level field: upstream has stated the contract as
autovector level 6, so the six SR-write sites that compare against
`5 << SRB_IPL` (`M68k_LINE0.c:877,1321,1691` and
`M68k_LINE4.c:1174,1602,1742`) keep comparing against a constant.

`PRESENT` matters because an unknown control register currently falls through
to `EMIT_Exception(VECTOR_ILLEGAL_INSTRUCTION)` (`M68k_LINE4.c:2395`), so
feature detection would otherwise mean probing around a trap handler.

### Where the pending bit goes

The union at `include/M68k.h:164-173` makes this nearly free:

```c
union {
    struct {
        uint8_t ARM;        /* +0 */
        uint8_t ARM_err;    /* +1 */
        uint8_t IPL;        /* +2 */
        uint8_t RESET;      /* +3 */
        uint8_t PPC;        /* +4 */
    } INTF;
    uint64_t INT64;
} __attribute__((aligned(8)));
```

Five of eight bytes are declared, so `+5`, `+6` and `+7` are available, and
`M68kReportInterrupt()` carries the comment *"we have 8 slots in total"*
pointing at exactly them. A new `INTF.HOST` at `+5` is read by the `ldr64` and
`cbz` that already exist in every translated inner loop
(`M68k_Translator.c:598`), and `ExecutionLoop.c:325`
(`if (unlikely(ctx->INT64 != 0))`) picks it up unchanged. Mask and
configuration stay in the control register, which is only read on the cold
path.

Two notes on that structure: `INTF.RESET` is declared but referenced nowhere
in the tree, so the real question is which offset is unreserved rather than
whether one is free; and since Emu68 builds `elf64-bigaarch64`, `INTF.ARM` is
the *most* significant byte of `INT64`, which is fine for a non-zero test but
rules out reading `INT64` as an ordered priority.

### Estimated shape

One field in `struct M68KState`, one read case and one write case in
`M68k_LINE4.c`, the two assembly fast paths, one line in arbitration. Roughly
40 lines. Purely additive — `INTF.ARM` and the PiStorm path are untouched, so
no conditional compilation is needed.

---

## The two options side by side

| | A — Paula registers | B — `HOSTIRQ` MOVEC |
|---|---|---|
| New ABI surface | none | one control register, permanent |
| Emu68 change | ~100 lines, mostly moved | ~40 lines, all new |
| Guest-side code | none — plain Amiga idiom | small Emu68-specific driver |
| Cost per interrupt | page fault + trap | a `MOVEC` |
| Reserves guest address space | `0xdff000` page | nothing |
| Works with `gic400.library` as written | yes | no, would need porting |
| Status | implemented, verified | sketch |

Our reading: **A asks for less and gives more.** It commits upstream to no
new contract, reuses code that already exists and is already maintained, and
any existing Amiga-shaped guest works with it unmodified. B is the better
engineering answer for a machine that genuinely has no chipset, and is
cheaper at high interrupt rates, but it is a new interface someone has to
own forever.

We implemented A because it seemed the smaller thing to ask. We are equally
happy to implement B.

---

## Two incidental findings

Independent of either option. One is a live bug, the other latent. Happy to
send them separately and first — they are easier to review in isolation.

### `JITCTRL2` bit 29 does not read back for fast-path interrupts

The assembly fast path stores `INTF.ARM = 6` (`mov w0, #6`), while
`M68kReportInterrupt()` (`ExecutionLoop.c:553`) stores `1`. The `JITCTRL2`
read path (`M68k_LINE4.c:2366`) does `bfi(reg, tmp, 29, 1)` — a 1-bit field
insert, so only bit 0 propagates. `6 & 1 == 0`.

Delivery still works, because `ExecutionLoop.c:341` tests the byte for
truthiness. But a guest asking "is a host interrupt pending?" via `MOVEC`
gets `0` for every interrupt delivered through the fast path — that is, for
essentially all of them. Storing `1` instead of `6` in the two handlers would
fix it; the value is never used as a level.

### The core-0 branch of `IRQHandler()` is unreachable

`IRQHandler()` has a `cpu_id == 0` branch (`vectors.c:2462-2471`) that
acknowledges the GIC and calls `M68kReportInterrupt(1)` with no `INTENA`
gate. It cannot execute: `IRQHandler` has one caller, `IRQonOtherCores`
(`vectors.c:275`), reached only after `tst x0, #3` / `b.ne` — cores 1 to 3.
Core 0 is handled inline and `eret`s; the SP0 vectors go to `SYSHandler`.

Worth recording because it is the one piece of code that reads like an
ungated delivery path, and it would become a real inconsistency the moment
anything routes a core-0 interrupt through it.

### Adjacent: `STOP` on stock builds

`EMIT_STOP` waits on `INT64` only under `PISTORM_ANY_MODEL`
(`M68k_LINE4.c:1619`). On a stock build it masks `DAIF` and issues a bare
`wfi()` (`M68k_LINE4.c:1611`), which wakes on a masked IRQ and so happens to
work, but never consults `INT64`. If host interrupts become a supported guest
feature, this is probably worth revisiting — a stock-build guest calling
`STOP` is a normal idle path, not an edge case.

## Open questions

1. Is Option A acceptable in principle — chipset register emulation in a
   build with no chipset — or is that a line worth not crossing?

2. If B instead: is there already a plan for the free `INTF` slots? The
   *"we have 8 slots in total"* comment and the unused `INTF.RESET` suggest
   reservations we cannot see from outside. Any free offset works for us.

3. `MOVEC` or an MMIO window, if a new interface is wanted at all?

4. Should the two incidental findings go first, separately? We think yes.
