// src/chipset/agnus/copper.c
//
// Copper co-processor — batch execution at VBL time.
//
// Model (Phase 4, simplified):
//   - At each VBL the PC is reloaded from COP1LC.
//   - MOVE instructions are dispatched immediately to agnus_write().
//   - WAIT instructions are skipped through (all MOVEs execute regardless of
//     beam position), except the end-of-list sentinel $FFFF/$FFFE which stops
//     execution.
//   - SKIP instructions are ignored.
//
// This gives the correct final state of all palette and bitplane-pointer
// registers before denise_render_frame() is called — which is all that
// matters for a static-frame render without mid-scanline colour effects.
//
// Future: for per-scanline copper effects, copper_vbl_execute() can be
// replaced by a scanline-driven scheduler.

#include "copper.h"
#include "chipset/agnus/agnus.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Chip RAM access
// ---------------------------------------------------------------------------

#define CHIP_RAM_VIRT  0xffffff9000000000ULL
#define CHIP_RAM_MASK  0x001FFFFFUL   // 2 MB

static inline uint16_t chip_read16(uint32_t addr)
{
    return *(const volatile uint16_t *)(CHIP_RAM_VIRT + (addr & CHIP_RAM_MASK));
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static uint32_t s_cop1lc = 0;
static uint32_t s_cop2lc = 0;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

void copper_init(void)
{
    s_cop1lc = 0;
    s_cop2lc = 0;
}

void copper_write(uint32_t addr, uint16_t value)
{
    switch (addr) {
    case COPPER_COP1LCH:
        s_cop1lc = (s_cop1lc & 0x0000FFFFU) | ((uint32_t)(value & 0x001F) << 16);
        break;
    case COPPER_COP1LCL:
        s_cop1lc = (s_cop1lc & 0x001F0000U) | (uint32_t)(value & 0xFFFEU);
        break;
    case COPPER_COP2LCH:
        s_cop2lc = (s_cop2lc & 0x0000FFFFU) | ((uint32_t)(value & 0x001F) << 16);
        break;
    case COPPER_COP2LCL:
        s_cop2lc = (s_cop2lc & 0x001F0000U) | (uint32_t)(value & 0xFFFEU);
        break;
    case COPPER_COPJMP1:
    case COPPER_COPJMP2:
    case COPPER_COPINS:
        // Strobes and direct writes: no persistent state needed here.
        break;
    default:
        break;
    }
}

uint32_t copper_read(uint32_t addr)
{
    (void)addr;
    return 0; // copper registers are write-only on real hardware
}

void copper_vbl_execute(void)
{
    // Reload from COP1LC at every VBL — standard Amiga behaviour.
    uint32_t pc = s_cop1lc;
    if (!pc) return;

    // Safety limit: prevent infinite loops from corrupt copper lists.
    int limit = 8192;

    pc &= CHIP_RAM_MASK & ~1U; // word-aligned, chip RAM bounds

    while (limit-- > 0) {
        if ((pc + 3) > CHIP_RAM_MASK) break;

        uint16_t ir1 = chip_read16(pc);
        uint16_t ir2 = chip_read16(pc + 2);
        pc += 4;

        if (!(ir1 & 1)) {
            // MOVE: destination register = $DFF000 | (ir1 & $01FE)
            // Only allow addresses >= $DFF040 (copper DANGER disabled).
            uint32_t reg = 0xDFF000U | (uint32_t)(ir1 & 0x01FEU);
            if ((ir1 & 0x01FEU) >= 0x0040U)
                agnus_write(reg, ir2, 2);
        } else {
            // WAIT or SKIP: stop only on the end-of-list sentinel.
            if (ir1 == 0xFFFFU && (ir2 & 0xFFFEU) == 0xFFFEU) break;
            // Other WAITs/SKIPs: execute through.
        }
    }
}
