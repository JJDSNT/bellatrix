# The VideoCore card's native/RTG switch, and its scalers

Reference note, written 2026-08-31 after asserting -- wrongly, and twice --
that no such mechanism existed. It does. This records what it is and where,
so the next person does not have to find it the way I did.

## Where it lives, and where it does not

Not in Emu68. Emu68 asks the firmware for a framebuffer
(`src/raspi/start_rpi64.c: init_display()`) and writes pixels into it; there is
no HVS code and no display list in its tree at all.

It is in **`VideoCore.card`** -- Emu68-tools' Picasso96 RTG board driver, a
separate project (`external/VideoCore.card` in the legacy tree). That driver
programs the Hardware Video Scaler directly: display lists at `0xf2402000`,
the active list pointer at `0xf2400024`, and filter kernels of its own.

## The switch

`SetSwitch(struct BoardInfo *, UWORD enabled)` is the Picasso96 entry point a
board provides for "show the Amiga's own video instead of the RTG screen". On
real hardware with a PiStorm that is a physical thing: the Amiga's video comes
in through the CSI (camera) port, Unicam captures it, and the HVS shows that
plane instead of the RTG framebuffer.

The card generalises the trigger:

```c
enum SwitchMode { None = 0, CTS, RTS, DTR, SEL, CSI };
```

-- a serial-line signal, the Amiga's own SEL line, or CSI -- with
`vc4_SwitchInverted` to flip the sense. In `CSI` mode `SetSwitch()` writes the
Unicam display list into `0xf2400024` when the switch says "native" and the RTG
list when it says "RTG". One list or the other; nothing is composited.

## What the legacy tree did with it

Two patches, and the idea is small:

- **0036** (against Emu68) publishes the framebuffer as a device-tree property
  in `display_logo()`:

  ```c
  of_node_t *emu68 = dt_find_node("/emu68");
  uint32_t native_fb[] = { (uint32_t)framebuffer, fb_width, fb_height, pitch };
  dt_add_property(emu68, "bellatrix-native-fb", native_fb, sizeof(native_fb));
  ```

- **0037** (against `VideoCore.card`) reads it in `FindCard()`, builds a second
  display list for it (`VC4_ConstructNativeDL` / `VC6_ConstructNativeDL`), and
  makes the CSI branch of `SetSwitch()` choose that list instead of Unicam's:

  ```c
  /* Bellatrix has no physical Denise/CSI capture.  Its Rigel framebuffer is
     published by Emu68 and can be fed to the same HVS native/RTG switch. */
  ```

So on a machine with no camera and no real Denise, the switch a PiStorm user
already knows now flips between the RTG desktop and the emulated chipset's
picture.

Note what legacy's framebuffer *was*: Emu68's own, the one `init_display()`
returned. Bellatrix drew straight into it. There was no aperture and no
per-frame copy -- the arrangement this tree has (Rigel composes into its own
buffer, we copy into `$01000000`, vcgfx reads from there) did not exist, so the
coherency question `src/amiga/frame.c` argues the copy removes was a question
legacy never had.

## The scalers

The card carries three filter kernels, each `BuddyAlloc(VC4Base, 11)`:

```c
ULONG vc4_ScalingKernel;   /* the desktop's, recomputed on demand */
ULONG vc4_UnityKernel;     /* 1:1 */
ULONG vc4_UnicamKernel;    /* the native/captured plane's */
```

built by `compute_scaling_kernel(base, offset, b, c)` -- Mitchell-Netravali,
B and C settable at runtime through the card's message interface -- or
`compute_nearest_neighbour_kernel()` when `vc4_IntegerScaler` is set from a
tooltype.

**But `VC4_ConstructNativeDL()` writes `CONTROL_UNITY`.** The machinery to
scale that plane is right there and legacy did not use it for the native list:
the chipset picture went up 1:1. Worth knowing before quoting "it has scalers"
as though the problem were already solved there.

## Why this matters to the current tree

`vcgfx.hidd` raises the chipset's picture as an **overlay plane composited
above the desktop** (`vc4_hvs_overlay()`, reused from windowed GL), and that
path refuses outright when the desktop is not at native size:

```c
/* Only on an owned, unity fb plane: on scaled desktops the overlay
 * contents would need scaling too to line up */
if (!st->hvs_Active || st->hvs_DestW != st->hvs_SrcW || ...) return FALSE;
```

DPaint's mode requester is a 640x480 screen scaled to 1440x1080
(`[VC4HVS] takeover: ... fb 640x480 -> 1440x1080 at 240,0`). So exactly when
an Amiga screen is about to be asked for, the desktop is scaled and the overlay
is unavailable. A switched display list has no such constraint -- there is
nothing to line up with, because there is only one plane -- and the kernels
above are how it would be made readable rather than tiny.

The switch is not portable as it stands: it lives in a driver this tree does
not use. What ports is the shape -- vcgfx already authors HVS display lists, so
a second list plus a `0xf2400024` flip is the same idea in our own driver, and
it would replace `vcgfx_denise.c`'s polling task with something the graphics
system drives.

See ISSUE-0083.
