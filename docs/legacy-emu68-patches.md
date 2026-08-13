# The legacy branch's Emu68 patches

What the pre-reset integration changed inside Emu68, why, and which of it this
tree still needs. It exists so our own series can be judged against something
other than its own reasoning: the `legacy` branch ran a working desktop, and
what it had to patch is evidence about what this problem actually requires.

Nothing here is a recommendation to restore anything. Several of these patches
serve subsystems this tree deliberately does not have.

## Where they are

Branch `legacy`, directory `patches/` — flat, and not only Emu68: the same
directory carried patches for Musashi, BTStack, CherryUSB, lide.device, odfs
and the VideoCore firmware. The fifteen below are the ones that touch Emu68.

```bash
git show legacy:patches/0003-bellatrix-execution-loop.patch
git ls-tree -r --name-only legacy | grep '^patches/'
```

They are plain diffs with no commit message, so the reasoning lives in the
comments they add. That is why the summaries below quote them.

## The numbering has holes, and they mean something

`0016`, `0021` and `0025`–`0034` are absent. The last block is the "public
machine API" (commits `c8599c8`–`f28b21a`, 2026-07-13): an attempt to replace
the fault handler with an explicit bridge. It was **abandoned** on 2026-07-15
and the patches were dropped;
`AI_context/consolidated/emu68_public_api.md` on that branch carries the header
*"DIREÇÃO DE PRODUTO SUPERADA"*. KS1.3 would not boot through the explicit
bridge while the same classifier over the old faulting path did.

That is a result, not an accident of history: **staying on Emu68's native
fault path is the option that survived contact.** Only `0035` was kept out of
that block.

## The fifteen

| # | Patch | Emu68 file | Kind |
|---|---|---|---|
| 0001 | `add-bellatrix-variant-cmake` | `CMakeLists.txt` | boundary |
| 0002 | `add-bellatrix-bus-hook` | `src/aarch64/vectors.c` | boundary |
| 0003 | `bellatrix-execution-loop` | `src/ExecutionLoop.c` | boundary |
| 0007 | `bellatrix-boot-sequence` | `src/aarch64/start.c` | boundary |
| 0008 | `bellatrix-console-redirect` | `include/support.h`, `src/raspi/*` | bring-up |
| 0009 | `bellatrix-boot-config` | `scripts/config.txt` | bring-up |
| 0010 | `bellatrix-z2ram-fixes` | `src/boards/z2ram.c` | fix |
| 0019 | `emu68-tlsf-hardening` | `include/tlsf.h`, `src/tlsf.c` | fix |
| 0020 | `emu68-stop-liveness` | `include/M68k.h`, `src/M68k_LINE4.c` | fix |
| 0022 | `bellatrix-bt-normal-irq` | `src/aarch64/vectors.c` | feature |
| 0023 | `bellatrix-host-only-irq` | `src/aarch64/vectors.c` | boundary |
| 0024 | `emu68-64bit-initrd-address` | `src/aarch64/start.c` | fix |
| 0035 | `emu68-modeled-cycles` | `src/M68k_Translator.c` | feature |
| 0036 | `emu68-bellatrix-native-framebuffer` | `src/raspi/start_rpi64.c` | feature |
| — | `generated/emu68-serial-kprintf-and-icache-debug` | `support.h`, `cache.c` | diagnostic |

### 0001 — the cmake variant

Four lines: adds `bellatrix` to `SUPPORTED_VARIANTS` and includes two cmake
files from the Bellatrix tree, one before the target exists and one after.
Every source list, option and define lives outside Emu68, so adding a file
never touches the patch again.

This is the shape our `patches/emu68/0006` copies, and it is why a 38-patch
integration still needed only one Emu68 patch for its build.

### 0002 — the bus hook

Even smaller:

```c
#elif defined(BELLATRIX)
#include "cpu/emu68/vectors.inc"
```

`SYSReadValFromAddr()`/`SYSWriteValToAddr()` are replaced wholesale by an
implementation in the Bellatrix tree. Same trick as 0001: the patch names a
file, the file is ours.

Our `0007` is deliberately lighter — two observation calls, with Emu68's own
implementation left intact. The trade is real: legacy's `vectors.inc` had to
track Emu68's fault-path internals, including its register conventions and its
board handling, and would need revisiting on every pin bump.

### 0003 — the execution loop, and the most important lesson on the branch

Calls `bellatrix_emu68_report_jit_progress()` from `MainLoop`, on every pass —
not from the fault path.

> "Advance the Bellatrix chipset on every pass through the loop, not only on
> MMIO faults. Without this, guest phases that touch no unmapped address
> (RAM-only loops, Wait/STOP idling) freeze chipset time: no VBL, no CIA, no
> IPL — hard deadlock."

**Routing and synchronisation are orthogonal.** The fault handler answers "what
is this address"; the progress driver answers "how much time has passed". The
fault handler never synchronised anything and never could: chip RAM is mapped
direct precisely so it does not fault, so a RAM-only loop produces no faults at
all. Gating the synchronisation block behind the *routing* mode flag removed it
from the build and the boot hung silently after `"[JIT] Let it go..."`.
`AI_context/consolidated/emu68_routing_vs_synchronization.md` on that branch is
the full account.

The patch also documents the register discipline the call needs, which is where
its second lesson is — see the register section below.

### 0007 — the boot sequence

Multicore placement: Core 0 hands the CPU backend to Core 1 and parks; Core 2
runs the chipset, Core 3 the IO. Also wraps `M68K_StartEmu()` (file-static
upstream) in a `void(void)` entry point the launcher can call.

Irrelevant to this tree as written — we are single-core — but it is the reason
`M68K_StartEmu` needed exporting at all, which is worth knowing before anyone
proposes calling it from outside again.

### 0008, 0009 and the console — the part that does apply to us

It is tempting to file these under "Bluetooth, which we do not have". That is
wrong, and the reason is worth stating first: **this tree logs over PL011.**
Emu68's `kprintf` drives the PL011 (`support_rpi.h`, `PL011_0_BASE`), and
`run.sh` wires QEMU's first `-serial` to it —

> "On raspi3b the first -serial is the PL011 that Emu68 logs to. Getting the
> order wrong is silence, not an error."

— so `scripts/boot-timing.py`, every verdict it produces and every finding in
`AI_context/issues/` rests on that one peripheral. The legacy branch gave PL011
to Bluetooth **unconditionally, in every build, BTSTACK compiled in or not**,
and had to move the console elsewhere to keep any logs at all. Anything that
takes PL011 here takes the measurements with it.

Three separable pieces, and only one of them is about Bluetooth.

**A putc override.** `0008` adds `kprintf_set_putc_override(fn)` and routes
`kprintf` through a dispatcher. That is a boundary in the same spirit as 0001
and 0002: patch Emu68 once, and decide where the console goes from outside it,
forever. Nothing in it is Bluetooth-specific.

**A kprintf kill switch.** The same patch adds `kprintf_set_enabled()` /
`kprintf_get_enabled()`, a runtime on/off for the whole console. This is not
about Bluetooth at all, and it is immediately relevant here: `src/machine/bus.c`
prints from the fault path, and the one measured disaster in this area is a
guest sweep turning the serial console into the bottleneck — 262144 aborts,
each with something to say. A switch is how you measure with the instrument off
without rebuilding.

**Where the console lives, and when it is decided.** The consolidated note on
that branch (`AI_context/consolidated/issue_logging_miniuart.md`) settles it by
copying Emu68's own PISTORM variant:

> "it picks a `kprintf` transport once, at the earliest point in boot, and
> never switches it."

That replaced a runtime handoff whose correctness depended on Bluetooth's
bootstrap state machine being right on every path. The generated debug patch
carries the early-init half of it — `bellatrix_console_log_init_early(core_hz)`
before the normal serial bring-up, with `serial_up = 1`.

**Two facts from that note that are hardware, not policy:**

- PL011 (GPIO 14/15, ALT0) and the mini-UART (same pins, ALT5) need *the same
  header pins*. They cannot coexist on real hardware. It is a pin conflict, not
  a sharing problem a buffer can solve — which is why `BELLATRIX_UART_PL011`
  was deleted outright rather than kept as a carve-out.
- The mini-UART's baud follows the core clock, which is why `0009` pins
  `core_freq=400`. On the Pi that is the difference between a readable log and
  garbage. `0009` also sets `hdmi_drive=2` for HDMI audio, which is the only
  genuinely inapplicable part here.

**The design principle underneath, which generalises past serial.** When both
Paula's emulated serial and `kprintf` shared one wire, `kprintf` stopped
writing hardware directly: it pushed into a non-blocking ring that drained
strictly *after* Paula's drain, on the same tick. Under contention the log
loses bytes and the guest's protocol never does.

That is the same rule the register clobber taught, in another costume: **an
instrument may degrade, but it may never corrupt what it observes.**

#### When Bluetooth arrives here

This is not hypothetical for this tree — Bluetooth is planned, and when it
lands PL011 belongs to it. So this section is a requirement, not a curiosity,
and the order matters because the failure is silent.

What breaks, and it breaks quietly: `run.sh` hands QEMU's first `-serial` to
PL011 because that is where `kprintf` goes today. Move the console and that
file goes empty. `scripts/boot-timing.py` drives `run.sh` and reads the serial
log, so every verdict becomes `logo` — not an error, a wrong answer. The
measurement chain has to move in the same commit as the console, and which
QEMU `-serial` position corresponds to the AUX mini-UART on `raspi3b` needs
checking rather than assuming.

What has to be true on real hardware: `core_freq=400` pinned in `config.txt`,
or the mini-UART's baud drifts with the core clock. And PL011 and the
mini-UART can never both be live, because they want the same header pins.

**The one thing worth doing early.** The putc override (`0008`) is a boundary
patch: it costs four lines in Emu68 and moves the decision permanently into our
tree. Taking it *before* Bluetooth means the transport switch is a change in
`src/`, decided once at the earliest point in boot. Taking it after means
patching Emu68 while debugging a BT bring-up with no console to debug it
through — which is exactly the position the legacy branch got into, and why its
first attempt was a runtime handoff whose correctness depended on Bluetooth's
own state machine being right on every path.

The kill switch (`kprintf_set_enabled`) belongs to the same patch and has a use
today, independent of any of this: measuring with the instrument silent.

### 0010 — z2ram fixes

Logging that prints the mapped range and the identity mapping explicitly, plus
an `else if` that fixes a fall-through in the `z2_ram_size=` parsing.

### 0019 — TLSF hardening

`TLSF_MULTITHREADING 1`, and alignment kept at 64 rather than upstream's 16:

> "the Cortex-A53 cache line is 64 bytes — a 16-byte-aligned buffer can share a
> cache line with an unrelated allocation, and DMA writes to it can corrupt
> that neighbor."

A DMA-coherency fix that only matters with a DMA-capable device driver in the
same heap. Worth re-reading if we ever put one there.

### 0020 — STOP liveness

Adds a modeled-cycle field and an exported STOP state, and stops `STOP` from
sleeping on `wfi`/`wfe`:

> "single-core Emu68 only advances Rigel's clock (and therefore VBL, and
> therefore any interrupt that could wake a real sleep) from the JIT's own
> progress hook and the bus-fault path, both of which require the CPU to still
> be retiring instructions."

Same failure family as 0003, and the same warning appears verbatim: a later
revision excluded this for the fault-driven build and fell through to a raw
`wfi()` that nothing could wake.

### 0022, 0023 — the IRQ pair

`0022` discriminates GPU IRQ 57 (UART0/BT) in the vector slot with only x0/x1
saved, and routes it through an out-of-line trampoline that spills x2–x30 and
q0–q31 before entering C. `0023` makes an unknown source mask IRQ on exception
return, count itself, and never set `INT.ARM`:

> "no host IRQ is implicitly propagated to AmigaOS."

That principle is the one our `patches/emu68/0003` reaches by a different
route, delivering host interrupts as an IPL level instead of through a shadow
Paula.

### 0024 — 64-bit initrd address

Reads the initrd address from the device tree as two cells rather than one.
A plain upstream limitation; a candidate to send upstream if it still applies.

### 0035 — modeled cycles

Emits, in the translated code, a load/add/store of `M68K_OpcodeCycles(opcode)`
into an accumulator. This is where legacy's cycle counter came from, and it is
the only survivor of the abandoned `0025`–`0034` block.

**We do not carry it**, which matters for reading legacy code: any legacy
comment about a cycle counter in a vector register describes a register this
tree does not use that way.

### 0036 — native framebuffer

Exports Bellatrix's software-Denise scanout to the guest and switches between
it and the P96 RTG plane. Needs a chipset; not applicable here.

### The generated debug patch

`kprintf` over serial plus icache diagnostics. Kept out of the numbered series
because it is an instrument, not integration.

## The register map is the trap

The single most transferable-looking fact on that branch is the one that does
**not** transfer. Legacy's Emu68 pin kept JIT state in `v28`–`v31`, and its
comments say so:

> "v28-v31 hold INSN_COUNT/CACR/FP flags and are not callee-saved"

Our pin does not. From `external/emu68/include/M68k.h`:

| What | Where, in our pin |
|---|---|
| m68k registers | `x13`–`x29` (`REG_PC` is 18, so the guest PC is `x18`) |
| translation-unit entry | `x12` |
| instruction count | `v20.d[0]` |
| m68k context pointer | `v20.d[1]` |
| CACR | `v21.s[0]` |

`CONTEXT_RESERVE_FLAGS` in Emu68's `CMakeLists.txt` reserves `v19`–`v26`
accordingly, and it is applied **per source file**, not globally — the
directory-level options pin `x12` and nothing else. A file added to
`BASE_FILES` inherits nothing else, and under AAPCS64 `v16`–`v31` are
caller-saved, so the compiler will happily use `v20` as scratch. Ours did,
until `cmake/bellatrix-variant.cmake` was given the same set `vectors.c` gets.

Two further facts from 0003 that are pin-independent and worth keeping:

- **`-ffixed-x12` does not stop GCC using `x12`** as prologue/epilogue scratch
  for large stack frames. Measured: a function with a big frame clobbered it
  and the guest jumped to the frame pitch (`ISSUE-0038` on that branch).
- `M68K_SaveContext`/`M68K_LoadContext` **do not cover `x12`**, and `v28` is
  assumed zero by the JIT and only cleared at `MainLoop` entry.

## Our series, for comparison

| # | Subject | Legacy equivalent |
|---|---|---|
| 0001 | Emulate the Amiga interrupt registers on non-PiStorm | none — legacy had a real chipset |
| 0002 | Make IPL injection reachable on stock builds | part of 0003's area |
| 0003 | Deliver host interrupts as an IPL, not through a shadow Paula | same principle as 0023 |
| 0004 | Report the first re-entry of SYSHandler, not the last | none |
| 0005 | Answer unmapped guest addresses with open bus, not a fault | none |
| 0006 | Hand the memory policy to the machine | 0001, plus a call site |
| 0007 | Report a guest access to the machine before serving it | lighter 0002 |

What legacy had and we do not, that is not about a missing subsystem:

- **A progress driver (0003, 0020).** We have no chipset, so nothing needs
  advancing — but the shape of that failure is worth holding onto, because it
  is invisible: no fault, no output, no crash, just a boot that stops.
- **A console boundary (0008).** The putc override and the kprintf kill switch.
  Four lines in Emu68, and the only one of these with a use today *and* a known
  future requirement: Bluetooth takes PL011, which is where our whole
  measurement chain currently listens.
- **`0024`**, if the initrd address ever needs two cells here.

What we have and legacy did not: everything about being a machine with no
chipset at all — the interrupt registers, the open-bus answer, and the
re-entry report. Those have no counterpart there because that branch always
had something on the other side of the bus.
