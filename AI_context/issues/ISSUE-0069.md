# ISSUE-0069 — Port Bellatrix to Radxa Orion O6 (CIX P1 / Sky1)

**Status:** research — contract captured, no code yet
**Opened:** 2026-07-20
**Contract doc:** `docs/orion_o6_platform_contract.md`

## Goal

Add a second bare-metal target beyond Raspberry Pi 3: the Radxa Orion O6
(CIX P1 / CD8180, Armv9.2). Splits across two places:

1. **Emu68** (`emu68/` submodule): new `orion-o6` TARGET (boot/MMU/GIC/PSCI).
   Ships as a `patches/` diff. Template = the existing `virt` target (PL011 +
   GICv3 + PSCI), not `raspi`.
2. **Bellatrix PAL** (`src/host/orion-o6/`): new backend implementing `pal.h`,
   mirroring `src/host/raspi3/`.

## Emu68 target delivery (fork-as-staging)

The `orion-o6` target is Emu68-internal boot code (can't live in `src/host/`) and
is generic, not Bellatrix-specific → it belongs upstream. Carry it via a personal
**fork** of michalsc/Emu68 as the staging remote:

- Fork branch `orion-o6-target` off the pinned commit (`305f686`) holds the
  **clean generic target only** (start/GIC/PSCI/UEFI + `SUPPORTED_TARGETS`). Real
  git history → PR to michalsc when stable.
- `VARIANT=bellatrix` glue stays as `patches/` — never in the fork branch (would
  make the PR unmergeable).
- Bellatrix build consumes the branch as a **generated** patch:
  `git -C emu68 format-patch 305f686..orion-o6-target -o patches/`. Keeps the
  submodule-patches-only model; regenerate when the branch moves.

## What is known (see contract doc for full table)

- UART = PL011 @ `0x040B0000` (SPI 296) — `pl011_backend.c` is near drop-in.
- Timer = ARMv8 generic — `time.c` near drop-in.
- GICv3: GICD `0x0E010000`, GICR `0x0E090000`, ITS `0x0E050000`.
- SMP = PSCI v1.0 smc.
- Cores: 4×A520 + 8×A720.

## Boot entry: UEFI (decided)

Enter as a **UEFI application** (`.efi` on the ESP, `/EFI/BOOT/BOOTAA64.EFI`) →
collect via Boot Services → `ExitBootServices()` → bare-metal. This closes two
gaps for free: **GetMemoryMap()** → DRAM layout; **GOP** → framebuffer (no TRM
display driver needed). GICv3/PSCI/timer are still ours post-exit. Firmware must
be in **DT mode** (grab DTB from `EFI_DTB_TABLE_GUID`). Caveat: GOP is a dumb
linear fb — RTG acceleration loses the VC4 HVS (software composition only).

## Gaps (after UEFI decision)

- [x] ~~Firmware entry + memory map~~ → UEFI `.efi` + `GetMemoryMap()`.
- [x] ~~Display full rewrite~~ → `PAL_Video_*` is a thin GOP wrapper.
- [ ] Runtime SD/eMMC driver (`bcm_emmc.c` is Pi-specific). UEFI FS covers
      boot-time ROM/ADF load; runtime disk-swap still needs a native driver.
- [ ] Physical IRQ SPI numbers for the peripherals we actually drive.

## Plan (staged)

1. Emu68 `orion-o6` target from `virt`, with a **UEFI `.efi` entry** (efi_main:
   GOP + GetMemoryMap + DTB config table → ExitBootServices → machine setup).
   Boot to a serial banner over UART0.
2. `src/host/orion-o6/`: pal_debug + time (drop-in) → pal_core (PSCI/GIC) →
   physical_interrupts. `PAL_Video_*` = GOP fb wrapper. Audio/runtime-storage
   stubbed.
3. Bring up chipset headless (serial only) — validates the whole PAL.
4. Wire GOP framebuffer to Denise output (match BGRA/RGBA 32bpp).
5. Native runtime SD/eMMC + audio backends.

## Non-goals / notes

- Chipset (CIA/Agnus/Copper/Blitter/Denise/Paula/IPL) is fully portable and
  untouched — it includes only `pal.h`.
- Keep the Pi 3 target the default; Orion is additive.
