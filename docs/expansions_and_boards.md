# Expansions and Boards

*Status: current as of 2026-07-17.*

How Bellatrix models Zorro II/III expansion boards, how a board is discovered and
mapped, and where the code is heading. The short version: **Emu68's board model
is the target**, not a thing to reinvent. This document explains that model, the
Bellatrix pieces that mirror it, and the two things that keep an older path alive.

---

## 1. The Emu68 board model (the target)

Emu68 describes an expansion board with one minimal descriptor
(`emu68/include/boards.h`):

```c
struct ExpansionBoard {
    const void *rom_file;   // AutoConfig image (nibble-encoded); may be a ROM
    uint32_t    rom_size;   // region / backing size
    uint32_t    map_base;   // filled at AutoConfig from the guest-assigned base
    uint32_t    is_z3;      // 0 = Zorro II, 1 = Zorro III
    uint32_t    enabled;    // participates in AutoConfig
    void      (*map)(struct ExpansionBoard *); // installs the region
};
```

One struct, one function pointer. Key properties:

- **Self-registration by linker section.** A board drops its descriptor into a
  section (`.boards.z2` / `.boards.z3`); the set is collected by the linker
  (`__boards_start`) and walked by `emu68/src/aarch64/vectors.c`. There is **no
  central `register_board()` call** — adding a board is adding a `.c` to the
  build; removing it is deleting the file.
- **AutoConfig is answered from `rom_file`.** Reads in `$E80000–$E8FFFF` return
  `rom_file[addr - 0xe80000]` byte by byte. The guest-written base at offset
  `0x44` (Z3) / `0x48` (Z2) sets `map_base` and calls `board->map(board)`.
- **`map()` installs a memory region and nothing else.** `z2ram.c` identity-maps
  RAM; `68040.c` / `sdcard.c` / `emmc.c` / `unicam.c` map a read-only ROM that
  contains an m68k driver. After `map()`, steady-state accesses hit the MMU
  directly and never fault again.
- **Every native Emu68 board is DIRECT.** There is *no* per-access register
  callback in the board interface. A board is a ROM or a RAM aperture. Device
  register I/O (e.g. a storage controller) is serviced outside the board struct.

---

## 2. Region types: DIRECT vs EXTERNAL

Bellatrix classifies every CPU-space access into one of three kinds. Only the
first two ever reach a board.

| Kind | Meaning | Datapath |
|---|---|---|
| **DIRECT** | RAM/ROM backing (a board's `map()` region) | Installed in the backend and consumed *before* the bus bridge — Emu68 via `mmu_map()`, Musashi via a memory bank. Never faults per access. |
| **EXTERNAL** | A register window with side effects | Routed per access to a semantic owner. |
| **UNMAPPED** | Nothing there | Open bus. |

DIRECT is the whole of the Emu68 board model. **EXTERNAL is a Bellatrix-specific
need** — it exists because some boards we care about (see lide, §5) expose
side-effecting registers, which a pure `mmu_map()` model cannot express.

### The shared DIRECT control plane

`src/cpu/direct_region.{h,c}` is the backend-neutral registry for DIRECT
regions. A board's `map()` calls `cpu_backend_map_direct()`, which the backend
turns into its own primitive:

- **Emu68**: `mmu_map()` with `MMU_ALLOW_EL0`; removal restores an EL1-only
  mapping so 68k accesses fault through `vectors.c` again (`emu68_direct_region.c`).
- **Musashi / harness**: installs a bank/buffer the memory callbacks consult
  (`bellatrix_direct_region_read/write`).

The lookup cost is thus specific to Musashi; Emu68's steady state never consults
the table.

### The Super Buster owns Zorro III decode (EXTERNAL)

On real A3000/A4000 hardware the **Super Buster** gate array decodes and
arbitrates the Zorro III bus. Bellatrix gives it that role as the single decode
authority for the 32-bit Z3 space (`src/machine/bus/superbuster/superbuster.c`):

```c
SuperBusterZ3Decode superbuster_decode_z3(const SuperBusterState *s, uint32_t addr);
```

It classifies a 32-bit address against the configured Z3 slots, gated on the
chip's own `NBSTAB` bit ("Z3 bus available"). The CPU bridge
(`src/cpu/cpu_bridge.c`) consumes this through `cpu_bridge_classify()`:

- `<= 0x00FFFFFF` → **AMIGA_LOW**: normal 24-bit machine dispatch (chip RAM,
  custom, CIA, AutoConfig window, Z2).
- `> 0x00FFFFFF` and owned by a configured Z3 board → **Z3_EXTERNAL**: routed to
  the owner with the **full** address (never masked to 24 bits — masking aliased
  into chip RAM and produced a non-deterministic boot in 2026-07-03).
- otherwise → **OPEN_BUS**.

This classification is **shared by both CPU backends** through the bridge; only
the DIRECT datapath is backend-specific. That boundary — shared classification,
backend-specific memory install — is the design rule for this subsystem.

---

## 3. Self-registration in Bellatrix: `board_registry`

`src/machine/bus/board_registry.{h,c}` brings Emu68's self-registration idiom to
the parts of Bellatrix that have no Emu68 walker (the Musashi backend and the
POSIX harness). It reuses Emu68's **real** `struct ExpansionBoard` (it
`#include <boards.h>`), so a board authored once has one descriptor shape across
backends — no mirror type to keep in sync.

```c
BELLATRIX_REGISTER_BOARD_Z2(my_board);   // drops &my_board into a linker section
BELLATRIX_REGISTER_BOARD_Z3(my_board);
```

- The section uses a **C-identifier name** (`bellatrix_boards`, no dot) so the
  linker synthesises `__start_/__stop_` boundary symbols on any ELF target,
  **with no linker script**. (The bare-metal Emu68 backend can instead place a
  board into Emu68's own `.boards.z3` section and let its native `vectors.c`
  walker consume it — a per-build placement choice.)
- `bellatrix_boards_autoconfig_*()` walks the table exactly like Emu68's
  `vectors.c`: config reads from `rom_file`, base write fires `map()`, shutup
  skips to the next board.

### Discipline: boards must be direct link objects, never in a `.a`

An archive member with no otherwise-referenced symbol is dropped by the linker,
and a self-registering board *is* such a member — its registration is silently
lost, and the boundary symbols can vanish with it (link error). Both the harness
(`add_executable` source list) and the product (`BELLATRIX_SOURCES` target
sources) link boards as direct objects, so this holds — **as long as boards never
move into a static library.**

### Tested on the host

`tests/unit/test_board_registry.c` (`bellatrix_unit_board_registry`) drops two
test boards and proves **discovery → AutoConfig → map → access** end to end;
`tests/unit/test_superbuster_z3.c` proves the Z3 EXTERNAL decode + NBSTAB gating.
Both run in the POSIX harness with no hardware — the harness covers the exact
production registration mechanism.

`board_registry` is currently **foundation**: it is not yet wired into the live
machine bus. The live paths are described next.

---

## 4. What actually drives boards today

`board_registry` (§3) is the single AutoConfig authority on the live bus for
both backends: every access routes through `bellatrix_machine_read/write →
machine_dispatch` (Emu68 via `cpu_bridge.c`, Musashi via `musashi_backend.c`).
The old ad-hoc Zorro II/III registries (`zorro2_bus.c`, `zorro3.c`) have been
**deleted** (ISSUE-0066); nothing routes through them any longer.

Two things remain alongside `board_registry`, on purpose:

| Piece | Role | Why it stays |
|---|---|---|
| `expansion.c` `bus_ops` | Per-access read/write for an EXTERNAL board's *window* | `struct ExpansionBoard` has no per-access callback, so an EXTERNAL board latches its base via the walker (`map == NULL`) and serves its window here. Used by **lide** and **RTG**. |
| Emu68 native `.boards` + `vectors.c` | Emu68's own boards (z2ram, sdcard, emmc, devicetree, 68040) | Compiled only in boards mode (below); they self-register into Emu68's sections and are needed on the real Pi. |

### Build modes (`BELLATRIX_ENABLE_EMU68_BOARDS`)

After ISSUE-0066 this flag is **no longer** "legacy registry vs modern" — that
registry is gone. It now selects which *native board set* is compiled:

- **ON ("boards")** — compiles Emu68's native `src/boards/{z2ram,sdcard,68040,
  devicetree}.c`. These self-register into Emu68's `.boards.z2/.z3` sections and
  are walked by Emu68's `vectors.c`. Fast RAM comes from Emu68's `z2ram.c`.
- **OFF ("legacy", the `build.sh` default)** — those Emu68 native boards are not
  compiled; the Bellatrix `z2_fast_ram.c` board (via `board_registry`) provides
  Fast RAM, sized by `BELLATRIX_LEGACY_Z2_RAM_MB`.

In **both** modes the Bellatrix `board_registry` boards (lide, RTG, fast_ram) are
present and drive AutoConfig through `machine_dispatch`. The flag name still says
"legacy" only for historical continuity — it is not renamed.

**Direction:** the two modes exist because two board *walkers/sections* still
coexist — Emu68's native `vectors.c` walker (`.boards.z2`) and Bellatrix's
`board_registry` walker (`bellatrix_boards`). Convergence (§6) is to unify them
so a single walk sees every `struct ExpansionBoard`, at which point the flag
disappears. That waits on stabilising boards-mode (e.g. the sdcard board under
QEMU) and is a dedicated effort. The real legacy still to retire is the
hardcoded memory-map in the `run.sh` TUI (`tools/launcher/tui.go`).

---

## 5. Board inventory

**Emu68 native — keep, never wrap.** `z2ram`, `68040`, `sdcard`, `emmc`,
`unicam`, `devicetree`, and `VideoCore.card`. These are Emu68's and come up
through Emu68's own mechanism (boards mode). Do not write Bellatrix wrappers
around them — a `z3_68040` wrapper existed and was deleted (2026-07-17) as
redundant. `VideoCore.card` may inform a future Raspberry presenter, but it is
not the prerequisite or guest contract of Bellatrix RTG.

**RTG (`src/machine/expansions/rtg/`) — active portable framebuffer work.** A
Bellatrix P96 graphics board whose first laboratory version reached `InitCard`
but not mode selection or live scanout. As of ISSUE-0066 it is a
`board_registry` **Zorro III** EXTERNAL board (`is_z3=1`, `map==NULL`), served
per access via `expansion.c` `bus_ops`; its window decodes through the Super
Buster. The active direction (2026-07-18) is a backend-neutral linear VRAM and
scanout contract, modeled behaviorally after Minimig/MiSTer P96. Harness SDL and
a future Raspberry/Emu68 presenter consume the same state independently. It is
enabled with `HARNESS_RTG`. See `docs/rtg_design.md` and ISSUE-0033.

**lide (`src/machine/expansions/lide_cdrom/`, `external/lide.device`) — the one
board Emu68 does not provide.** It gives **ISO (CD-ROM)** and **HDF (hard-disk
image)** support. lide is *fully EXTERNAL*: its ROM is address-transformed
(nibble bootldr, BYTEWIDE device binary, odd address → 0xFF) plus side-effecting
ATA/IDE registers, so no DIRECT region can express it. AutoConfig via
`board_registry` (Zorro II, `map==NULL`); window served per access via
`expansion.c` `bus_ops`. **This is the sole reason `expansion.c` is kept alive**
(documented at the top of `src/machine/expansion.h`) — used by `machine_rigel*.c`,
`lide_cdrom.c`, and `rtg.c`.

---

## 6. Convergence plan

1. **Done.** Super Buster as the Z3 decode authority; DIRECT/EXTERNAL/UNMAPPED
   classification shared by both backends (`cpu_bridge_classify`).
2. **Done.** `board_registry`: Emu68-style self-registration over the real
   `struct ExpansionBoard`; wired as the live AutoConfig authority for both
   backends.
3. **Done.** lide re-expressed as an EXTERNAL `board_registry` Z2 board; RTG as
   an EXTERNAL `board_registry` Z3 board (exercising the Super Buster Z3 decode).
4. **Done.** Deleted the legacy `zorro2_bus.c`/`zorro3.c` registries and the
   harness-only plugin subsystem; live bus depends only on `board_registry`
   (ISSUE-0066).
5. **Next.** Unify the two board walkers/sections (Emu68 `vectors.c` `.boards.z2`
   vs Bellatrix `board_registry` `bellatrix_boards`) so one walk sees every
   board and `BELLATRIX_ENABLE_EMU68_BOARDS` can disappear. Depends on
   stabilising boards-mode (sdcard under QEMU).
6. **Finally.** Retire `expansion.c`'s per-access registry once `board_registry`
   grows a shared EXTERNAL-window serving mechanism; retire the `run.sh` TUI
   hardcoded memory-map. This refactor must preserve the portable RTG register/
   VRAM contract and does not depend on a Pi-only presenter.

---

## See also

- `AI_context/issues/ISSUE-0032` — Zorro III 32-bit contract (DIRECT + EXTERNAL).
- `AI_context/issues/ISSUE-0060` — vectors.c minimal contract, board mechanism.
- `AI_context/specs/SPEC-0001-cpu-memory-integration.md` — region→owner matrix.
- `docs/memory_architecture.md` — memory map and apertures.
- `docs/rtg_design.md` — RTG lab notes (superseded; see its header).
