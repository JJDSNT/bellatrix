#ifndef FBGFX_SUPPORT_H
#define FBGFX_SUPPORT_H

#include <exec/types.h>
#include <oop/oop.h>

#define PCI_VENDOR_S3 0x5333

#define vgaIOBase 0x3d0

struct HWData
{
    APTR	 framebuffer;
    ULONG	 fbsize;
    ULONG	 width;
    ULONG	 height;
    ULONG	 depth;
    ULONG	 bytesperpixel;
    ULONG	 bitsperpixel;
    ULONG	 redmask;
    ULONG	 greenmask;
    ULONG	 bluemask;
    ULONG	 redshift;
    ULONG	 greenshift;
    ULONG	 blueshift;
    ULONG	 bytesperline;
    /* TRUE when the surface's pixels are stored in the opposite byte order to
       the CPU's, which a 16-bit 5:6:5 halfword on a big-endian target is. The
       display class hands this to aHidd_PixFmt_SwapPixelBytes; deciding it
       belongs in initFBGfxHW(), which is the only part of this driver that
       knows what the hardware actually produced. */
    BOOL	 swappixelbytes;
    BOOL	 owned;
    UBYTE	 palettewidth;
    UBYTE	 DAC[768];
    /* Used by PCI scanning routine */
    OOP_AttrBase pciDeviceAttrBase;
};

#undef HiddPCIDeviceAttrBase
#define HiddPCIDeviceAttrBase sd->pciDeviceAttrBase

struct FBGfx_staticdata;
struct FBGfxBitMapData;

BOOL initFBGfxHW(struct HWData *);
void DACLoad(struct FBGfx_staticdata *, UBYTE *, unsigned char, int);
void ClearBuffer(struct HWData *data);
void fbDoRefreshArea(struct HWData *hwdata, struct FBGfxBitMapData *data,
		       LONG x1, LONG y1, LONG x2, LONG y2);

#endif /* FBGFX_SUPPORT_H */
