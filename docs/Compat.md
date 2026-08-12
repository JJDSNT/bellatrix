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

Bellatrix does not boot a Commodore/Amiga Kickstart ROM in order to run an AmigaOS installation.

Instead, AROS assumes the role normally provided by the resident Kickstart components.

---

# 2. Architectural Principle

The compatibility objective is:

> **One kernel, multiple compatible 68k userlands.**

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

The selected system volume then determines which userland is initialized:

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

This distinction belongs primarily to the **AROS DOS/bootstrap compatibility layer**, not to the Bellatrix machine architecture.

---

# 3. AROS as the Resident System

When running an AmigaOS installation, Bellatrix shall not replace AROS with another kernel.

Resident services continue to be provided by AROS, including the fundamental equivalents of the components historically supplied by Kickstart.

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
    └── Bellatrix platform support
             │
             ▼
       selected system disk
             │
       ┌─────┴─────┐
       │           │
     AROS       AmigaOS
~~~

Therefore, an AmigaOS installation is treated as an **Amiga-compatible userland running on the AROS resident system**, rather than as a second kernel.

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

This path remains the reference implementation and must not regress as AmigaOS compatibility is added.

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

The objective is to identify, complete and validate the existing AROS BCPL compatibility path sufficiently for a legacy AmigaOS/Workbench installation to bootstrap correctly.

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

# 5. Later AmigaOS / Workbench

Later AmigaOS versions use the evolved DOS/CLI environment rather than depending entirely on the original BCPL implementation.

Bellatrix shall support this environment through the normal AROS m68k compatibility interfaces wherever possible.

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

# 6. Common Filesystem Bootstrap

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

# 7. Compatibility Boundary

The primary compatibility boundary is:

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

# 8. Existing AROS Foundation

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

# 9. No Duplicate DOS Implementation

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

# 10. Bellatrix Hardware Independence

The userland compatibility mechanism must remain separate from Bellatrix hardware support.

For example:

~~~text
AmigaOS application
        │
        ▼
Amiga-compatible OS API
        │
        ▼
AROS resident component
        │
        ▼
Bellatrix driver
        │
        ▼
hardware
~~~

An AmigaOS application should not need to know that storage, graphics, networking or other platform services ultimately come from Raspberry Pi/Bellatrix hardware.

Likewise, the DOS compatibility layer should not need Bellatrix-specific knowledge unless strictly required by the boot medium.

---

# 11. Relationship with Rigel

Rigel solves a different compatibility problem.

AROS provides the **operating-system ABI compatibility environment**.

Rigel provides the **classic Amiga hardware compatibility environment** where required.

Therefore:

~~~text
                  Amiga software
                       │
          ┌────────────┴────────────┐
          │                         │
      OS-friendly              hardware-aware
      software                   software
          │                         │
          ▼                         ▼
     AROS APIs               chipset accesses
          │                         │
          ▼                         ▼
 Bellatrix drivers          Bellatrix bus
                                    │
                                    ▼
                                  Rigel
~~~

The ability to boot an AmigaOS installation must not imply that Rigel is required for every operation.

Software using normal operating-system APIs should remain capable of operating through AROS and Bellatrix native services.

Rigel is required only where classic Amiga hardware semantics are actually needed.

---

# 12. Boot Selection

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
```

This selection must not require different Emu68 targets or different Bellatrix machine implementations.

---

# 13. Compatibility Milestones

Development should proceed incrementally.

### Milestone 1 — Native AROS

~~~text
AROS kernel
    ↓
AROS SYS:
    ↓
AROS userland
~~~

This is the baseline and regression target.

### Milestone 2 — Amiga executable compatibility

Validate loading and execution of Amiga HUNK binaries from an Amiga filesystem under the normal AROS environment.

### Milestone 3 — Later AmigaOS bootstrap

Use an AmigaOS system volume as `SYS:` and identify the first incompatibility encountered while executing its normal boot environment.

### Milestone 4 — Legacy BCPL bootstrap

Validate the historical process/CLI environment, including:

~~~text
pr_SegList
pr_GlobVec
BCPL_Fixup
startup packets
CliInit
Shell-Seg
~~~

and complete only the missing compatibility semantics.

### Milestone 5 — Workbench

Reach a functional Workbench environment from both supported AmigaOS bootstrap families.

### Milestone 6 — Application compatibility

Validate representative software in three categories:

~~~text
OS-friendly applications
        │
        ├── normal AmigaOS APIs
        │
        ▼
       AROS

mixed applications
        │
        ├── OS APIs
        └── limited hardware access
                 │
                 ▼
              AROS + Rigel

hardware-oriented applications
        │
        ▼
       Rigel
~~~

---

# 14. Non-Goals

This objective does **not** require:

* booting a proprietary Kickstart ROM;
* replacing the AROS kernel with AmigaOS;
* implementing separate Bellatrix machine targets for each OS;
* cloning AmigaDOS inside Bellatrix;
* duplicating AROS DOS;
* implementing Zorro merely to boot AmigaOS;
* making AmigaOS aware of Raspberry Pi hardware;
* replacing native Bellatrix drivers with classic Amiga hardware emulation.

---

# 15. Architectural Invariant

The central invariant is:

> **AROS remains the resident operating-system foundation regardless of which compatible 68k userland is booted.**

Therefore:

~~~text
                  Bellatrix
                      │
                    Emu68
                      │
                AROS m68k
                      │
        ┌─────────────┼─────────────┐
        │             │             │
       AROS       AmigaOS        AmigaOS
     userland      legacy          later
        │             │             │
        └─────────────┼─────────────┘
                      │
            Bellatrix platform
                      │
             ┌────────┴────────┐
             │                 │
      native hardware        Rigel
                              │
                       classic chipset
~~~

---

# 16. Project Objective

Bellatrix shall ultimately provide a single coherent m68k platform in which:

> **AROS m68k acts as the resident system and compatibility foundation, while native AROS software and both historical families of AmigaOS userland can execute from their respective system installations without requiring a proprietary Kickstart ROM.**

This is an explicit Bellatrix compatibility objective and should guide future work on the AROS m68k DOS/bootstrap compatibility layer.
