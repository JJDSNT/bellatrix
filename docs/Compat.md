# Bellatrix Operating System Compatibility Objective

**Status:** Architectural Objective  
**Scope:** Bellatrix / Emu68 / AROS m68k  
**Role of AROS:** Resident operating-system foundation and Amiga-compatible kernel environment

---

# 1. Purpose

Bellatrix shall not be limited to booting an AROS installation.

A core project objective is for Bellatrix to use the **AROS m68k kernel and resident system components as the operating-system foundation** while supporting three distinct 68k userland environments:

1. native AROS m68k;
2. legacy AmigaOS / Workbench using the historical BCPL-compatible DOS environment;
3. later AmigaOS / Workbench using the post-BCPL DOS/CLI environment.

The intended architecture is:

~~~text
Bellatrix
    │
    ▼
Emu68
    │
    ▼
AROS m68k kernel
    │
    ├── AROS m68k userland
    │
    ├── legacy AmigaOS / Workbench
    │      └── BCPL-compatible bootstrap
    │
    └── later AmigaOS / Workbench
           └── post-BCPL DOS/CLI bootstrap
~~~

In all three cases, **AROS remains the resident operating-system foundation**.

Bellatrix does not require a proprietary Kickstart ROM in order to run an AmigaOS installation.

Instead, the AROS m68k resident system assumes the role normally provided by the Kickstart-resident operating-system components.

---

# 2. Architectural Principle

The compatibility objective is:

> **One resident kernel, multiple compatible 68k userlands.**

Bellatrix shall not implement separate machine targets for AROS and AmigaOS.

The common execution environment remains:

~~~text
Raspberry Pi
    │
    ▼
Bellatrix
    │
    ▼
Emu68
    │
    ▼
AROS m68k kernel
~~~

The selected system volume and bootstrap environment then determine which userland is initialized:

~~~text
                         AROS m68k kernel
                               │
              ┌────────────────┼────────────────┐
              │                │                │
              ▼                ▼                ▼
          AROS SYS:       AmigaOS SYS:      AmigaOS SYS:
                             legacy             later
              │                │                │
              ▼                ▼                ▼
        AROS userland      BCPL path       DOS/CLI path
~~~

This distinction belongs primarily to the **AROS DOS/bootstrap compatibility layer**, not to separate Bellatrix machine implementations.

---

# 3. AROS as the Resident System

When running an AmigaOS installation, Bellatrix shall not replace AROS with another kernel.

Resident operating-system services continue to be provided by AROS, including equivalents of components historically supplied by Kickstart.

Conceptually:

~~~text
Traditional Amiga

Kickstart ROM
    │
    ├── exec.library
    ├── dos.library
    ├── graphics.library
    ├── devices/resources
    └── other resident components
             │
             ▼
       AmigaOS system disk
~~~

becomes:

~~~text
Bellatrix

AROS m68k
    │
    ├── exec.library
    ├── dos.library
    ├── graphics.library
    ├── devices/resources
    └── resident platform support
             │
             ▼
       selected system disk
             │
       ┌─────┴─────┐
       │           │
     AROS       AmigaOS
~~~

An AmigaOS installation is therefore treated as an **Amiga-compatible userland running on the AROS resident system**, rather than as a second kernel.

---

# 4. Supported Boot Environments

## 4.1 Native AROS m68k

This is the normal Bellatrix boot environment.

~~~text
AROS kernel
    │
    ▼
AROS DOS bootstrap
    │
    ▼
AROS SYS:
    │
    ▼
AROS Startup-Sequence
    │
    ▼
AROS userland
~~~

This path remains the reference implementation and must not regress as AmigaOS compatibility is extended.

---

## 4.2 Legacy AmigaOS / Workbench

Bellatrix shall support installations that depend on the historical BCPL-compatible AmigaDOS environment.

The relevant compatibility boundary includes concepts such as:

~~~text
BPTR / BSTR
Process
pr_SegList
pr_GlobVec
SegList
startup packets
BCPL calling conventions
Shell-Seg
CliInit semantics
~~~

The AROS m68k DOS implementation already contains portions of this compatibility infrastructure.

The objective is therefore **not to create a second DOS implementation**.

The objective is to identify, complete and validate the existing AROS BCPL compatibility path sufficiently for legacy AmigaOS/Workbench installations to bootstrap correctly.

Conceptually:

~~~text
AROS m68k kernel
       │
       ▼
AROS dos.library
       │
       ▼
BCPL compatibility environment
       │
       ▼
legacy AmigaOS system volume
       │
       ▼
historical Shell / CLI bootstrap
       │
       ▼
Startup-Sequence
       │
       ▼
Workbench / AmigaOS userland
~~~

---

## 4.3 Later AmigaOS / Workbench

Later AmigaOS versions use the evolved DOS/CLI environment rather than depending entirely on the original BCPL implementation.

Bellatrix shall support this environment through the existing AROS m68k compatibility interfaces wherever possible.

Conceptually:

~~~text
AROS m68k kernel
       │
       ▼
AROS dos.library
       │
       ▼
Amiga-compatible Process / CLI environment
       │
       ▼
AmigaOS system volume
       │
       ▼
Startup-Sequence
       │
       ▼
Workbench / AmigaOS userland
~~~

Compatibility should be implemented by improving the existing AROS interfaces rather than introducing AmigaOS-specific replacements for the kernel.

---

# 5. Common Filesystem Bootstrap

All environments should converge on the same general system-volume model.

After selecting and mounting a bootable volume:

~~~text
SYS:
 │
 ├── C:
 ├── S:
 ├── L:
 ├── LIBS:
 ├── DEVS:
 ├── FONTS:
 ├── Classes:
 └── other system directories
~~~

The contents of the selected volume determine the userland.

For example:

~~~text
AROS volume
    │
    └── SYS:S/Startup-Sequence
             │
             ▼
         AROS userland
~~~

or:

~~~text
AmigaOS volume
    │
    └── SYS:S/Startup-Sequence
             │
             ▼
        AmigaOS userland
~~~

The system-volume mechanism itself should remain generic.

---

# 6. Compatibility Boundary

The primary operating-system compatibility boundary is:

~~~text
             AROS kernel
                  │
             AROS DOS
                  │
        ┌─────────┴─────────┐
        │                   │
 process creation      executable loading
 CLI bootstrap         HUNK / SegList
        │                   │
        └─────────┬─────────┘
                  │
           Amiga DOS ABI
                  │
        ┌─────────┴─────────┐
        │                   │
    legacy BCPL        later DOS/CLI
        │                   │
        └─────────┬─────────┘
                  │
           AmigaOS userland
~~~

Changes required for AmigaOS boot compatibility should therefore normally live in the existing AROS m68k/DOS compatibility mechanisms.

They should **not** introduce parallel Bellatrix implementations of:

* Exec;
* DOS;
* filesystem semantics;
* process management;
* CLI;
* executable loading.

---

# 7. Existing AROS Foundation

The current AROS implementation already provides significant portions of the required infrastructure, including mechanisms for:

~~~text
Amiga HUNK loading
SegList handling
BPTR/BSTR semantics
Process creation
pr_SegList
pr_GlobVec
BCPL compatibility fixups
CLI initialization
startup packets
system-volume assigns
Startup-Sequence execution
~~~

Therefore, the initial engineering task is not to design this compatibility layer from scratch.

The required process is:

~~~text
inspect
   ↓
test
   ↓
identify first incompatibility
   ↓
complete existing mechanism
   ↓
test again
~~~

Compatibility work should be evidence-driven and based on actual boot failures.

---

# 8. No Duplicate DOS Implementation

A fundamental constraint is:

> **AmigaOS compatibility must extend the existing AROS DOS implementation rather than create a separate AmigaDOS implementation.**

The desired architecture is:

~~~text
                    AROS dos.library
                          │
          ┌───────────────┼───────────────┐
          │               │               │
      AROS ABI       legacy ABI      later Amiga ABI
          │               │               │
          ▼               ▼               ▼
       AROS apps      WB 1.x-era      later AmigaOS
~~~

not:

~~~text
             AROS kernel
                 │
        ┌────────┴────────┐
        │                 │
    AROS DOS         Amiga DOS clone
        │                 │
        ▼                 ▼
      AROS             AmigaOS
~~~

Code duplication at this boundary should be avoided.

---

# 9. Native Raspberry Pi Hardware Access

Bellatrix does **not** require the AmigaOS userland to be isolated from Raspberry Pi hardware.

Because the execution environment is m68k and platform drivers can themselves be implemented as m68k code, Bellatrix may expose native Raspberry Pi devices through drivers following Amiga-compatible operating-system interfaces.

For example:

~~~text
AmigaOS application
        │
        ▼
Amiga-compatible device API
        │
        ▼
m68k Bellatrix driver
        │
        ▼
Raspberry Pi MMIO / platform interface
        │
        ▼
Raspberry Pi hardware
~~~

The same principle applies to AROS applications:

~~~text
AROS application
        │
        ▼
AROS / Amiga-compatible API
        │
        ▼
m68k Bellatrix driver
        │
        ▼
Raspberry Pi hardware
~~~

Therefore, the relevant distinction is not whether the userland is AROS or AmigaOS.

The relevant distinction is whether a driver depends exclusively on AROS-specific interfaces or exposes an ABI usable by Amiga-compatible software.

---

# 10. Shared m68k Platform Drivers

Where practical, Bellatrix should prefer m68k platform drivers that can operate in both environments.

The ideal path is:

~~~text
                    m68k driver
                         │
              Amiga-compatible ABI
                         │
              ┌──────────┴──────────┐
              │                     │
          AROS userland        AmigaOS userland
~~~

Such drivers may directly manage Raspberry Pi hardware.

Examples may include:

~~~text
storage
timers
interrupt controllers
network interfaces
input devices
graphics
mailbox/property interfaces
other SoC peripherals
~~~

A driver that requires AROS-specific facilities remains valid, but is then considered an AROS-only platform driver rather than a generally Amiga-compatible Bellatrix driver.

This distinction should be explicit:

~~~text
                 Bellatrix m68k drivers
                         │
             ┌───────────┴───────────┐
             │                       │
     Amiga-compatible            AROS-specific
             │                       │
       AROS + AmigaOS               AROS
~~~

---

# 11. Hardware Model

Bellatrix therefore exposes two fundamentally different classes of hardware to 68k software.

## 11.1 Native Bellatrix / Raspberry Pi Hardware

Native platform hardware may be accessed through m68k drivers.

~~~text
68k software
     │
     ▼
OS device/resource/HIDD API
     │
     ▼
m68k platform driver
     │
     ▼
Raspberry Pi hardware
~~~

This path does not require Rigel.

---

## 11.2 Classic Amiga Hardware

Software that requires classic Amiga chipset semantics is handled through the Bellatrix machine bus and Rigel.

~~~text
68k software
     │
     ▼
classic hardware access
     │
     ▼
Bellatrix bus
     │
     ▼
Rigel
     │
     ▼
Amiga chipset model
~~~

These two mechanisms coexist.

They should not be conflated.

---

# 12. Relationship with Rigel

Rigel solves a different compatibility problem from the AROS operating-system compatibility layer.

AROS provides the **operating-system ABI and resident-system compatibility environment**.

Bellatrix m68k drivers provide access to **native platform hardware**.

Rigel provides the **classic Amiga hardware compatibility environment**.

The resulting architecture is:

~~~text
                       68k software
                            │
              ┌─────────────┴─────────────┐
              │                           │
         OS-level access           direct/classic
              │                    hardware access
              ▼                           │
     Amiga-compatible API                 ▼
              │                     Bellatrix bus
              ▼                           │
       m68k device driver                 ▼
              │                         Rigel
              ▼                           │
   Raspberry Pi hardware          Amiga chipset model
~~~

This separation is fundamental.

Rigel must not become an abstraction layer for native Raspberry Pi hardware.

Likewise, native Bellatrix drivers must not emulate classic Amiga hardware when a real operating-system device interface is sufficient.

---

# 13. AmigaOS and Native Bellatrix Devices

An AmigaOS userland running over the AROS resident system may use native Bellatrix devices when the corresponding m68k driver exposes a compatible interface.

For example:

~~~text
AmigaOS program
      │
      ▼
OpenDevice()
      │
      ▼
Bellatrix m68k device
      │
      ▼
native Raspberry Pi hardware
~~~

This means that Bellatrix is not merely reproducing an old Amiga machine.

It can expose hardware capabilities that did not exist in classic Amiga systems while retaining the Amiga programming model.

Conceptually:

~~~text
             Bellatrix 68k environment
                       │
          ┌────────────┴────────────┐
          │                         │
   native Pi devices         classic Amiga devices
          │                         │
   m68k drivers                    Rigel
          │                         │
          └────────────┬────────────┘
                       │
                 68k software
~~~

This is an intentional property of the platform.

---

# 14. Boot Selection

The eventual Bellatrix boot policy should allow the system volume or boot configuration to select the desired environment.

Conceptually:

~~~text
Bellatrix boot
      │
      ▼
AROS m68k kernel
      │
      ▼
select boot volume
      │
      ├── AROS installation
      │
      ├── legacy AmigaOS installation
      │
      └── later AmigaOS installation
~~~

This selection must not require different Emu68 targets or different Bellatrix machine implementations.

---

# 15. Compatibility Milestones

Development should proceed incrementally.

## Milestone 1 — Native AROS

~~~text
AROS kernel
    ↓
AROS SYS:
    ↓
AROS userland
~~~

This is the baseline and regression target.

---

## Milestone 2 — Amiga Executable Compatibility

Validate loading and execution of Amiga HUNK binaries from an Amiga filesystem under the normal AROS environment.

---

## Milestone 3 — Later AmigaOS Bootstrap

Use an AmigaOS system volume as `SYS:` and identify the first incompatibility encountered while executing its normal boot environment.

The objective is to fix compatibility at the existing AROS ABI/bootstrap boundary rather than introduce a parallel implementation.

---

## Milestone 4 — Legacy BCPL Bootstrap

Validate the historical process/CLI environment, including:

~~~text
pr_SegList
pr_GlobVec
BCPL_Fixup
startup packets
CliInit
Shell-Seg
~~~

Complete only the compatibility semantics demonstrated to be missing.

---

## Milestone 5 — Workbench

Reach a functional Workbench environment from both supported AmigaOS bootstrap families.

The resident operating-system foundation remains AROS in both cases.

---

## Milestone 6 — Shared Native Drivers

Validate that selected Bellatrix m68k drivers can be used from both AROS and AmigaOS-compatible userlands.

The initial objective should be to prove the model with a small number of devices rather than make every Bellatrix driver immediately cross-compatible.

---

## Milestone 7 — Mixed Hardware Compatibility

Validate representative software in three categories:

~~~text
OS-friendly applications
        │
        ▼
AROS / Amiga-compatible APIs
        │
        ▼
native Bellatrix m68k drivers


mixed applications
        │
        ├── OS APIs ──────────► native drivers
        │
        └── chipset access ───► Rigel


hardware-oriented applications
        │
        ▼
Bellatrix bus
        │
        ▼
Rigel
~~~

---

# 16. Non-Goals

This objective does **not** require:

* booting a proprietary Kickstart ROM;
* replacing the AROS kernel with AmigaOS;
* implementing separate Bellatrix machine targets for each userland;
* cloning AmigaDOS inside Bellatrix;
* duplicating AROS DOS;
* implementing Zorro merely to boot AmigaOS;
* routing native Raspberry Pi hardware through Rigel;
* hiding Raspberry Pi hardware from AmigaOS-compatible software;
* requiring all Bellatrix drivers to be AROS-specific;
* requiring every native Bellatrix driver to be immediately compatible with every AmigaOS environment.

---

# 17. Architectural Invariants

The central operating-system invariant is:

> **AROS remains the resident operating-system foundation regardless of which compatible 68k userland is booted.**

The central hardware invariant is:

> **Native Raspberry Pi hardware and classic Amiga hardware are separate hardware domains. Native hardware is exposed through Bellatrix m68k drivers; classic hardware semantics are provided through Rigel.**

Together:

~~~text
                         Bellatrix
                             │
                           Emu68
                             │
                       AROS m68k
                             │
          ┌──────────────────┼──────────────────┐
          │                  │                  │
       AROS             AmigaOS            AmigaOS
     userland            legacy              later
          │                  │                  │
          └──────────────────┼──────────────────┘
                             │
                   Amiga-compatible APIs
                             │
             ┌───────────────┴───────────────┐
             │                               │
       m68k native drivers            classic HW access
             │                               │
             ▼                               ▼
    Raspberry Pi hardware              Bellatrix bus
                                             │
                                             ▼
                                           Rigel
                                             │
                                             ▼
                                    Amiga chipset model
~~~

---

# 18. Project Objective

Bellatrix shall ultimately provide a single coherent m68k platform in which:

> **AROS m68k acts as the resident operating-system and compatibility foundation; native AROS software and both historical families of AmigaOS userland can execute from their respective system installations without requiring a proprietary Kickstart ROM; and m68k drivers may expose native Raspberry Pi hardware directly through Amiga-compatible operating-system interfaces, while Rigel independently provides classic Amiga hardware semantics where required.**

This is an explicit Bellatrix architectural objective and should guide future work on:

~~~text
AROS m68k DOS/bootstrap compatibility
AmigaOS ABI compatibility
Bellatrix m68k platform drivers
native Raspberry Pi hardware access
Rigel/classic chipset integration
~~~

The intended result is not merely an emulated classic Amiga.

The intended result is a **native m68k Bellatrix platform with AROS as its resident system, capable of running AROS and AmigaOS-compatible userlands while combining native Raspberry Pi devices with optional classic Amiga hardware compatibility.**
