# Bellatrix Source Structure (Suggestion)

## Goal
Keep the project organized around clear responsibilities, with low directory depth and small files.

## Structure

```text
bellatrix/
├── src/
│   ├── core/       # machine lifecycle (init, reset, scheduler)
│   ├── cpu/        # Emu68 integration
│   ├── memory/     # address space, RAM/ROM, MMIO
│   ├── bus/        # Zorro III, AutoConfig, routing
│   │   └── zorro3/ # split by responsibility (autoconfig, routing, map, device)
│   ├── chipset/    # custom chips (one folder per chip)
│   │   ├── agnus/  # dma, blitter, copper, bitplane, beam
│   │   ├── denise/
│   │   ├── paula/
│   │   └── cia/
│   ├── devices/    # floppy, hdf, input, rtc
│   ├── firmware/   # Kickstart, AROS
│   ├── host/       # video, audio, input, filesystem
│   └── debug/      # logging, tracing, monitor
│
├── tests/
├── docs/
├── referencias/
└── AI_context/

```

Rules
Keep files around ~500 lines (guideline, not strict)
Split by responsibility when needed
Keep structure shallow
One folder per chip (chipset/)
One folder per bus (bus/)

Architecture Notes
memory/ is the entry point for all reads/writes
bus/zorro3/ handles device routing and AutoConfig
devices/ are devices connected through Zorro III
chipset/ is only custom chips (Agnus, Denise, Paula, CIA)

Isolation
Emu68 → cpu/
Zorro III → bus/zorro3/
Host-specific code → host/