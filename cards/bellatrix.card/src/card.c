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

static void ProbeFillRect(__REGA0(struct BoardInfo *bi),
                          __REGA1(struct RenderInfo *ri),
                          __REGD0(WORD x), __REGD1(WORD y),
                          __REGD2(WORD w), __REGD3(WORD h),
                          __REGD4(ULONG pen), __REGD5(UBYTE mask),
                          __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    probe_report(base, PROBE_FILLRECT, w, h,
                 ((ULONG)fmt << 8) | mask);
    bi->FillRectDefault(bi, ri, x, y, w, h, pen, mask, fmt);
}

static void ProbeInvertRect(__REGA0(struct BoardInfo *bi),
                            __REGA1(struct RenderInfo *ri),
                            __REGD0(WORD x), __REGD1(WORD y),
                            __REGD2(WORD w), __REGD3(WORD h),
                            __REGD4(UBYTE mask), __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    probe_report(base, PROBE_INVERTRECT, w, h,
                 ((ULONG)fmt << 8) | mask);
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
    probe_report(base, PROBE_BLITRECT, w, h,
                 ((ULONG)fmt << 8) | mask);
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
    probe_report(base, PROBE_BLITTEMPLATE, w, h,
                 ((ULONG)fmt << 8) | mask);
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
    probe_report(base, PROBE_BLITPATTERN, w, h,
                 ((ULONG)fmt << 8) | mask);
    bi->BlitPatternDefault(bi, ri, pat, x, y, w, h, mask, fmt);
}

static void ProbeDrawLine(__REGA0(struct BoardInfo *bi),
                          __REGA1(struct RenderInfo *ri),
                          __REGA2(struct Line *line), __REGD0(UBYTE mask),
                          __REGD7(RGBFTYPE fmt))
{
    struct BellatrixCardBase *base = (struct BellatrixCardBase *)bi->CardBase;
    probe_report(base, PROBE_DRAWLINE, 0, 0,
                 ((ULONG)fmt << 8) | mask);
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
    probe_report(base, PROBE_BLITCOMPLETE, w, h,
                 ((ULONG)fmt << 8) | opcode);
    bi->BlitRectNoMaskCompleteDefault(bi, src, dst, sx, sy, dx, dy,
                                      w, h, opcode, fmt);
}

static ULONG rtg_format(RGBFTYPE fmt)
{
    switch (fmt) {
        case RGBFB_CLUT:     return RTG_FMT_CLUT;
        case RGBFB_R5G6B5:   return RTG_FMT_R5G6B5;
        case RGBFB_A8R8G8B8: return RTG_FMT_A8R8G8B8;
        default:             return RTG_FMT_CLUT;
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

    bi->Flags |= BIF_GRANTDIRECTACCESS | BIF_NOBLITTER;
    bi->RGBFormats = RGBFF_CLUT | RGBFF_R5G6B5 | RGBFF_A8R8G8B8;
    bi->SoftSpriteFlags = bi->RGBFormats;   /* all sprites in software */
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
    bi->FillRect = ProbeFillRect;
    bi->InvertRect = ProbeInvertRect;
    bi->BlitRect = ProbeBlitRect;
    bi->BlitTemplate = ProbeBlitTemplate;
    bi->BlitPattern = ProbeBlitPattern;
    bi->DrawLine = ProbeDrawLine;
    bi->BlitRectNoMaskComplete = ProbeBlitComplete;

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
