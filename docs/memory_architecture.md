// AI_context/memory_architecture.md
//
// Bellatrix – Memory Architecture and AROS Boot Model
//
// Purpose:
// Define a correct AND practical Amiga-compatible memory architecture
// for Bellatrix, enabling AROS boot while keeping the system extensible
// toward Zorro II/III and AutoConfig.
//
// This document replaces the previous bus-centric model with a
// memory-subsystem architecture based on memory_map.
//
// -----------------------------------------------------------------------------
// 1. Problem Summary
// -----------------------------------------------------------------------------
//
// Initial state:
//
// - Only Chip RAM implemented (0x000000–0x1FFFFF)
// - CPU accessed Chip RAM directly (CHIP_RAM_VIRT)
// - Other regions handled via traps / bus interception
//
// Result:
//
// - AROS boots partially but stalls early
//
// Root cause:
//
// AROS expects:
//
//   ✔ Fast RAM (Zorro II or higher)
//   ✔ Coherent memory map
//   ✔ Predictable address space (not traps)
//
// Key insight:
//
// AROS behaves like a modern OS, not legacy Kickstart:
//
//   → It performs memory probing early
//   → It expects contiguous, valid memory regions
//
// -----------------------------------------------------------------------------
// 2. Reference Model (Omega Emulator)
// -----------------------------------------------------------------------------
//
// Known-good layout:
//
//   CHIP RAM     : 0x000000–0x1FFFFF
//   FAST RAM     : 0x200000–0x9FFFFF   (Zorro II)
//   CIA          : 0xBF0000–0xBFFFFF
//   CUSTOM       : 0xDFF000–0xDFF1FF
//   AUTOCONFIG   : 0xE80000–0xEFFFFF (idle / no board in Phase 1)
//   ROM          : 0xF80000–0xFFFFFF
//
// Critical:
//
//   Zorro II region is used as FAST RAM
//
// This is sufficient for AROS boot.
//
// -----------------------------------------------------------------------------
// 3. Bellatrix Target Architecture
// -----------------------------------------------------------------------------
//
// Bellatrix now uses a structured memory subsystem:
//
//   CPU / DMA / Chipset
//           ↓
//     bellatrix_mem_*()
//           ↓
//     memory_map_decode()
//           ↓
//     region handler (chip / fast / rom / etc)
//
// Key components:
//
//   memory.c        → public API
//   memory_map.c    → address decoding (SOURCE OF TRUTH)
//   chip_ram.c      → Chip RAM implementation
//   fast_ram.c      → Fast RAM implementation
//   overlay.c       → ROM overlay logic
//   autoconfig.c    → empty placeholder until dynamic Zorro boards exist
//
// -----------------------------------------------------------------------------
// 4. Phase 1 – Minimum Viable AROS Boot
// -----------------------------------------------------------------------------
//
// Required memory layout:
//
//   0x000000–0x1FFFFF  → Chip RAM
//   0x200000–0x9FFFFF  → Fast RAM (static)
//   0xDFF000–0xDFFFFF  → Custom chips
//   0xF80000–0xFFFFFF  → ROM
//
// Everything else:
//
//   → can return default values (0xFF / 0xFFFF)
//
// IMPORTANT:
//
// Fast RAM is STATIC in Phase 1.
//
// No AutoConfig board is exposed yet.
//
// -----------------------------------------------------------------------------
// 5. Implementation Requirements
// -----------------------------------------------------------------------------
//
// 5.1 Fast RAM (critical for boot)
//
// - Allocate 8MB
// - Map to 0x200000–0x9FFFFF
//
// In memory_map:
//
//   if (addr >= 0x200000 && addr <= 0x9FFFFF)
//       return MEM_REGION_FAST;
//
//
//
// 5.2 Overlay behavior (Amiga rule)
//
//   Reads at 0x000000:
//
//     if overlay = ON  → ROM
//     if overlay = OFF → Chip RAM
//
//   Writes:
//
//     ALWAYS go to Chip RAM
//
//
//
// 5.3 Endianness
//
// All memory is BIG-ENDIAN:
//
//   read16 = (hi << 8) | lo
//   read32 = (b0 << 24) | ...
//
//
//
// 5.4 Access model (critical rule)
//
// CPU:
//
//   bellatrix_mem_read*
//   bellatrix_mem_write*
//
// Chipset / DMA:
//
//   bellatrix_chip_read*
//   bellatrix_chip_write*
//
//
//
// DO NOT:
//
//   - bypass memory subsystem
//   - access raw pointers directly
//
// -----------------------------------------------------------------------------
// 6. Phase 2 – AutoConfig (Future)
// -----------------------------------------------------------------------------
//
// Region:
//
//   0xE80000–0xEFFFFF
//
// Behavior:
//
//   - OS reads board descriptors
//   - A future device responds
//   - OS assigns base address
//
// Goal:
//
// Replace static Fast RAM with:
//
//   → dynamically configured Zorro devices
//
// -----------------------------------------------------------------------------
// 7. Phase 3 – Zorro III (Future)
// -----------------------------------------------------------------------------
//
// 32-bit address space:
//
//   ≥ 0x10000000
//
// Enables:
//
//   ✔ large RAM (>16MB)
//   ✔ modern AROS configs
//
// Not required for initial boot.
//
// -----------------------------------------------------------------------------
// 8. Design Principles
// -----------------------------------------------------------------------------
//
// 1. Memory is owned by MachineState
//
// 2. memory_map is the SINGLE source of truth
//
// 3. CPU never accesses raw memory directly
//
// 4. Chip RAM and Fast RAM behave identically
//    (only address differs)
//
// 5. Overlay affects READS only
//
// 6. Architecture must allow:
//
//      Chip RAM
//      Fast RAM
//      Zorro II
//      Zorro III
//      Expansion boards
//
// without redesign
//
// -----------------------------------------------------------------------------
// 9. Expected Outcome
// -----------------------------------------------------------------------------
//
// After Phase 1:
//
//   ✔ AROS detects Fast RAM
//   ✔ Memory probing succeeds
//   ✔ Boot progresses beyond current stall
//
// -----------------------------------------------------------------------------
// 10. Non-Goals (current phase)
// -----------------------------------------------------------------------------
//
// - No cycle-accurate timing
// - No AutoConfig yet
// - No slow RAM
// - No cache emulation
//
// Focus:
//
//   → Boot AROS reliably
//
// -----------------------------------------------------------------------------
// END
