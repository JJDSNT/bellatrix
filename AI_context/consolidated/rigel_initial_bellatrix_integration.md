# Initial Bellatrix / Rigel integration

The first integration slice adds Rigel as an optional, project-owned submodule
and establishes the lifecycle, MMIO, memory/DMA, and interrupt boundaries
without making chipset correctness depend on host wall-clock time.

# Motivation

Bellatrix needs to run both as the existing standalone Emu68 machine and as a
machine with Rigel providing the classic Amiga compatibility domain. The
integration must remain build-gated and must not move address decoding,
INTREQ/INTENA semantics, RTC policy, or chipset timing into the wrong owner.

# Solution adopted

`CONFIG_RIGEL=0` remains the default.
<!-- Correction, 2026-08-29: no longer true. CONFIG_RIGEL defaults to ON;
     CONFIG_RIGEL=0 still builds the chipset-less composition. See
     AI_context/issues/ISSUE-0068.md. --> `CONFIG_RIGEL=1` initializes and links
`external/rigel`, whose canonical repository is owned by this project.

Bellatrix `machine/` remains the memory-policy control plane. It programs the
Emu68 MMU from a region table and dispatches trapped transactions to providers.
`amiga/bus.c` forwards a single physical M68K address to Rigel; Rigel owns the
classic CIA, RTC, and custom-register decode. Unmapped machine regions finish
as open-bus transactions rather than falling through to a still-faulting host
address.

The Amiga IPL boundary uses Emu68's existing PiStorm-shaped protocol without
declaring Bellatrix to be a PiStorm: `INTF.IPL` is the cheap pending gate,
`amiga_irq_get_ipl()` pulls the authoritative level from Rigel, and the PiStorm
`WFE` loop supplies STOP liveness. CPU exception acceptance never clears the
Rigel-owned source. Native platform interrupts remain on `INTF.ARM`.

The Rigel build presents `$000000-$1FFFFF` as directly mapped Chip RAM. AROS
and Bellatrix consume one shared topology definition. The first page remains
reserved for vectors, `AbsExecBase`, and the boot marker; the remainder is an
AROS `MEMF_CHIP | MEMF_24BITDMA` region. High system RAM becomes `MEMF_FAST`.
Without Rigel, AROS preserves its former unified `MEMF_CHIP | MEMF_FAST` heap.
Rigel DMA reaches the Bellatrix-owned backing through 16-bit memory callbacks.

The same AROS ELF supports both compositions. The kernel build records its
composition, and QEMU/SD/release tooling adds `bellatrix.rigel=1` only for a
Rigel image. A Rigel build is named `Bellatrix.img`; a build without Rigel is
named `Emu68.img`.

Rigel RTC conversion and current-time services are supplied through host
callbacks. Freestanding allocation is likewise supplied by Bellatrix. No
host `localtime()` implementation or wall-clock chipset progression lives in
Rigel.

# Files changed

- `.gitmodules`, `external/rigel`
- `cmake/bellatrix-target.cmake`, `cmake/bellatrix-variant.cmake`
- `src/machine/{machine,bus,memory}.{c,h}` and the existing region table
- `src/amiga/{bus,irq}.{c,h}`
- `aros/arch/m68k-emu68/boot/{boot.c,mmakefile.src}`
- `aros/arch/m68k-emu68/include/bellatrix/memory_map.h`
- `patches/emu68/0012` through `0016`
- `scripts/{setup,build,make-sdcard,release}.sh`, `run.sh`, and
  `scripts/boot-timing.py`
- `README.md`

# Architectural impact

The integration keeps four independent boundaries:

- machine memory policy: Bellatrix `machine/` to the Emu68 MMU;
- CPU MMIO: Emu68 fault to Bellatrix bus to Rigel;
- classic DMA: Rigel to Bellatrix-owned guest memory;
- interrupts: Rigel IPL through `INTF.IPL`, native platform IRQ through
  `INTF.ARM`.

The Emu68 patches remain mechanism-only. They expose handled fault
transactions, preserve a level-held IPL, reuse gate/pull arbitration and guard
the optional secondary read result. They contain no Amiga address decode.

# Documentation updated

- `README.md`
- this consolidated integration record

# Validation

- Rigel unit tests: 27/27 passed before the submodule update was published.
- `CONFIG_RIGEL=1 ./scripts/build.sh`: passed.
- `CONFIG_RIGEL=0 ./scripts/build.sh`: passed.
- AROS validation rebuilt only `boot.o` and relinked
  `aros-emu68-m68k.elf` through the generated boot makefile.
- QEMU smoke with Rigel observed the `$E80000` classic-domain probe as open
  bus and continued through Exec, ColdStart, graphics, and driver startup.
- `CONFIG_RIGEL=1 ./scripts/setup.sh --verify`: 58 AROS patches, two
  AROS-contrib patches, and 16 Emu68 patches applied.
- shell syntax, Python syntax, and `git diff --check`: passed.

# Next steps

The current Bellatrix specification deliberately leaves the canonical virtual
progress unit unresolved. Emu68 instruction count must not be mapped 1:1 to
Rigel CCK/cycles without an explicit contract. Deadline-based
`rigel_step_until()` integration, timed chipset events, and their resulting IPL
transitions remain the next integration slice. Hardware validation is also
still required.
