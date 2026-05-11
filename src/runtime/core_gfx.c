#include "runtime/core_gfx.h"

#include <string.h>

#include "chipset/agnus/agnus.h"
#include "chipset/agnus/beam.h"
#include "chipset/agnus/copper/copper.h"
#include "chipset/agnus/blitter.h"
#include "chipset/agnus/bitplanes.h"

#include "chipset/denise/denise.h"
#include "chipset/denise/sprites.h"

static void core_gfx_step_agnus(RuntimeCoreGFX *core,
                                uint32_t cycles)
{
    /*
     * Agnus is the owner of time.
     */
    agnus_step(&core->machine->agnus, cycles);
}

static void core_gfx_step_beam(RuntimeCoreGFX *core,
                               uint32_t cycles)
{
    beam_step(&core->machine->agnus.beam,
              cycles);

    core->vpos = core->machine->agnus.beam.vpos;
    core->hpos = core->machine->agnus.beam.hpos;
}

static void core_gfx_step_copper(RuntimeCoreGFX *core,
                                 uint32_t cycles)
{
    copper_step_exec(&core->machine->agnus.copper,
                     &core->machine->agnus,
                     (int)cycles);
}

static void core_gfx_step_blitter(RuntimeCoreGFX *core,
                                  uint32_t cycles)
{
    blitter_step(&core->machine->agnus.blitter,
                 &core->machine->agnus,
                 (uint64_t)cycles);
}

static void core_gfx_step_bitplanes(RuntimeCoreGFX *core,
                                    uint32_t cycles)
{
    (void)cycles;
    bitplanes_step(&core->machine->agnus.bitplanes,
                   &core->machine->agnus);
}

static void core_gfx_step_sprites(RuntimeCoreGFX *core)
{
    /*
     * Denise consumes already-fetched sprite words.
     */
    denise_sprite_step_pixel(
        &core->machine->denise.sprites);
}

static void core_gfx_step_denise(RuntimeCoreGFX *core,
                                 uint32_t cycles)
{
    denise_step(&core->machine->denise,
                cycles);
}

static void core_gfx_handle_vblank(RuntimeCoreGFX *core)
{
    /*
     * Optional future hook:
     *
     * publish VBL event
     * wake CPU interrupt path
     * trigger frame presentation
     */

    core->frame_counter++;
}

bool core_gfx_init(RuntimeCoreGFX *core,
                   BellatrixMachine *machine)
{
    if (!core || !machine) {
        return false;
    }

    memset(core, 0, sizeof(*core));

    core->machine = machine;
    core->running = true;

    core->master_cycles = 0;

    core->vpos = 0;
    core->hpos = 0;

    core->frame_counter = 0;

    return true;
}

void core_gfx_shutdown(RuntimeCoreGFX *core)
{
    if (!core) {
        return;
    }

    core->running = false;
}

uint64_t core_gfx_master_cycles(
    const RuntimeCoreGFX *core)
{
    if (!core) {
        return 0;
    }

    return core->master_cycles;
}

void core_gfx_step(RuntimeCoreGFX *core,
                   uint32_t cycles)
{
    if (!core || !core->running) {
        return;
    }

    /*
     * Agnus advances observable system time.
     */
    core->master_cycles += cycles;

    /*
     * Beam progression.
     */
    core_gfx_step_beam(core, cycles);

    /*
     * DMA orchestration.
     */
    core_gfx_step_agnus(core, cycles);

    /*
     * Copper execution.
     */
    core_gfx_step_copper(core, cycles);

    /*
     * Blitter execution.
     */
    core_gfx_step_blitter(core, cycles);

    /*
     * Bitplane DMA/fetch.
     */
    core_gfx_step_bitplanes(core, cycles);

    /*
     * Sprite shifters/pixels.
     */
    core_gfx_step_sprites(core);

    /*
     * Denise pixel generation.
     */
    core_gfx_step_denise(core, cycles);

    /*
     * Vertical blank detection.
     */
    if (beam_vblank_entered(
            &core->machine->agnus.beam))
    {
        core_gfx_handle_vblank(core);
    }
}