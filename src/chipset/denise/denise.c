// src/chipset/denise/denise.c
//
// Denise — bitplane fetch and composite to VC4 framebuffer.
//
// ARM runs big-endian (-mbig-endian).  Chip RAM is also big-endian (M68K
// native), so uint16_t reads from chip RAM give the correct M68K word value
// directly.  The VC4 display controller expects little-endian pixels, so
// palette entries are pre-converted with LE16() at write time; the hot render
// path then writes them without any further byte manipulation.

#include "denise.h"
#include "support.h"    // LE16() — byte-swap on BE ARM, identity on LE
#include <stdint.h>

// ---------------------------------------------------------------------------
// Chip RAM virtual base (Emu68 kernel space maps physical 0 here)
// ---------------------------------------------------------------------------
#define CHIP_RAM_VIRT   0xffffff9000000000ULL
#define CHIP_RAM_MASK   0x001FFFFFUL   // 2 MB chip RAM

// ---------------------------------------------------------------------------
// Framebuffer globals — defined in emu68/src/raspi/start_rpi64.c,
// weak-declared in emu68/src/aarch64/start.c.
// Already initialised by display_logo() before bellatrix_init() runs.
// ---------------------------------------------------------------------------
extern uint16_t *framebuffer;
extern uint32_t  pitch;       // stride in bytes
extern uint32_t  fb_width;
extern uint32_t  fb_height;

// ---------------------------------------------------------------------------
// Denise state
// ---------------------------------------------------------------------------
static uint16_t s_bplcon0;          // [15]=HIRES [14:12]=nplanes [9]=HAM
static uint16_t s_bplcon1;          // horizontal scroll (unused in Phase 4)
static uint16_t s_bplcon2;          // playfield priority (unused in Phase 4)
static int16_t  s_bpl1mod;          // signed modulo, odd planes
static int16_t  s_bpl2mod;          // signed modulo, even planes

// Bitplane DMA pointers (M68K 24-bit chip RAM addresses)
static uint32_t s_bplpt[6];

// Display window / data fetch
static uint16_t s_diwstrt;   // [15:8]=vstrt  [7:0]=hstrt
static uint16_t s_diwstop;   // [15:8]=vstop  [7:0]=hstop (256-wrap in vstop)
static uint16_t s_ddfstrt;   // data-fetch start (colour clocks)
static uint16_t s_ddfstop;   // data-fetch stop

// Palette: 32 entries pre-converted to LE16 RGB565 for zero-cost render writes.
static uint16_t s_palette[32];

// ---------------------------------------------------------------------------
// Colour conversion: Amiga 12-bit 0x0RGB → LE16 RGB565
// ---------------------------------------------------------------------------
static uint16_t amiga_color_to_le16(uint16_t amiga)
{
    uint8_t r4 = (amiga >> 8) & 0xF;
    uint8_t g4 = (amiga >> 4) & 0xF;
    uint8_t b4 = (amiga >> 0) & 0xF;
    // Expand 4→8 by replicating the nibble: 0xA → 0xAA
    uint8_t r8 = (r4 << 4) | r4;
    uint8_t g8 = (g4 << 4) | g4;
    uint8_t b8 = (b4 << 4) | b4;
    // Pack to RGB565: R[15:11] G[10:5] B[4:0]
    uint16_t rgb565 = (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
    // Convert to LE16 so the render loop can write without further swapping.
    return LE16(rgb565);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void denise_init(void)
{
    s_bplcon0 = 0;
    s_bplcon1 = 0;
    s_bplcon2 = 0;
    s_bpl1mod = 0;
    s_bpl2mod = 0;
    for (int i = 0; i < 6; i++) s_bplpt[i] = 0;
    // PAL defaults: line 44–300, fetch 320 low-res pixels
    s_diwstrt = 0x2C81;
    s_diwstop = 0xF4C1;
    s_ddfstrt = 0x0038;
    s_ddfstop = 0x00D0;
    for (int i = 0; i < 32; i++) s_palette[i] = 0;
}

// ---------------------------------------------------------------------------
// Register write
// ---------------------------------------------------------------------------
void denise_write(uint32_t addr, uint32_t value)
{
    uint16_t v = (uint16_t)value;

    // Colour registers: 32 × 2-byte slots starting at 0xDFF180
    if (addr >= DENISE_COLOR00 && addr <= DENISE_COLOR31) {
        int idx = (int)((addr - DENISE_COLOR00) >> 1);
        s_palette[idx] = amiga_color_to_le16(v);
        return;
    }

    switch (addr) {
    // Bitplane control
    case DENISE_BPLCON0: s_bplcon0 = v; break;
    case DENISE_BPLCON1: s_bplcon1 = v; break;
    case DENISE_BPLCON2: s_bplcon2 = v; break;
    case DENISE_BPL1MOD: s_bpl1mod = (int16_t)v; break;
    case DENISE_BPL2MOD: s_bpl2mod = (int16_t)v; break;

    // Display window / data fetch
    case DENISE_DIWSTRT: s_diwstrt = v; break;
    case DENISE_DIWSTOP: s_diwstop = v; break;
    case DENISE_DDFSTRT: s_ddfstrt = v; break;
    case DENISE_DDFSTOP: s_ddfstop = v; break;

    // Bitplane pointer high words [20:16]
    case AGNUS_BPL1PTH: s_bplpt[0] = (s_bplpt[0] & 0x0000FFFFU) | ((uint32_t)(v & 0x001F) << 16); break;
    case AGNUS_BPL2PTH: s_bplpt[1] = (s_bplpt[1] & 0x0000FFFFU) | ((uint32_t)(v & 0x001F) << 16); break;
    case AGNUS_BPL3PTH: s_bplpt[2] = (s_bplpt[2] & 0x0000FFFFU) | ((uint32_t)(v & 0x001F) << 16); break;
    case AGNUS_BPL4PTH: s_bplpt[3] = (s_bplpt[3] & 0x0000FFFFU) | ((uint32_t)(v & 0x001F) << 16); break;
    case AGNUS_BPL5PTH: s_bplpt[4] = (s_bplpt[4] & 0x0000FFFFU) | ((uint32_t)(v & 0x001F) << 16); break;
    case AGNUS_BPL6PTH: s_bplpt[5] = (s_bplpt[5] & 0x0000FFFFU) | ((uint32_t)(v & 0x001F) << 16); break;

    // Bitplane pointer low words [15:1] (bit 0 always 0 — word-aligned)
    case AGNUS_BPL1PTL: s_bplpt[0] = (s_bplpt[0] & 0x001F0000U) | (v & 0xFFFEU); break;
    case AGNUS_BPL2PTL: s_bplpt[1] = (s_bplpt[1] & 0x001F0000U) | (v & 0xFFFEU); break;
    case AGNUS_BPL3PTL: s_bplpt[2] = (s_bplpt[2] & 0x001F0000U) | (v & 0xFFFEU); break;
    case AGNUS_BPL4PTL: s_bplpt[3] = (s_bplpt[3] & 0x001F0000U) | (v & 0xFFFEU); break;
    case AGNUS_BPL5PTL: s_bplpt[4] = (s_bplpt[4] & 0x001F0000U) | (v & 0xFFFEU); break;
    case AGNUS_BPL6PTL: s_bplpt[5] = (s_bplpt[5] & 0x001F0000U) | (v & 0xFFFEU); break;

    default: break;
    }
}

// ---------------------------------------------------------------------------
// Register read
// ---------------------------------------------------------------------------
uint32_t denise_read(uint32_t addr)
{
    if (addr >= DENISE_COLOR00 && addr <= DENISE_COLOR31)
        return 0; // colour registers are write-only on real hardware
    return 0;
}

// ---------------------------------------------------------------------------
// Frame render
//
// Reads bitplane data directly from chip RAM and composites into the VC4
// framebuffer at 2× scale (each Amiga pixel → 2×2 framebuffer pixels),
// centred on screen.
// ---------------------------------------------------------------------------

// Read one chip-RAM word.  ARM is big-endian, chip RAM is big-endian (M68K
// native): no byte-swap needed.
static inline uint16_t chip_read16(uint32_t addr)
{
    return *(const volatile uint16_t *)(CHIP_RAM_VIRT + (addr & CHIP_RAM_MASK));
}

void denise_render_frame(void)
{
    if (!framebuffer || !pitch) return;

    // Number of bitplanes from BPLCON0 [14:12]
    int nplanes = (s_bplcon0 >> 12) & 7;
    if (nplanes > 6) nplanes = 6;
    if (nplanes == 0) return;

    // Hi-res flag: BPLCON0 bit 15 → pixels are half as wide
    int hires = (s_bplcon0 >> 15) & 1;

    // Display window vertical extent from DIWSTRT/DIWSTOP
    int vstrt = (s_diwstrt >> 8) & 0xFF;
    int vstop = (s_diwstop >> 8) & 0xFF;
    // vstop wraps: PAL display extends past line 255
    if (vstop <= vstrt) vstop += 256;
    int vheight = vstop - vstrt;
    if (vheight <= 0 || vheight > 512) return;

    // Words per line: derive from DDFSTRT/DDFSTOP.
    // Each fetch unit = 1 word = 16 lores pixels (8 hires pixels).
    // Formula: nwords = (DDFSTOP - DDFSTRT) / 8 + 2, clamped to sane range.
    int ddf_words = ((int)(s_ddfstop & 0xFE) - (int)(s_ddfstrt & 0xFC)) / 8 + 2;
    if (ddf_words < 1)  ddf_words = 20;  // fall back to 320 lores pixels
    if (ddf_words > 80) ddf_words = 80;
    int pix_per_line = ddf_words * 16;   // lores pixel count

    // Scale factor: 2× for both axes makes 320×256 → 640×512
    int scale = hires ? 1 : 2;

    int out_w = pix_per_line * scale;
    int out_h = vheight     * scale;

    // Centre in framebuffer (clamp if framebuffer is smaller than output)
    uint32_t fb_x0 = ((uint32_t)out_w < fb_width)  ? (fb_width  - (uint32_t)out_w) / 2 : 0;
    uint32_t fb_y0 = ((uint32_t)out_h < fb_height) ? (fb_height - (uint32_t)out_h) / 2 : 0;

    // Snapshot bitplane pointers — we advance our own copies each line
    uint32_t bpt[6];
    for (int p = 0; p < nplanes; p++)
        bpt[p] = s_bplpt[p] & CHIP_RAM_MASK;

    for (int line = 0; line < vheight; line++) {

        // Fetch one row of words from each bitplane
        uint16_t plane_row[6][80];  // max 80 words per line
        for (int p = 0; p < nplanes; p++) {
            for (int w = 0; w < ddf_words; w++)
                plane_row[p][w] = chip_read16(bpt[p] + (uint32_t)(w * 2));
        }

        // Write scale rows to framebuffer
        for (int sy = 0; sy < scale; sy++) {
            uint32_t fb_y = fb_y0 + (uint32_t)(line * scale + sy);
            if (fb_y >= fb_height) break;
            uint16_t *row = (uint16_t *)((uintptr_t)framebuffer + fb_y * pitch);

            for (int w = 0; w < ddf_words; w++) {
                uint16_t pdata[6];
                for (int p = 0; p < nplanes; p++)
                    pdata[p] = plane_row[p][w];

                // 16 pixels per word; bit 15 = leftmost pixel
                for (int b = 15; b >= 0; b--) {
                    int cidx = 0;
                    for (int p = 0; p < nplanes; p++)
                        cidx |= (((pdata[p] >> b) & 1) << p);

                    uint16_t pixel = s_palette[cidx & 31];

                    uint32_t fb_x = fb_x0 + (uint32_t)((w * 16 + (15 - b)) * scale);
                    for (int sx = 0; sx < scale; sx++) {
                        if (fb_x + (uint32_t)sx < fb_width)
                            row[fb_x + (uint32_t)sx] = pixel;
                    }
                }
            }
        }

        // Advance bitplane pointers past the fetched data, then add modulo.
        // Odd planes (1,3,5 → index 0,2,4) use BPL1MOD; even use BPL2MOD.
        for (int p = 0; p < nplanes; p++) {
            bpt[p] = (bpt[p] + (uint32_t)(ddf_words * 2)) & CHIP_RAM_MASK;
            int16_t mod = (p & 1) ? s_bpl2mod : s_bpl1mod;
            bpt[p] = (uint32_t)((int32_t)bpt[p] + (int32_t)mod) & CHIP_RAM_MASK;
        }
    }
}
