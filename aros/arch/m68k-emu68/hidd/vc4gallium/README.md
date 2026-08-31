# vc4gallium on m68k-emu68

Bellatrix owns the complete emu68 VC4 HIDD source in this directory. The
upstream `arch/arm-native` mmakefile remains only as the single integration
point that generates Mesa/CLE/GalliumCoreAPI artifacts and links the module;
patch 0079 makes that target select this directory on emu68. Raspi builds
continue selecting the upstream ARM sources.

Its mmakefile is 9 KB and does far more than list sources: it reads Mesa's
`Makefile.sources`, generates the broadcom CLE pack headers from XML with a
Python script, builds a linklib from the Mesa driver, then runs
`galliumglue.py` to rewrite that archive so every Mesa-core call routes through
a function-pointer table, and only then links the module. Mirroring that here
would mean maintaining a copy of all of it against a file that changes with
Mesa.

Keeping that build machinery at its upstream integration point avoids a
duplicate `hidd-vc4gallium` target while leaving all driver implementation,
headers, DRM compatibility shims, and module configuration under project
ownership.

The targets are invoked where the integration lives:

```
make -C out/build/aros linklibs-gallium_vc4       # Mesa's VC4 driver
make -C out/build/aros linklibs-gallium_vc4-gca   # rewritten through the table
make -C out/build/aros hidd-vc4gallium            # the module itself
make -C out/build/aros mesa3dgl-library           # the other half of the table
```

**The last one is not optional.** `galliumglue.py` generates both halves of the
GalliumCoreAPI: the consumer table linked into this hidd and the provider table
linked into `mesa3dgl.library`. A driver built against one table and a
mesa3dgl carrying another refuses to bind at `CreatePipeScreen` -- a clean
failure with a reason on the serial line, but a failure. Rebuild and ship both
together, always.

Commit `cb4e697` preserves the patch-by-patch history of how the original ARM
driver evolved into this snapshot. Those implementation patches were removed
from the active series after migration. New emu68 driver changes belong
directly here; only changes to the remaining upstream build/Mesa integration
require an AROS patch.
