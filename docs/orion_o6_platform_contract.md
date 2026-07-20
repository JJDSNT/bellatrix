# Radxa Orion O6 / CIX P1 (Sky1, CD8180) — Platform Contract

Register-level facts needed to bring Bellatrix up on the Radxa Orion O6.
Authoritative source: mainline Linux DTS series **v9** (`arm64: Introduce CIX
P1 (SKY1) SoC`, Peter Chen / cixtech) plus the CIX P1 TRM (released Dec 2025).

> Status: **research / not started.** No code written yet. This document is the
> "contract" the port is built against. See ISSUE-0069.

## Where the port lives (two places, two repos)

| Layer | Repo | What | Model |
|---|---|---|---|
| Boot / SoC | `emu68/` (submodule, read-only) | New **TARGET** `orion-o6` next to `raspi64`/`pbpro`/`rockpro64`/`virt` | ships as a `patches/` diff or upstream |
| Host / PAL | `src/host/orion-o6/` | New backend implementing `src/host/pal.h`, mirroring `src/host/raspi3/` | native Bellatrix source |

The build glue (`scripts/build.sh` + `cmake/bellatrix-*.cmake`) selects
`TARGET=orion-o6`, which compiles `src/host/orion-o6/` instead of `raspi3/` and
points Emu68 at the new start code.

**Key leverage:** UART is PL011 and the timer is the ARMv8 generic timer — the
same as the Pi. Combined with GICv3 + PSCI, the closest Emu68 start-code
template is **`virt`** (QEMU), *not* `raspi`.

## Platform contract

| Facility | Value | Source | Confidence |
|---|---|---|---|
| **UART (debug)** | PL011 (`arm,pl011`): UART0 `0x040B0000` (SPI 296), UART1 `0x040C0000` (SPI 297), UART2 `0x040D0000` (SPI 298), UART3 `0x040E0000` (SPI 299) | DTS v9 | High |
| **GIC** | GICv3: GICD `0x0E010000`, GICR `0x0E090000` (12 redistributors), ITS `0x0E050000` | DTS v9 | High |
| **SMP bringup** | PSCI v1.0, method `smc` | DTS v9 | High |
| **Timer** | `arm,armv8-timer` (generic timer, PPIs: secure/phys/virt/hyp) | DTS v9 | High |
| **Cores** | 12 total, Armv9.2: 4× Cortex-A520 (cpu0–3), 8× Cortex-A720 (cpu4–11). Marketing tri-cluster = 4×A720 big @≤2.6GHz + 4×A720 med + 4×A520 little | DTS v9 / SBCwiki | High |
| **Mailboxes** | CIX custom (`cix,sky1-mbox`), used for SCMI (clocks/power): ap2se `0x05060000`(SPI 378), se2ap `0x05070000`(SPI 379), ap2pm `0x06590080`(SPI 363), pm2ap `0x065A0080`(SPI 359), sfh2ap `0x08090000`(SPI 391), ap2sfh `0x080A0000`(SPI 392) | DTS v9 | High |
| **IRQ convention** | GIC_SPI, 4-cell format with level/edge flags. UART SPI = DTS number (e.g. 296) | DTS v9 | High |
| **DRAM / memory map** | **Firmware-provided.** No fixed memory node in DTS. LPDDR5, 128-bit, 5500 MT/s, 4–64 GB configs. Board boots via UEFI/EDK2. | DTS v9 / SBCwiki | Firmware-dependent |
| **Display controller (DPU)** | **Absent from mainline DTS.** GPU = Arm Immortalis-G720 MC10. Register-level display engine only in the CIX P1 TRM. | DTS v9 / SBCwiki | **Gap** |
| **SD/eMMC, USB, Ethernet, I2C, SPI, GPIO** | **Absent from v9 DTS** — upstream enablement is early-stage. Need TRM / later DTS revisions. | DTS v9 | **Gap** |

## Boot entry: UEFI (recommended path)

The O6 boots **TF-A (BL31, EL3) → UEFI/EDK2 (BL33)**. Unlike the Pi (loaded raw
by VideoCore firmware into a fixed address), Emu68 runs *inside* the UEFI
environment first, then takes the machine. Chosen path:

1. Build Emu68 as a **UEFI application** (`.efi`, PE/COFF with
   `efi_main(handle, systab)`), placed on the **ESP** (FAT EFI partition) as
   `/EFI/BOOT/BOOTAA64.EFI` for removable-media auto-boot. Nice symmetry with
   Bellatrix's existing FAT usage (analog of `kernel8.img`).
2. Before handing off, use **Boot Services** to collect everything.
3. Call **`ExitBootServices()`** and own the machine — pure bare-metal from here,
   identical to the Pi model.

**Why UEFI is the cheap path — it closes two of the four gaps for free:**

| Gap | Closed by |
|---|---|
| DRAM memory map | `GetMemoryMap()` — full layout before ExitBootServices. No TRM needed. |
| Display (`PAL_Video_*`) | **GOP (Graphics Output Protocol)** — linear framebuffer base/pitch/format/resolution. Stays valid after ExitBootServices (just a memory region). **No display-engine driver against the TRM.** |
| DTB source | UEFI config table `EFI_DTB_TABLE_GUID` (replaces the Pi's x0 DTB ptr). Requires firmware in **DT mode**, not ACPI. |

Unchanged post-ExitBootServices (we own them, same as Pi): **GICv3**, **PSCI**
(`PSCI_CPU_ON` via SMC works before and after — it is EL3/TF-A), **generic
timer**. UEFI does not alter the register contract above.

**UEFI caveats:**
- **GOP is a dumb framebuffer** — linear, fixed mode, no HVS/scaling/sprite
  overlay like VC4. Fine for the classic chipset (Denise renders in software →
  flip, which is already the model). But **RTG acceleration has no equivalent**:
  without the VC4 HVS it becomes software composition into the GOP framebuffer —
  a downgrade for the RTG path only.
- GOP format is usually BGRA/RGBA 32bpp — must match Denise output.
- **Storage boot-read** via UEFI Block I/O / Simple File System can read ROM/ADF
  from the ESP *before* ExitBootServices, softening the eMMC gap for initial
  load (runtime disk-swap still wants a native driver).

## Open gaps

1. ~~**Firmware entry + memory map.**~~ **Resolved via UEFI:** enter as `.efi`,
   `GetMemoryMap()` for DRAM layout. Remaining decision is only firmware config
   (DT vs ACPI mode → force DT).
2. ~~**Display full rewrite.**~~ **Reduced to a GOP wrapper:** `PAL_Video_*`
   returns the GOP framebuffer. TRM-level display-engine driver no longer
   required for basic scanout. (RTG acceleration still has no HVS equivalent.)
3. **Storage (runtime).** Bellatrix reads the SD card via
   `src/storage/sdcard/bcm_emmc.c` (Broadcom EMMC) — Pi-specific. UEFI FS covers
   the boot-time ROM/ADF load; a native MMC controller (not yet in mainline DTS)
   is still needed for runtime disk-swap.
4. **Physical IRQ routing** (`physical_interrupts.c`): once we pick which
   peripherals Bellatrix drives, pull their SPI numbers from the DTS.

## How the Emu68 target is carried (fork-as-staging, patch-as-derivative)

The `orion-o6` target is Emu68-internal (pre-machine aarch64 boot: start/GIC/
PSCI/UEFI entry) — it cannot live in `src/host/`. It is **not** Bellatrix-
specific either, so it belongs upstream. Workflow that keeps Bellatrix's
submodule-patches-only model intact while giving real git history to PR from:

**Split (must not mix):**

| Code | Home | Goes in the upstream PR? |
|---|---|---|
| Generic `orion-o6` target (start/GIC/PSCI/UEFI + `SUPPORTED_TARGETS` entry) | branch on a personal **fork** of michalsc/Emu68 | yes — this is what the PR contains |
| `VARIANT=bellatrix` glue (bus hook, exec loop) | `patches/` in Bellatrix | never — Bellatrix-specific |

If the Bellatrix glue leaks into the fork branch, the upstream PR becomes
unmergeable. The fork branch carries the clean target only.

**Flow:**

1. Fork michalsc/Emu68 → `github.com/<user>/Emu68`; add it as a second submodule
   remote `fork` (keep `origin` = michalsc to track upstream cleanly).
2. Branch off the **exact commit Bellatrix pins** (currently `305f686`):
   `git checkout -b orion-o6-target 305f686`. Basing on the pinned commit is what
   makes step 4 clean and lets `setup.sh`'s `git apply --reverse --check` work.
3. Develop the target as real commits; `git push fork orion-o6-target`. The fork
   branch is the **authoritative source**.
4. Feed the Bellatrix build by generating a patch from the branch (the patch is a
   derivative, not hand-written):
   `git -C emu68 format-patch 305f686..orion-o6-target -o patches/`.
   `setup.sh` applies it over the pristine submodule as today. Regenerate whenever
   the branch moves. New-files-only patches do not context-drift.
5. Open the PR to michalsc/Emu68 from `<user>:orion-o6-target` only when stable.

Net: real history + PR origin live in the fork; Bellatrix stays submodule-patches-
only; the upstream PR is clean of any Bellatrix code.

## Portability map (what moves, what doesn't)

- **~Drop-in (change base address only):** `pl011_backend.c`
  (`0x3F20_1000` → `0x040B_0000`), `time.c` (generic timer).
- **Rewrite (different SoC mechanism):** `pal_core.c` (PSCI/GIC vs Pi mailbox
  spin-table), `physical_interrupts.c` (GICv3), the Emu68 start code (model on
  `virt`).
- **Thin wrapper (via UEFI, no TRM):** `PAL_Video_*` over the GOP framebuffer;
  boot-time ROM/ADF read over UEFI Simple File System.
- **From scratch (no VideoCore equivalent):** `hdmi_audio.c`, native runtime
  SD/eMMC driver, RTG acceleration (no HVS — software composition only).
- **Untouched (fully portable):** all of the chipset — CIA, Agnus, Copper,
  Blitter, Denise, Paula, IPL injection via `TPIDRRO_EL0`. None of it references
  platform headers (it includes only `pal.h`).

## Sources

- DTS v9: https://patchew.org/linux/20250609031627.1605851-1-peter.chen@cixtech.com/20250609031627.1605851-9-peter.chen@cixtech.com/
- LWN, arm64 Introduce CIX P1 (SKY1): https://lwn.net/Articles/1017523/
- CIX releases P1 TRM (Dec 2025): https://www.cnx-software.com/2025/12/13/cix-releases-p1-cpu-trm-and-developer-guides-for-gpu-ai-accelerator-os-and-firmware-bios/
- SBCwiki CD8180-P1: https://sbcwiki.com/docs/soc-manufacturers/cix/cd8180-p1/
