# MMIO contact policy

This records the CPU/Core 2 ordering contract used by ISSUE-0049. It describes
host contact policy, not register ownership inside Rigel.

## Read classes

| Class | Registers | Contract |
|---|---|---|
| Published scalar | DMACONR, INTENAR, INTREQR | Atomic Core 2 snapshot; no rendezvous |
| Projected beam | VPOSR, VHPOSR | `f(cpu_time)` over coherent geometry; synchronous fallback |
| Temporal live | DSKDATR, ADKCONR, POTGOR/POTINP, SERDATR, DSKBYTR | Catch up and read owner state |
| CIA | CIA-A/B register windows | Synchronous contact until per-register policy exists |
| Static/input | JOYxDAT, POTxDAT, DENISEID and ordinary readbacks | Direct/live read; revisit with input publication |

Every synchronous read drains earlier posted writes before dispatch. Published
reads may bypass unrelated writes, but writes that change published state or
beam geometry are critical and force republication.

## Write classes

| Class | Registers | Contract |
|---|---|---|
| Critical trigger/control | DSKLEN, BLTSIZE, BLTSIZV, BLTSIZH, COPJMP1/2, DMACON, INTENA, INTREQ | Catch up, drain older writes, apply, republish |
| Beam/fetch boundary | VPOSW, VHPOSW, DDFSTRT/STOP, BEAMCON0 | Same; invalidates temporal interpretation |
| CIA | CIA-A/B register windows | Synchronous contact |
| Posted custom | Other custom writes, including pointer/data/display/audio/colour setup | SPSC order, applied at timestamp by Core 2 |

Pointer/setup writes preceding a critical trigger are consumed when the trigger
drains the queue. A later synchronous read also drains the queue, preserving
read-after-write without making every setup write a rendezvous.

When the posted ring is full, the producer never waits for Core 2: posting
returns false and `cpu_bridge` takes its synchronous lock+drain fallback. This
preserves program order and guarantees progress even when pause has stopped the
Core-2 host loop. `full_fallbacks` counts this pressure path. Normal Core-2
consumption applies only entries with `stamp_cck <= chipset_cck`; only an
explicit CPU contact may force-drain future-stamped entries as an ordering
barrier.

## Lifecycle invariants

- Reset limpa fila/snapshots e solicita rebase. Pause/resume e troca de modo
  são pedidos atômicos aplicados pelo Core 0, com rebase no contador e CCK
  correntes; portanto tempo de parede pausado nunca é recuperado.
- Mode changes must still reconcile entries timestamped under the prior mode;
  this lifecycle barrier remains open.
- Self-paced mode stamps writes at chipset "now", not unconstrained CPU time.
- Copper geometry changes require publication before later projections.
- Table-driven tests prevent new Rigel registers inheriting the wrong class.

## Wakeup contract

Core 2 configures the architectural timer event stream on its own PE. Ordinary
CPU progress is sampled by that fixed cadence and does not send one SEV per
block while the stream is active. MMIO contacts, queue pressure, shutdown and
other latency-sensitive transitions retain explicit SEV. Platforms without an
event stream report it inactive and keep the original publication wakeup.
