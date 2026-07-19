/*
 * bellatrix.card — P96 card driver for the "bellatrix.rtg" Zorro III board.
 *
 * Consumed by the AROS m68k p96gfx HIDD (external/aros .../hidd/p96gfx),
 * which scans exec's LibList for libraries named "*.card". The board is
 * served by src/machine/expansions/rtg/rtg.c. Harness and future Raspberry
 * presenters consume the same backend-neutral scanout state. Register spec:
 * docs/rtg_design.md — keep in sync with src/machine/expansions/rtg/rtg.h.
 *
 * BoardInfo scaffolding follows external/VideoCore.card (MPL-2.0, (c) Michal
 * Schulz / Emu68 project); framebuffer behavior is deliberately backend-neutral.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <libraries/configvars.h>
#include <stddef.h>

#include "boardinfo.h"

#define CARD_NAME    "bellatrix.card"
#define CARD_VERSION 1
#define CARD_REVISION 0
#define CARD_PRIORITY 0

#define RTG_MANUFACTURER 0x07DB
#define RTG_PRODUCT      0x10

/* Register offsets — mirror of src/machine/expansions/rtg/rtg.h */
#define RTG_REG_ID            0x00
#define RTG_REG_VRAM_OFF      0x08
#define RTG_REG_VRAM_SIZE     0x0C
#define RTG_REG_ENABLE        0x10
#define RTG_REG_MODE_W        0x14
#define RTG_REG_MODE_H        0x18
#define RTG_REG_FORMAT        0x1C
#define RTG_REG_BYTES_PER_ROW 0x20
#define RTG_REG_PAN           0x24
#define RTG_REG_PAL_INDEX     0x28
#define RTG_REG_PAL_DATA      0x2C
#define RTG_REG_VBLANK        0x30
#define RTG_REG_DEBUG         0x34
#define RTG_REG_ACCEL_DST     0x38
#define RTG_REG_ACCEL_PITCH   0x3C
#define RTG_REG_ACCEL_XY      0x40
#define RTG_REG_ACCEL_WH      0x44
#define RTG_REG_ACCEL_COLOR   0x48
#define RTG_REG_ACCEL_FMTMASK 0x4C
#define RTG_REG_ACCEL_COMMAND 0x50
#define RTG_REG_ACCEL_STATUS  0x54
#define RTG_REG_ACCEL_SRC     0x58
#define RTG_REG_ACCEL_SRC_PITCH 0x5C
#define RTG_REG_ACCEL_SRC_XY  0x60
#define RTG_REG_ACCEL_OPCODE  0x64
#define RTG_REG_ACCEL_UPLOAD_RESET 0x68
#define RTG_REG_ACCEL_UPLOAD_DATA  0x6C
#define RTG_REG_ACCEL_MODE    0x70
#define RTG_REG_ACCEL_FGPEN   0x74
#define RTG_REG_ACCEL_BGPEN   0x78
#define RTG_REG_SPRITE_ENABLE 0x7C
#define RTG_REG_SPRITE_XY     0x80
#define RTG_REG_SPRITE_WH     0x84
#define RTG_REG_SPRITE_COLOR_INDEX 0x88
#define RTG_REG_SPRITE_COLOR_DATA  0x8C
#define RTG_REG_SPRITE_UPLOAD_RESET 0x90
#define RTG_REG_SPRITE_UPLOAD_DATA  0x94

#define RTG_ACCEL_FILLRECT 1
#define RTG_ACCEL_BLIT_COPY 2
#define RTG_ACCEL_INVERTRECT 3
#define RTG_ACCEL_BLITTEMPLATE 4
#define RTG_ACCEL_BLITPATTERN 5
#define RTG_ACCEL_DRAWLINE 6
#define RTG_ACCEL_PLANAR2CHUNKY 7
#define RTG_ACCEL_PLANAR2DIRECT 8

#define RTG_ID_MAGIC 0x42525447

/* FORMAT register values (RGBFTYPE subset) */
#define RTG_FMT_CLUT      1
#define RTG_FMT_A8R8G8B8  6
#define RTG_FMT_R5G6B5    10

struct BellatrixCardBase {
    struct Library      cb_Lib;
    struct ExecBase    *cb_SysBase;
    volatile ULONG     *cb_Regs;     /* board base                      */
    UBYTE              *cb_VRAM;     /* board base + VRAM_OFF           */
    ULONG               cb_VRAMSize;
    UWORD               cb_Width;    /* latched by SetGC for SetPanning */
    UWORD               cb_Height;
    ULONG               cb_ProbeCount[7];
};

/* AROS p96gfx accesses BoardInfo through fixed byte offsets rather than this C
 * declaration. Keep a compile-time m68k ABI oracle beside the driver so a
 * header/toolchain change cannot silently move the fields we populate. */
#define P96_ABI_ASSERT(field, expected) \
    _Static_assert(offsetof(struct BoardInfo, field) == (expected), \
                   "P96 BoardInfo offset mismatch: " #field)
P96_ABI_ASSERT(RegisterBase,          0);
P96_ABI_ASSERT(MemoryBase,            4);
P96_ABI_ASSERT(MemorySize,           12);
P96_ABI_ASSERT(BoardName,            16);
P96_ABI_ASSERT(CardBase,             52);
P96_ABI_ASSERT(ResolutionsList,     158);
P96_ABI_ASSERT(BoardType,           170);
P96_ABI_ASSERT(Flags,               186);
P96_ABI_ASSERT(RGBFormats,          200);
P96_ABI_ASSERT(PixelClockCount,     254);
P96_ABI_ASSERT(SetSwitch,           282);
P96_ABI_ASSERT(SetGC,               294);
P96_ABI_ASSERT(SetPanning,          298);
P96_ABI_ASSERT(CalculateBytesPerRow,302);
P96_ABI_ASSERT(WaitVerticalSync,    346);
#undef P96_ABI_ASSERT

#define SysBase (base->cb_SysBase)

static inline void reg_write(struct BellatrixCardBase *base, ULONG off, ULONG val)
{
    base->cb_Regs[off / 4] = val;
}

static inline ULONG reg_read(struct BellatrixCardBase *base, ULONG off)
{
    return base->cb_Regs[off / 4];
}

enum {
    PROBE_FILLRECT = 1,
    PROBE_INVERTRECT,
    PROBE_BLITRECT,
    PROBE_BLITTEMPLATE,
    PROBE_BLITPATTERN,
    PROBE_DRAWLINE,
    PROBE_BLITCOMPLETE
};

/* Telemetry-only callbacks. Report the first and power-of-two calls, then
 * invoke AROS' Default entry so AROSFlag is cleared and the existing software
 * fallback remains authoritative. */
static void probe_report(struct BellatrixCardBase *base, UWORD op,
                         UWORD width, UWORD height, ULONG detail)
{
    ULONG count = ++base->cb_ProbeCount[op - 1];
    if (count != 1 && (count & (count - 1)) != 0)
        return;
    reg_write(base, RTG_REG_DEBUG,
              0xB7000000UL | ((ULONG)op << 20) | (count & 0x0fffff));
    reg_write(base, RTG_REG_DEBUG,
              0xB8000000UL | (((ULONG)width & 0x0fff) << 12) |
              ((ULONG)height & 0x0fff));
    reg_write(base, RTG_REG_DEBUG, 0xB9000000UL | (detail & 0x00ffffff));
}

static ULONG rtg_format(RGBFTYPE fmt);

static void AccelPlanar2Chunky(__REGA0(struct BoardInfo *bi),
                               __REGA1(struct BitMap *bm),
                               __REGA2(struct RenderInfo *ri),
                               __REGD0(SHORT sx), __REGD1(SHORT sy),
                               __REGD2(SHORT dx), __REGD3(SHORT dy),
                               __REGD4(SHORT w), __REGD5(SHORT h),
                               __REGD6(UBYTE plane_mask),
                               __REGD7(UBYTE minterm))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    ULONG off, upload_pitch, total, plane, row, byte;
    UBYTE *src;
    if (minterm == 0xc0 && sx >= 0 && sy >= 0 && dx >= 0 && dy >= 0 &&
        w > 0 && h > 0 && bm->Depth > 0 && bm->Depth <= 8 &&
        (ULONG)(UWORD)sy + (UWORD)h <= bm->Rows && ri->BytesPerRow > 0 &&
        (UBYTE *)ri->Memory >= base->cb_VRAM &&
        (UBYTE *)ri->Memory < base->cb_VRAM + base->cb_VRAMSize) {
        upload_pitch = (((ULONG)(UWORD)sx & 7) + (UWORD)w + 7) >> 3;
        if (((ULONG)(UWORD)sx >> 3) + upload_pitch <= bm->BytesPerRow) {
            total = upload_pitch * (ULONG)(UWORD)h * bm->Depth;
            if (total <= 65536UL) {
                off = (ULONG)((UBYTE *)ri->Memory - base->cb_VRAM);
                reg_write(base, RTG_REG_ACCEL_UPLOAD_RESET, 0);
                for (plane = 0; plane < bm->Depth; ++plane) {
                    src = (UBYTE *)bm->Planes[plane];
                    for (row = 0; row < (ULONG)(UWORD)h; ++row) {
                        for (byte = 0; byte < upload_pitch; ++byte) {
                            ULONG value = 0;
                            if (src == (UBYTE *)-1) value = 0xff;
                            else if (src) value = src[((ULONG)(UWORD)sy + row) *
                                bm->BytesPerRow + ((ULONG)(UWORD)sx >> 3) + byte];
                            reg_write(base, RTG_REG_ACCEL_UPLOAD_DATA, value);
                        }
                    }
                }
                reg_write(base, RTG_REG_ACCEL_DST, off);
                reg_write(base, RTG_REG_ACCEL_PITCH, (UWORD)ri->BytesPerRow);
                reg_write(base, RTG_REG_ACCEL_XY,
                          ((ULONG)(UWORD)dx << 16) | (UWORD)dy);
                reg_write(base, RTG_REG_ACCEL_WH,
                          ((ULONG)(UWORD)w << 16) | (UWORD)h);
                reg_write(base, RTG_REG_ACCEL_SRC_PITCH, upload_pitch);
                reg_write(base, RTG_REG_ACCEL_SRC_XY, (UWORD)sx & 7);
                reg_write(base, RTG_REG_ACCEL_MODE, bm->Depth);
                reg_write(base, RTG_REG_ACCEL_FMTMASK, plane_mask);
                reg_write(base, RTG_REG_ACCEL_OPCODE, minterm);
                reg_write(base, RTG_REG_ACCEL_COMMAND,
                          RTG_ACCEL_PLANAR2CHUNKY);
                if (reg_read(base, RTG_REG_ACCEL_STATUS) == 1)
                    return;
            }
        }
    }
    bi->BlitPlanar2ChunkyDefault(bi, bm, ri, sx, sy, dx, dy, w, h,
                                 plane_mask, minterm);
}

static void AccelPlanar2Direct(__REGA0(struct BoardInfo *bi),
                               __REGA1(struct BitMap *bm),
                               __REGA2(struct RenderInfo *ri),
                               __REGA3(struct ColorIndexMapping *cim),
                               __REGD0(SHORT sx), __REGD1(SHORT sy),
                               __REGD2(SHORT dx), __REGD3(SHORT dy),
                               __REGD4(SHORT w), __REGD5(SHORT h),
                               __REGD6(UBYTE minterm),
                               __REGD7(UBYTE plane_mask))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    ULONG off, hostfmt, upload_pitch, total, colors, plane, row, byte, i, value;
    UBYTE *src;
    hostfmt = rtg_format(ri->RGBFormat);
    if (minterm == 0x0c && (hostfmt == RTG_FMT_R5G6B5 ||
        hostfmt == RTG_FMT_A8R8G8B8) && sx >= 0 && sy >= 0 && dx >= 0 &&
        dy >= 0 && w > 0 && h > 0 && bm->Depth > 0 && bm->Depth <= 8 &&
        (ULONG)(UWORD)sy + (UWORD)h <= bm->Rows && ri->BytesPerRow > 0 &&
        (UBYTE *)ri->Memory >= base->cb_VRAM &&
        (UBYTE *)ri->Memory < base->cb_VRAM + base->cb_VRAMSize) {
        upload_pitch = (((ULONG)(UWORD)sx & 7) + (UWORD)w + 7) >> 3;
        colors = 1UL << bm->Depth;
        total = upload_pitch * (ULONG)(UWORD)h * bm->Depth + colors * 4;
        if (((ULONG)(UWORD)sx >> 3) + upload_pitch <= bm->BytesPerRow &&
            total <= 65536UL) {
            off = (ULONG)((UBYTE *)ri->Memory - base->cb_VRAM);
            reg_write(base, RTG_REG_ACCEL_UPLOAD_RESET, 0);
            for (plane = 0; plane < bm->Depth; ++plane) {
                src = (UBYTE *)bm->Planes[plane];
                for (row = 0; row < (ULONG)(UWORD)h; ++row)
                    for (byte = 0; byte < upload_pitch; ++byte) {
                        value = 0;
                        if (src == (UBYTE *)-1) value = 0xff;
                        else if (src) value = src[((ULONG)(UWORD)sy + row) *
                            bm->BytesPerRow + ((ULONG)(UWORD)sx >> 3) + byte];
                        reg_write(base, RTG_REG_ACCEL_UPLOAD_DATA, value);
                    }
            }
            for (i = 0; i < colors; ++i) {
                value = cim->Colors[i];
                reg_write(base, RTG_REG_ACCEL_UPLOAD_DATA, value >> 24);
                reg_write(base, RTG_REG_ACCEL_UPLOAD_DATA, value >> 16);
                reg_write(base, RTG_REG_ACCEL_UPLOAD_DATA, value >> 8);
                reg_write(base, RTG_REG_ACCEL_UPLOAD_DATA, value);
            }
            reg_write(base, RTG_REG_ACCEL_DST, off);
            reg_write(base, RTG_REG_ACCEL_PITCH, (UWORD)ri->BytesPerRow);
            reg_write(base, RTG_REG_ACCEL_XY,
                      ((ULONG)(UWORD)dx << 16) | (UWORD)dy);
            reg_write(base, RTG_REG_ACCEL_WH,
                      ((ULONG)(UWORD)w << 16) | (UWORD)h);
            reg_write(base, RTG_REG_ACCEL_SRC_PITCH, upload_pitch);
            reg_write(base, RTG_REG_ACCEL_SRC_XY, (UWORD)sx & 7);
            reg_write(base, RTG_REG_ACCEL_MODE, bm->Depth);
            reg_write(base, RTG_REG_ACCEL_FMTMASK,
                      (hostfmt << 8) | plane_mask);
            reg_write(base, RTG_REG_ACCEL_COLOR, cim->ColorMask);
            reg_write(base, RTG_REG_ACCEL_OPCODE, minterm);
            reg_write(base, RTG_REG_ACCEL_COMMAND, RTG_ACCEL_PLANAR2DIRECT);
            if (reg_read(base, RTG_REG_ACCEL_STATUS) == 1)
                return;
        }
    }
    bi->BlitPlanar2DirectDefault(bi, bm, ri, cim, sx, sy, dx, dy, w, h,
                                 minterm, plane_mask);
}

static void ProbeFillRect(__REGA0(struct BoardInfo *bi),
                          __REGA1(struct RenderInfo *ri),
                          __REGD0(WORD x), __REGD1(WORD y),
                          __REGD2(WORD w), __REGD3(WORD h),
                          __REGD4(ULONG pen), __REGD5(UBYTE mask),
                          __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    ULONG dst, hostfmt;
    probe_report(base, PROBE_FILLRECT, w, h,
                 ((ULONG)fmt << 8) | mask);
    hostfmt = rtg_format(fmt);
    if (hostfmt != 0 && mask == 0xff && x >= 0 && y >= 0 &&
        w > 0 && h > 0 && ri->BytesPerRow > 0 &&
        (UBYTE *)ri->Memory >= base->cb_VRAM &&
        (UBYTE *)ri->Memory < base->cb_VRAM + base->cb_VRAMSize) {
        dst = (ULONG)((UBYTE *)ri->Memory - base->cb_VRAM);
        reg_write(base, RTG_REG_ACCEL_DST, dst);
        reg_write(base, RTG_REG_ACCEL_PITCH, (UWORD)ri->BytesPerRow);
        reg_write(base, RTG_REG_ACCEL_XY,
                  ((ULONG)(UWORD)x << 16) | (UWORD)y);
        reg_write(base, RTG_REG_ACCEL_WH,
                  ((ULONG)(UWORD)w << 16) | (UWORD)h);
        reg_write(base, RTG_REG_ACCEL_COLOR, pen);
        reg_write(base, RTG_REG_ACCEL_FMTMASK,
                  (hostfmt << 8) | mask);
        reg_write(base, RTG_REG_ACCEL_COMMAND, RTG_ACCEL_FILLRECT);
        if (reg_read(base, RTG_REG_ACCEL_STATUS) == 1)
            return;
    }
    bi->FillRectDefault(bi, ri, x, y, w, h, pen, mask, fmt);
}

static void ProbeInvertRect(__REGA0(struct BoardInfo *bi),
                            __REGA1(struct RenderInfo *ri),
                            __REGD0(WORD x), __REGD1(WORD y),
                            __REGD2(WORD w), __REGD3(WORD h),
                            __REGD4(UBYTE mask), __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    ULONG off, hostfmt;
    probe_report(base, PROBE_INVERTRECT, w, h,
                 ((ULONG)fmt << 8) | mask);
    hostfmt = rtg_format(fmt);
    if (mask == 0xff && hostfmt && x >= 0 && y >= 0 && w > 0 && h > 0 &&
        ri->BytesPerRow > 0 && (UBYTE *)ri->Memory >= base->cb_VRAM &&
        (UBYTE *)ri->Memory < base->cb_VRAM + base->cb_VRAMSize) {
        off = (ULONG)((UBYTE *)ri->Memory - base->cb_VRAM);
        reg_write(base, RTG_REG_ACCEL_DST, off);
        reg_write(base, RTG_REG_ACCEL_PITCH, (UWORD)ri->BytesPerRow);
        reg_write(base, RTG_REG_ACCEL_XY, ((ULONG)(UWORD)x << 16) | (UWORD)y);
        reg_write(base, RTG_REG_ACCEL_WH, ((ULONG)(UWORD)w << 16) | (UWORD)h);
        reg_write(base, RTG_REG_ACCEL_FMTMASK, (hostfmt << 8) | mask);
        reg_write(base, RTG_REG_ACCEL_COMMAND, RTG_ACCEL_INVERTRECT);
        if (reg_read(base, RTG_REG_ACCEL_STATUS) == 1)
            return;
    }
    bi->InvertRectDefault(bi, ri, x, y, w, h, mask, fmt);
}

static void ProbeBlitRect(__REGA0(struct BoardInfo *bi),
                          __REGA1(struct RenderInfo *ri),
                          __REGD0(WORD sx), __REGD1(WORD sy),
                          __REGD2(WORD dx), __REGD3(WORD dy),
                          __REGD4(WORD w), __REGD5(WORD h),
                          __REGD6(UBYTE mask), __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    ULONG off, hostfmt;
    probe_report(base, PROBE_BLITRECT, w, h,
                 ((ULONG)fmt << 8) | mask);
    hostfmt = rtg_format(fmt);
    if (mask == 0xff && hostfmt != 0 && sx >= 0 && sy >= 0 &&
        dx >= 0 && dy >= 0 && w > 0 && h > 0 && ri->BytesPerRow > 0 &&
        (UBYTE *)ri->Memory >= base->cb_VRAM &&
        (UBYTE *)ri->Memory < base->cb_VRAM + base->cb_VRAMSize) {
        off = (ULONG)((UBYTE *)ri->Memory - base->cb_VRAM);
        reg_write(base, RTG_REG_ACCEL_SRC, off);
        reg_write(base, RTG_REG_ACCEL_SRC_PITCH, (UWORD)ri->BytesPerRow);
        reg_write(base, RTG_REG_ACCEL_SRC_XY,
                  ((ULONG)(UWORD)sx << 16) | (UWORD)sy);
        reg_write(base, RTG_REG_ACCEL_DST, off);
        reg_write(base, RTG_REG_ACCEL_PITCH, (UWORD)ri->BytesPerRow);
        reg_write(base, RTG_REG_ACCEL_XY,
                  ((ULONG)(UWORD)dx << 16) | (UWORD)dy);
        reg_write(base, RTG_REG_ACCEL_WH,
                  ((ULONG)(UWORD)w << 16) | (UWORD)h);
        reg_write(base, RTG_REG_ACCEL_FMTMASK, hostfmt << 8);
        reg_write(base, RTG_REG_ACCEL_OPCODE, 0x0c);
        reg_write(base, RTG_REG_ACCEL_COMMAND, RTG_ACCEL_BLIT_COPY);
        if (reg_read(base, RTG_REG_ACCEL_STATUS) == 1)
            return;
    }
    bi->BlitRectDefault(bi, ri, sx, sy, dx, dy, w, h, mask, fmt);
}

static void ProbeBlitTemplate(__REGA0(struct BoardInfo *bi),
                              __REGA1(struct RenderInfo *ri),
                              __REGA2(struct Template *tmpl),
                              __REGD0(WORD x), __REGD1(WORD y),
                              __REGD2(WORD w), __REGD3(WORD h),
                              __REGD4(UBYTE mask), __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    ULONG off, hostfmt, upload_pitch, row, byte;
    UBYTE *src;
    probe_report(base, PROBE_BLITTEMPLATE, w, h,
                 ((ULONG)fmt << 8) | mask);
    hostfmt = rtg_format(fmt);
    upload_pitch = ((ULONG)tmpl->XOffset + (ULONG)(UWORD)w + 7) >> 3;
    if (mask == 0xff && hostfmt && x >= 0 && y >= 0 && w > 0 && h > 0 &&
        tmpl->BytesPerRow > 0 && upload_pitch <= (ULONG)tmpl->BytesPerRow &&
        upload_pitch * (ULONG)(UWORD)h <= 65536UL &&
        (tmpl->DrawMode & ~5) == 0 && ri->BytesPerRow > 0 &&
        (UBYTE *)ri->Memory >= base->cb_VRAM &&
        (UBYTE *)ri->Memory < base->cb_VRAM + base->cb_VRAMSize) {
        off = (ULONG)((UBYTE *)ri->Memory - base->cb_VRAM);
        reg_write(base, RTG_REG_ACCEL_UPLOAD_RESET, 0);
        src = (UBYTE *)tmpl->Memory;
        for (row = 0; row < (ULONG)(UWORD)h; ++row)
            for (byte = 0; byte < upload_pitch; ++byte)
                reg_write(base, RTG_REG_ACCEL_UPLOAD_DATA,
                          src[row * (UWORD)tmpl->BytesPerRow + byte]);
        reg_write(base, RTG_REG_ACCEL_DST, off);
        reg_write(base, RTG_REG_ACCEL_PITCH, (UWORD)ri->BytesPerRow);
        reg_write(base, RTG_REG_ACCEL_XY, ((ULONG)(UWORD)x << 16) | (UWORD)y);
        reg_write(base, RTG_REG_ACCEL_WH, ((ULONG)(UWORD)w << 16) | (UWORD)h);
        reg_write(base, RTG_REG_ACCEL_SRC_PITCH, upload_pitch);
        reg_write(base, RTG_REG_ACCEL_SRC_XY, tmpl->XOffset);
        reg_write(base, RTG_REG_ACCEL_FMTMASK, (hostfmt << 8) | mask);
        reg_write(base, RTG_REG_ACCEL_MODE, tmpl->DrawMode);
        reg_write(base, RTG_REG_ACCEL_FGPEN, tmpl->FgPen);
        reg_write(base, RTG_REG_ACCEL_BGPEN, tmpl->BgPen);
        reg_write(base, RTG_REG_ACCEL_COMMAND, RTG_ACCEL_BLITTEMPLATE);
        if (reg_read(base, RTG_REG_ACCEL_STATUS) == 1)
            return;
    }
    bi->BlitTemplateDefault(bi, ri, tmpl, x, y, w, h, mask, fmt);
}

static void ProbeBlitPattern(__REGA0(struct BoardInfo *bi),
                             __REGA1(struct RenderInfo *ri),
                             __REGA2(struct Pattern *pat),
                             __REGD0(WORD x), __REGD1(WORD y),
                             __REGD2(WORD w), __REGD3(WORD h),
                             __REGD4(UBYTE mask), __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    ULONG off, hostfmt, rows, i;
    UBYTE *src;
    probe_report(base, PROBE_BLITPATTERN, w, h,
                 ((ULONG)fmt << 8) | mask);
    hostfmt = rtg_format(fmt);
    rows = pat->Size <= 8 ? 1UL << pat->Size : 0;
    if (mask == 0xff && hostfmt && rows && x >= 0 && y >= 0 && w > 0 && h > 0 &&
        pat->XOffset <= 15 && (pat->DrawMode & ~5) == 0 &&
        ri->BytesPerRow > 0 && (UBYTE *)ri->Memory >= base->cb_VRAM &&
        (UBYTE *)ri->Memory < base->cb_VRAM + base->cb_VRAMSize) {
        off = (ULONG)((UBYTE *)ri->Memory - base->cb_VRAM);
        src = (UBYTE *)pat->Memory;
        reg_write(base, RTG_REG_ACCEL_UPLOAD_RESET, 0);
        for (i = 0; i < rows * 2; ++i)
            reg_write(base, RTG_REG_ACCEL_UPLOAD_DATA, src[i]);
        reg_write(base, RTG_REG_ACCEL_DST, off);
        reg_write(base, RTG_REG_ACCEL_PITCH, (UWORD)ri->BytesPerRow);
        reg_write(base, RTG_REG_ACCEL_XY, ((ULONG)(UWORD)x << 16) | (UWORD)y);
        reg_write(base, RTG_REG_ACCEL_WH, ((ULONG)(UWORD)w << 16) | (UWORD)h);
        reg_write(base, RTG_REG_ACCEL_SRC_PITCH, rows);
        reg_write(base, RTG_REG_ACCEL_SRC_XY,
                  ((ULONG)pat->XOffset << 16) | pat->YOffset);
        reg_write(base, RTG_REG_ACCEL_FMTMASK, (hostfmt << 8) | mask);
        reg_write(base, RTG_REG_ACCEL_MODE, pat->DrawMode);
        reg_write(base, RTG_REG_ACCEL_FGPEN, pat->FgPen);
        reg_write(base, RTG_REG_ACCEL_BGPEN, pat->BgPen);
        reg_write(base, RTG_REG_ACCEL_COMMAND, RTG_ACCEL_BLITPATTERN);
        if (reg_read(base, RTG_REG_ACCEL_STATUS) == 1)
            return;
    }
    bi->BlitPatternDefault(bi, ri, pat, x, y, w, h, mask, fmt);
}

static void ProbeDrawLine(__REGA0(struct BoardInfo *bi),
                          __REGA1(struct RenderInfo *ri),
                          __REGA2(struct Line *line), __REGD0(UBYTE mask),
                          __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    ULONG off, hostfmt;
    probe_report(base, PROBE_DRAWLINE, 0, 0,
                 ((ULONG)fmt << 8) | mask);
    hostfmt = rtg_format(fmt);
    /* AROS initializes the endpoints, pens and LinePtrn. Other Line fields
     * are not authoritative in this path, so the first safe contract is the
     * solid (0xffff) foreground line. */
    if (hostfmt && line->LinePtrn == 0xffff && line->X >= 0 && line->Y >= 0 &&
        line->dX >= 0 && line->dY >= 0 && ri->BytesPerRow > 0 &&
        (UBYTE *)ri->Memory >= base->cb_VRAM &&
        (UBYTE *)ri->Memory < base->cb_VRAM + base->cb_VRAMSize) {
        off = (ULONG)((UBYTE *)ri->Memory - base->cb_VRAM);
        reg_write(base, RTG_REG_ACCEL_DST, off);
        reg_write(base, RTG_REG_ACCEL_PITCH, (UWORD)ri->BytesPerRow);
        reg_write(base, RTG_REG_ACCEL_XY,
                  ((ULONG)(UWORD)line->X << 16) | (UWORD)line->Y);
        reg_write(base, RTG_REG_ACCEL_WH,
                  ((ULONG)(UWORD)line->dX << 16) | (UWORD)line->dY);
        reg_write(base, RTG_REG_ACCEL_FMTMASK, hostfmt << 8);
        reg_write(base, RTG_REG_ACCEL_FGPEN, line->FgPen);
        reg_write(base, RTG_REG_ACCEL_COMMAND, RTG_ACCEL_DRAWLINE);
        if (reg_read(base, RTG_REG_ACCEL_STATUS) == 1)
            return;
    }
    bi->DrawLineDefault(bi, ri, line, mask, fmt);
}

static void ProbeBlitComplete(__REGA0(struct BoardInfo *bi),
                              __REGA1(struct RenderInfo *src),
                              __REGA2(struct RenderInfo *dst),
                              __REGD0(WORD sx), __REGD1(WORD sy),
                              __REGD2(WORD dx), __REGD3(WORD dy),
                              __REGD4(WORD w), __REGD5(WORD h),
                              __REGD6(UBYTE opcode),
                              __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    ULONG src_off, dst_off, hostfmt;
    probe_report(base, PROBE_BLITCOMPLETE, w, h,
                 ((ULONG)fmt << 8) | opcode);
    hostfmt = rtg_format(fmt);
    /* P96 minterm 0x00 is a constant-zero raster operation.  AROS uses it
     * for the initial full-screen clear, so routing it through the generic
     * 68k fallback is especially expensive.  It needs no readable source. */
    if (opcode == 0x00 && hostfmt != 0 && dx >= 0 && dy >= 0 &&
        w > 0 && h > 0 && dst->BytesPerRow > 0 &&
        (UBYTE *)dst->Memory >= base->cb_VRAM &&
        (UBYTE *)dst->Memory < base->cb_VRAM + base->cb_VRAMSize) {
        dst_off = (ULONG)((UBYTE *)dst->Memory - base->cb_VRAM);
        reg_write(base, RTG_REG_ACCEL_DST, dst_off);
        reg_write(base, RTG_REG_ACCEL_PITCH, (UWORD)dst->BytesPerRow);
        reg_write(base, RTG_REG_ACCEL_XY,
                  ((ULONG)(UWORD)dx << 16) | (UWORD)dy);
        reg_write(base, RTG_REG_ACCEL_WH,
                  ((ULONG)(UWORD)w << 16) | (UWORD)h);
        reg_write(base, RTG_REG_ACCEL_COLOR, 0);
        reg_write(base, RTG_REG_ACCEL_FMTMASK, (hostfmt << 8) | 0xffu);
        reg_write(base, RTG_REG_ACCEL_COMMAND, RTG_ACCEL_FILLRECT);
        if (reg_read(base, RTG_REG_ACCEL_STATUS) == 1)
            return;
    }
    if (opcode == 0x0c && hostfmt != 0 && sx >= 0 && sy >= 0 &&
        dx >= 0 && dy >= 0 && w > 0 && h > 0 &&
        src->BytesPerRow > 0 && dst->BytesPerRow > 0 &&
        (UBYTE *)src->Memory >= base->cb_VRAM &&
        (UBYTE *)src->Memory < base->cb_VRAM + base->cb_VRAMSize &&
        (UBYTE *)dst->Memory >= base->cb_VRAM &&
        (UBYTE *)dst->Memory < base->cb_VRAM + base->cb_VRAMSize) {
        src_off = (ULONG)((UBYTE *)src->Memory - base->cb_VRAM);
        dst_off = (ULONG)((UBYTE *)dst->Memory - base->cb_VRAM);
        reg_write(base, RTG_REG_ACCEL_SRC, src_off);
        reg_write(base, RTG_REG_ACCEL_SRC_PITCH, (UWORD)src->BytesPerRow);
        reg_write(base, RTG_REG_ACCEL_SRC_XY,
                  ((ULONG)(UWORD)sx << 16) | (UWORD)sy);
        reg_write(base, RTG_REG_ACCEL_DST, dst_off);
        reg_write(base, RTG_REG_ACCEL_PITCH, (UWORD)dst->BytesPerRow);
        reg_write(base, RTG_REG_ACCEL_XY,
                  ((ULONG)(UWORD)dx << 16) | (UWORD)dy);
        reg_write(base, RTG_REG_ACCEL_WH,
                  ((ULONG)(UWORD)w << 16) | (UWORD)h);
        reg_write(base, RTG_REG_ACCEL_FMTMASK, hostfmt << 8);
        reg_write(base, RTG_REG_ACCEL_OPCODE, opcode);
        reg_write(base, RTG_REG_ACCEL_COMMAND, RTG_ACCEL_BLIT_COPY);
        if (reg_read(base, RTG_REG_ACCEL_STATUS) == 1)
            return;
    }
    bi->BlitRectNoMaskCompleteDefault(bi, src, dst, sx, sy, dx, dy,
                                      w, h, opcode, fmt);
}

static ULONG rtg_format(RGBFTYPE fmt)
{
    switch (fmt) {
        case RGBFB_CLUT:     return RTG_FMT_CLUT;
        case RGBFB_R5G6B5:   return RTG_FMT_R5G6B5;
        case RGBFB_A8R8G8B8: return RTG_FMT_A8R8G8B8;
        default:             return 0;
    }
}

static ULONG format_bpp(RGBFTYPE fmt)
{
    switch (fmt) {
        case RGBFB_CLUT:     return 1;
        case RGBFB_R5G6B5:   return 2;
        case RGBFB_A8R8G8B8: return 4;
        default:             return 1;
    }
}

/* ------------------------------------------------------------------ */
/* P96 card API                                                        */
/* ------------------------------------------------------------------ */

static int FindCard(__REGA0(struct BoardInfo *bi), __REGA6(struct BellatrixCardBase *base))
{
    struct Library *ExpansionBase;
    struct ConfigDev *cd;

    ExpansionBase = OpenLibrary((STRPTR)"expansion.library", 0);
    if (!ExpansionBase)
        return 0;

    cd = FindConfigDev(NULL, RTG_MANUFACTURER, RTG_PRODUCT);
    CloseLibrary(ExpansionBase);
    if (!cd)
        return 0;

    base->cb_Regs = (volatile ULONG *)cd->cd_BoardAddr;
    if (reg_read(base, RTG_REG_ID) != RTG_ID_MAGIC)
        return 0;

    base->cb_VRAM     = (UBYTE *)cd->cd_BoardAddr + reg_read(base, RTG_REG_VRAM_OFF);
    base->cb_VRAMSize = reg_read(base, RTG_REG_VRAM_SIZE);

    bi->MemoryBase   = base->cb_VRAM;
    bi->MemorySize   = base->cb_VRAMSize;
    bi->RegisterBase = (APTR)cd->cd_BoardAddr;
    reg_write(base, RTG_REG_DEBUG, 0xCAFD0001);
    return 1;
}

static BOOL SetSwitch(__REGA0(struct BoardInfo *bi), __REGD0(BOOL state))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    reg_write(base, RTG_REG_ENABLE, state ? 1 : 0);
    /* P96 expects the inverse of the selected monitor-switch state. */
    return state ? 0 : 1;
}

static void SetColorArray(__REGA0(struct BoardInfo *bi), __REGD0(UWORD start), __REGD1(UWORD count))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    UWORD i;
    reg_write(base, RTG_REG_PAL_INDEX, start);
    for (i = 0; i < count; i++) {
        ULONG rgb = ((ULONG)bi->CLUT[start + i].Red << 16) |
                    ((ULONG)bi->CLUT[start + i].Green << 8) |
                     (ULONG)bi->CLUT[start + i].Blue;
        reg_write(base, RTG_REG_PAL_DATA, rgb);
    }
}

static void SetDAC(__REGA0(struct BoardInfo *bi), __REGD7(RGBFTYPE fmt))
{
    (void)bi; (void)fmt;
}

static void SetGC(__REGA0(struct BoardInfo *bi), __REGA1(struct ModeInfo *mi), __REGD0(BOOL border))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    (void)border;
    bi->ModeInfo = mi;
    base->cb_Width  = mi->Width;
    base->cb_Height = mi->Height;
    reg_write(base, RTG_REG_MODE_W, mi->Width);
    reg_write(base, RTG_REG_MODE_H, mi->Height);
}

static UWORD CalculateBytesPerRow(__REGA0(struct BoardInfo *bi), __REGD0(UWORD width), __REGD7(RGBFTYPE fmt))
{
    (void)bi;
    return (UWORD)(width * format_bpp(fmt));
}

static void SetPanning(__REGA0(struct BoardInfo *bi), __REGA1(UBYTE *mem), __REGD0(UWORD width),
                       __REGD1(WORD xoffset), __REGD2(WORD yoffset), __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    ULONG bpr = width * format_bpp(fmt);
    ULONG pan = (ULONG)(mem - base->cb_VRAM)
              + (ULONG)yoffset * bpr
              + (ULONG)xoffset * format_bpp(fmt);

    bi->XOffset = xoffset;
    bi->YOffset = yoffset;

    reg_write(base, RTG_REG_FORMAT, rtg_format(fmt));
    reg_write(base, RTG_REG_BYTES_PER_ROW, bpr);
    reg_write(base, RTG_REG_PAN, pan);
}

static APTR CalculateMemory(__REGA0(struct BoardInfo *bi), __REGA1(APTR addr), __REGD7(RGBFTYPE fmt))
{
    (void)bi; (void)fmt;
    return addr;
}

static ULONG GetCompatibleFormats(__REGA0(struct BoardInfo *bi), __REGD7(RGBFTYPE fmt))
{
    (void)bi; (void)fmt;
    return RGBFF_CLUT | RGBFF_R5G6B5 | RGBFF_A8R8G8B8;
}

static BOOL SetDisplay(__REGA0(struct BoardInfo *bi), __REGD0(BOOL state))
{
    (void)bi; (void)state;
    return 1;
}

static LONG ResolvePixelClock(__REGA0(struct BoardInfo *bi), __REGA1(struct ModeInfo *mi),
                              __REGD0(ULONG pixelclock), __REGD7(RGBFTYPE fmt))
{
    (void)bi; (void)pixelclock; (void)fmt;
    mi->PixelClock = 60 * 1000 * 1000;
    mi->pll1.Clock = 0;
    mi->pll2.ClockDivide = 1;
    return 0;
}

static ULONG GetPixelClock(__REGA0(struct BoardInfo *bi), __REGA1(struct ModeInfo *mi),
                           __REGD0(ULONG index), __REGD7(RGBFTYPE fmt))
{
    (void)bi; (void)mi; (void)index; (void)fmt;
    return 60 * 1000 * 1000;
}

static void SetClock(__REGA0(struct BoardInfo *bi))
{
    (void)bi;
}

static void SetMemoryMode(__REGA0(struct BoardInfo *bi), __REGD7(RGBFTYPE fmt))
{
    (void)bi; (void)fmt;
}

static void SetWriteMask(__REGA0(struct BoardInfo *bi), __REGD0(UBYTE mask))
{
    (void)bi; (void)mask;
}

static void SetClearMask(__REGA0(struct BoardInfo *bi), __REGD0(UBYTE mask))
{
    (void)bi; (void)mask;
}

static void SetReadPlane(__REGA0(struct BoardInfo *bi), __REGD0(UBYTE plane))
{
    (void)bi; (void)plane;
}

static void WaitVerticalSync(__REGA0(struct BoardInfo *bi), __REGD0(BOOL toggle))
{
    /* The host presenter owns cadence. Busy-waiting here deadlocks the harness:
     * its frame loop cannot advance RTG_REG_VBLANK while the guest is trapped
     * inside this callback. MiSTer.card likewise treats this as a no-op. */
    (void)bi;
    (void)toggle;
}

static ULONG GetVBeamPos(__REGA0(struct BoardInfo *bi))
{
    (void)bi;
    return 0;
}

static BOOL SetInterrupt(__REGA0(struct BoardInfo *bi), __REGD0(BOOL state))
{
    (void)bi; (void)state;
    return 1;
}

static void WaitBlitter(__REGA0(struct BoardInfo *bi))
{
    (void)bi;
}

static BOOL SetSprite(__REGA0(struct BoardInfo *bi), __REGD0(BOOL enable),
                      __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    (void)fmt;
    reg_write(base, RTG_REG_SPRITE_ENABLE, enable ? 1 : 0);
    return 1;
}

static void SetSpritePosition(__REGA0(struct BoardInfo *bi),
                              __REGD0(WORD x), __REGD1(WORD y),
                              __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    (void)x; (void)y; (void)fmt;
    x = bi->MouseX - bi->XOffset;
    y = bi->MouseY - bi->YOffset;
    reg_write(base, RTG_REG_SPRITE_XY,
              ((ULONG)(UWORD)x << 16) | (UWORD)y);
}

static void SetSpriteColor(__REGA0(struct BoardInfo *bi),
                           __REGD0(UBYTE idx), __REGD1(UBYTE red),
                           __REGD2(UBYTE green), __REGD3(UBYTE blue),
                           __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    (void)fmt;
    if (idx >= 3) return;
    reg_write(base, RTG_REG_SPRITE_COLOR_INDEX, (ULONG)idx + 1u);
    reg_write(base, RTG_REG_SPRITE_COLOR_DATA,
              ((ULONG)red << 16) | ((ULONG)green << 8) | blue);
}

static void SetSpriteImage(__REGA0(struct BoardInfo *bi),
                           __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    UWORD *src16 = bi->MouseImage;
    ULONG *src32 = (ULONG *)bi->MouseImage;
    ULONG width, height, row, block;
    int hires = (bi->Flags & BIF_HIRESSPRITE) != 0;
    (void)fmt;

    width = hires ? (ULONG)bi->MouseWidth * 2u : bi->MouseWidth;
    height = bi->MouseHeight;
    if (!src16 || width == 0 || height == 0) return;
    if (width > 32u) width = 32u;
    if (height > 64u) height = 64u;
    reg_write(base, RTG_REG_SPRITE_WH, (width << 16) | height);
    reg_write(base, RTG_REG_SPRITE_UPLOAD_RESET, 0);
    if (hires) {
        src32 += 2;
        for (row = 0; row < height; ++row) {
            ULONG p0 = *src32++, p1 = *src32++;
            for (block = 0; block < 2u; ++block) {
                ULONG packed = 0, bit;
                for (bit = 0; bit < 16u; ++bit) {
                    ULONG shift = 31u - block * 16u - bit;
                    ULONG pixel = ((p0 >> shift) & 1u) |
                                  (((p1 >> shift) & 1u) << 1);
                    packed |= pixel << (30u - bit * 2u);
                }
                reg_write(base, RTG_REG_SPRITE_UPLOAD_DATA, packed);
            }
        }
    } else {
        src16 += 2;
        for (row = 0; row < height; ++row) {
            ULONG p0 = *src16++, p1 = *src16++, packed = 0, bit;
            for (bit = 0; bit < 16u; ++bit) {
                ULONG pixel = ((p0 >> (15u - bit)) & 1u) |
                              (((p1 >> (15u - bit)) & 1u) << 1);
                packed |= pixel << (30u - bit * 2u);
            }
            reg_write(base, RTG_REG_SPRITE_UPLOAD_DATA, packed);
        }
    }
}

static int InitCard(__REGA0(struct BoardInfo *bi), __REGA1(const char **ToolTypes),
                    __REGA6(struct BellatrixCardBase *base))
{
    int i;
    (void)ToolTypes;

    reg_write(base, RTG_REG_DEBUG, 0xCAFD0003);
    bi->CardBase = (struct CardBase *)base;
    bi->ExecBase = base->cb_SysBase;
    bi->BoardName = (char *)"Bellatrix RTG";
    bi->BoardType = BT_uaegfx;
    bi->PaletteChipType = PCT_S3ViRGE;
    bi->GraphicsControllerType = GCT_S3ViRGE;

    /* The callbacks below accelerate supported subsets, but the card must not
     * advertise a complete blitter yet. AROS otherwise selects operations
     * such as full-screen minterm 0x00 that still fall back to the 68k. */
    bi->Flags |= BIF_GRANTDIRECTACCESS | BIF_HARDWARESPRITE | BIF_NOBLITTER;
    bi->Flags &= ~BIF_BLITTER;
    bi->RGBFormats = RGBFF_CLUT | RGBFF_R5G6B5 | RGBFF_A8R8G8B8;
    bi->SoftSpriteFlags = 0;
    bi->BitsPerCannon = 8;
    bi->MemoryClock = 100 * 1000 * 1000;

    for (i = 0; i < MAXMODES; i++) {
        bi->MaxHorValue[i] = 1920;
        bi->MaxVerValue[i] = 1080;
        bi->MaxHorResolution[i] = 1920;
        bi->MaxVerResolution[i] = 1080;
        bi->PixelClockCount[i] = 1;
    }

    bi->SetSwitch = SetSwitch;
    bi->SetColorArray = SetColorArray;
    bi->SetDAC = SetDAC;
    bi->SetGC = SetGC;
    bi->SetPanning = SetPanning;
    bi->CalculateBytesPerRow = CalculateBytesPerRow;
    bi->CalculateMemory = CalculateMemory;
    bi->GetCompatibleFormats = GetCompatibleFormats;
    bi->SetDisplay = SetDisplay;
    bi->ResolvePixelClock = ResolvePixelClock;
    bi->GetPixelClock = GetPixelClock;
    bi->SetClock = SetClock;
    bi->SetMemoryMode = SetMemoryMode;
    bi->SetWriteMask = SetWriteMask;
    bi->SetClearMask = SetClearMask;
    bi->SetReadPlane = SetReadPlane;
    bi->WaitVerticalSync = WaitVerticalSync;
    bi->GetVBeamPos = GetVBeamPos;
    bi->SetInterrupt = SetInterrupt;
    bi->WaitBlitter = WaitBlitter;
    bi->SetSprite = SetSprite;
    bi->SetSpritePosition = SetSpritePosition;
    bi->SetSpriteImage = SetSpriteImage;
    bi->SetSpriteColor = SetSpriteColor;
    bi->BlitPlanar2Chunky = AccelPlanar2Chunky;
    bi->FillRect = ProbeFillRect;
    bi->InvertRect = ProbeInvertRect;
    bi->BlitRect = ProbeBlitRect;
    bi->BlitTemplate = ProbeBlitTemplate;
    bi->BlitPattern = ProbeBlitPattern;
    bi->DrawLine = ProbeDrawLine;
    bi->BlitRectNoMaskComplete = ProbeBlitComplete;
    bi->BlitPlanar2Direct = AccelPlanar2Direct;

    return 1;
}

#undef SysBase

/* ------------------------------------------------------------------ */
/* library scaffolding                                                 */
/* ------------------------------------------------------------------ */

static const char card_name[] = CARD_NAME;
static const char card_idstring[] = CARD_NAME " " "1.0 (2026-07-03)";

/* exec's InitResident/MakeLibrary autoinit calls this with the freshly
 * made library in D0, segList in A0 and SysBase in A6 (see AROS
 * rom/exec/initresident.c AROS_UFC3). It was previously declared as
 * A0/A1/A6, so "base" received the segList (0 via the CardLoader), the
 * fields were scribbled at low memory and the returned NULL made exec
 * discard the library — bellatrix.card never reached the LibList. */
struct BellatrixCardBase *card_init(__REGD0(struct BellatrixCardBase *base),
                                    __REGA0(BPTR seglist),
                                    __REGA6(struct ExecBase *sysbase))
{
    (void)seglist;
    base->cb_SysBase = sysbase;
    base->cb_Lib.lib_Node.ln_Type = NT_LIBRARY;
    base->cb_Lib.lib_Node.ln_Name = (char *)card_name;
    base->cb_Lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    base->cb_Lib.lib_Version = CARD_VERSION;
    base->cb_Lib.lib_Revision = CARD_REVISION;
    base->cb_Lib.lib_IdString = (char *)card_idstring;
    return base;
}

/* Standard open/close/expunge/reserved stubs */
static ULONG card_open(__REGA6(struct BellatrixCardBase *base))
{
    base->cb_Lib.lib_OpenCnt++;
    base->cb_Lib.lib_Flags &= ~LIBF_DELEXP;
    return (ULONG)base;
}

static ULONG card_close(__REGA6(struct BellatrixCardBase *base))
{
    if (base->cb_Lib.lib_OpenCnt)
        base->cb_Lib.lib_OpenCnt--;
    return 0;
}

static ULONG card_expunge(void)
{
    return 0;
}

static ULONG card_reserved(void)
{
    return 0;
}

static const ULONG lib_vectors[] = {
    (ULONG)card_open,
    (ULONG)card_close,
    (ULONG)card_expunge,
    (ULONG)card_reserved,
    (ULONG)FindCard,      /* LVO 5 */
    (ULONG)InitCard,      /* LVO 6 */
    0xFFFFFFFF,
};

static const struct {
    ULONG size;
    const ULONG *vectors;
    const void *init_data;
    const void *init_func;
} lib_inittab = {
    sizeof(struct BellatrixCardBase),
    lib_vectors,
    NULL,
    (const void *)card_init,
};

extern const struct Resident card_romtag;

const struct Resident card_romtag = {
    RTC_MATCHWORD,
    (struct Resident *)&card_romtag,
    (APTR)(&card_romtag + 1),
    RTF_AUTOINIT | RTF_COLDSTART,
    CARD_VERSION,
    NT_LIBRARY,
    CARD_PRIORITY,
    (char *)card_name,
    (char *)card_idstring,
    (APTR)&lib_inittab,
};
