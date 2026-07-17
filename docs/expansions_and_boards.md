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

Three paths coexist right now. Convergence means collapsing them onto §1/§3.

| Path | Used by | Notes |
|---|---|---|
| Emu68 native `.boards` + `vectors.c` | **Emu68 backend** (product, `BELLATRIX_ENABLE_EMU68_BOARDS=ON`) | Emu68's own z2ram, 68040, sdcard, emmc, unicam, devicetree. Already the target model — untouched. |
| `board_registry` (§3) | harness / Musashi (foundation) | Same descriptor + self-registration; not yet the live consumer. |
| `expansion.c` + Zorro II/III registries | **lide** (§5) and the harness AutoConfig path | The older approach; kept alive for lide only. |

The real legacy to retire is neither of these: it is the **hardcoded memory-map
in the `run.sh` TUI** (`tools/launcher/tui.go`).

---

## 5. Board inventory

**Emu68 native — keep, never wrap.** `z2ram`, `68040`, `sdcard`, `emmc`,
`unicam`, `devicetree`, and `VideoCore.card` (the RTG equivalent). These are
Emu68's and come up through Emu68's own mechanism on the Emu68 backend. Do not
write Bellatrix wrappers around them — a `z3_68040` wrapper existed and was
deleted (2026-07-17) as redundant.

**RTG (`src/machine/expansions/rtg/`) — a lab that never worked.** An attempt at
a Bellatrix-specific P96 graphics board. It never reached a working state and is
not a reference. The target for RTG is Emu68's `VideoCore.card`. Deletion is
deferred (the blast radius is large: harness `main` `HARNESS_RTG`,
`screenshot.c`, the `rtg_rom_data` Docker ROM build, and the `cards/bellatrix.card`
tree). See `docs/rtg_design.md` (kept for design reference only, with a
correction header) and `AI_context/issues/ISSUE-0033`.

**lide (`src/machine/expansions/lide_cdrom/`, `external/lide.device`) — the one
board Emu68 does not provide.** It gives **ISO (CD-ROM)** and **HDF (hard-disk
image)** support. lide is a *mixed* board: a ROM plus side-effecting ATA/IDE
registers served per access via `expansion.c` `bus_ops`. **This is the sole
reason the older `expansion.c` + Zorro II/III registry path is kept alive**
(documented at the top of `src/machine/expansion.h`). `expansion.c` is *not*
orphaned — it is used by `machine_rigel*.c`, `lide_cdrom.c`, and
`src/plugin/plugin_loader.c`.

---

## 6. Convergence plan

1. **Done.** Super Buster as the Z3 decode authority; DIRECT/EXTERNAL/UNMAPPED
   classification shared by both backends (`cpu_bridge_classify`).
2. **Done (foundation).** `board_registry`: Emu68-style self-registration over
   the real `struct ExpansionBoard`, harness-tested.
3. **Next.** Wire `board_registry` into the live Musashi/harness bus so it, not
   the ad-hoc Zorro registries, drives AutoConfig there.
4. **Then.** Re-express **lide** as an Emu68-style DIRECT ROM board **plus** an
   EXTERNAL register window (routed by the Super Buster / bus dispatch). This is
   the mixed-region case (`AI_context/issues/ISSUE-0032` item 6) and the last
   holdout on the old path.
5. **Finally.** Retire `expansion.c`'s parallel registry and the `run.sh` TUI
   hardcoded memory-map. Decide RTG's fate against `VideoCore.card`.

---

## See also

- `AI_context/issues/ISSUE-0032` — Zorro III 32-bit contract (DIRECT + EXTERNAL).
- `AI_context/issues/ISSUE-0060` — vectors.c minimal contract, board mechanism.
- `AI_context/specs/SPEC-0001-cpu-memory-integration.md` — region→owner matrix.
- `docs/memory_architecture.md` — memory map and apertures.
- `docs/rtg_design.md` — RTG lab notes (superseded; see its header).
