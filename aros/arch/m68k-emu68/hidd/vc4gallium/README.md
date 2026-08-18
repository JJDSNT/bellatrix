# vc4gallium on m68k-emu68

The VC4 Gallium driver builds for this target through **arch/arm-native's own
mmakefile**, not through a target of ours. That is deliberate and worth
recording, because it differs from how `vc4gfx` and `fbgfx` are wired here.

Its mmakefile is 9 KB and does far more than list sources: it reads Mesa's
`Makefile.sources`, generates the broadcom CLE pack headers from XML with a
Python script, builds a linklib from the Mesa driver, then runs
`galliumglue.py` to rewrite that archive so every Mesa-core call routes through
a function-pointer table, and only then links the module. Mirroring that here
would mean maintaining a copy of all of it against a file that changes with
Mesa.

So the targets are invoked where they live:

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

What the port needed is in `patches/aros/0035` and `0036`: seven ARM barriers,
one CP15 read guarded, one spin hint, and m68k trampolines in the glue
generator. Nothing structural.
