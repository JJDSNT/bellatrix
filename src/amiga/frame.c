/*
 * Publishing the Denise frame to the guest.
 *
 * Rigel renders into a buffer of its own, in Emu68's heap, valid only until
 * the next rigel_step and deliberately outside the guest's address range. A
 * consumer needs the opposite of all three: a stable address, a stable
 * lifetime, and reachability from the m68k. So one copy per finished frame,
 * into an aperture the machine map owns.
 *
 * The copy is not a compromise to be optimised away later. It is what removes
 * the coherency question for whoever reads it -- see
 * AI_context/consolidated/vc4_memory_coherency_upstream.md -- and Rigel gives
 * no way to say where it should render, so there is nothing to be zero-copy
 * about yet.
 */

#include "amiga/frame.h"
#include "machine/region.h"

#include "A64.h"
#include "tlsf.h"

extern void *tlsf;

static uint8_t *frame_buffer;
static uint32_t frame_pitch;
static uint32_t frame_width;
static uint32_t frame_height;
static uint32_t frame_flags;
static uint32_t frame_count;
static uint8_t  publish_reported;

static uint32_t amiga_frame_reg(uint32_t offset)
{
    switch (offset)
    {
        case AMIGA_FRAME_REG_MAGIC:   return AMIGA_FRAME_MAGIC;
        case AMIGA_FRAME_REG_VERSION: return AMIGA_FRAME_VERSION;
        case AMIGA_FRAME_REG_BASE:    return AMIGA_FRAME_BASE;
        case AMIGA_FRAME_REG_PITCH:   return frame_pitch;
        case AMIGA_FRAME_REG_WIDTH:   return frame_width;
        case AMIGA_FRAME_REG_HEIGHT:  return frame_height;
        case AMIGA_FRAME_REG_FLAGS:   return frame_flags;
        case AMIGA_FRAME_REG_COUNT:   return frame_count;
        default:                      return 0;
    }
}

/*
 * Serve any access width from the 32-bit register value, so a guest may read
 * the descriptor a word at a time without the reader and the writer having to
 * agree on anything but the byte offset. The guest is big-endian and so is
 * this build, so the high half of a longword lives at the lower offset.
 */
static uint32_t amiga_frame_read(const MachineRegion *region, uint32_t address,
                                 int size)
{
    uint32_t offset = (address - AMIGA_FRAME_DESC_BASE) & 0xfffu;
    uint32_t value = amiga_frame_reg(offset & ~3u);
    uint32_t shift = (3u - (offset & 3u)) * 8u;

    (void)region;
    switch (size)
    {
        case 1:  return (value >> shift) & 0xffu;
        case 2:  return (value >> ((offset & 2u) ? 0u : 16u)) & 0xffffu;
        default: return value;
    }
}

static void amiga_frame_write(const MachineRegion *region, uint32_t address,
                              int size, uint32_t value)
{
    /* The descriptor is what the machine says about itself. Nothing to set. */
    (void)region; (void)address; (void)size; (void)value;
}

static const MachineRegionOps amiga_frame_ops =
{
    .read = amiga_frame_read,
    .write = amiga_frame_write,
};

void amiga_frame_init(void)
{
    MachineRegion region;

    /*
     * Page-aligned because machine_region_install() refuses anything else, and
     * it refuses it precisely so a region cannot half-cover a page the MMU has
     * to program as one thing.
     */
    frame_buffer = (uint8_t *)tlsf_malloc_aligned(tlsf, AMIGA_FRAME_SIZE, 4096);
    if (frame_buffer == 0)
    {
        kprintf("[BELLATRIX:RIGEL:FRAME] no memory for the frame aperture\n");
        return;
    }

    region.base      = AMIGA_FRAME_BASE;
    region.size      = AMIGA_FRAME_SIZE;
    region.kind      = MACHINE_REGION_DIRECT;
    region.name      = "Denise frame aperture";
    region.host_phys = (uintptr_t)frame_buffer;
    region.attr      = MMU_ATTR_CACHED;
    region.ops       = 0;
    region.owner     = 0;
    if (machine_region_install(&region) != 0)
        return;

    region.base      = AMIGA_FRAME_DESC_BASE;
    region.size      = AMIGA_FRAME_DESC_SIZE;
    region.kind      = MACHINE_REGION_EXTERNAL;
    region.name      = "Denise frame descriptor";
    region.host_phys = 0;
    region.attr      = 0;
    region.ops       = &amiga_frame_ops;
    region.owner     = 0;
    machine_region_install(&region);
}

void amiga_frame_publish(RigelContext *ctx)
{
    rigel_frame_t frame;
    uint32_t rows;
    uint32_t y;

    if (frame_buffer == 0 || ctx == 0)
        return;
    if (!rigel_get_frame(ctx, &frame))
        return;
    if (frame.pixels == 0 || frame.width == 0 || frame.height == 0)
        return;
    if (frame.format != RIGEL_PIXEL_RGBA8888)
        return;

    /*
     * Copy what fits and say what was copied, rather than refusing a frame
     * whose geometry grew. A consumer reading height from the descriptor draws
     * a short image; one that assumed a size would draw someone else's memory.
     */
    rows = frame.height;
    if (frame.pitch != 0 && rows > AMIGA_FRAME_SIZE / frame.pitch)
        rows = AMIGA_FRAME_SIZE / frame.pitch;

    for (y = 0; y < rows; ++y)
    {
        const uint8_t *src = (const uint8_t *)frame.pixels +
                             (size_t)y * frame.pitch;
        uint8_t *dst = frame_buffer + (size_t)y * frame.pitch;
        uint32_t n = frame.pitch;

        while (n >= 4u)
        {
            *(uint32_t *)dst = *(const uint32_t *)src;
            dst += 4; src += 4; n -= 4u;
        }
    }

    frame_pitch  = frame.pitch;
    frame_width  = frame.width;
    frame_height = rows;
    frame_count  = (uint32_t)frame.frame_count;
    frame_flags  = AMIGA_FRAME_FLAG_VALID;

    if (!publish_reported)
    {
        publish_reported = 1;
        kprintf("[BELLATRIX:RIGEL:FRAME] publishing %ux%u pitch=%u at $%08x, "
                "descriptor at $%08x\n",
            (unsigned)frame_width, (unsigned)frame_height,
            (unsigned)frame_pitch, (unsigned)AMIGA_FRAME_BASE,
            (unsigned)AMIGA_FRAME_DESC_BASE);
    }
}
