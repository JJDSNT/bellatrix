````markdown
# Suspected Issues to Check — Bellatrix / AROS m68k-emu68

The following issues were pointed out after reviewing the current Bellatrix/AROS patches.

Treat them as **suspects to verify in the current code**, not as confirmed bugs.

For each item:

1. check whether the problem exists;
2. identify the affected code;
3. fix it if confirmed;
4. avoid unrelated architectural changes.

---

## 1. Native Hardware Access Through Emu68 MMU Fault Handler

Check whether the hardware code currently reused from:

~~~text
arch/arm-native/soc/broadcom
~~~

causes native Raspberry Pi hardware accesses from M68K to go through the **Emu68 MMU page-fault handler**.

The concern is that something equivalent to this may currently happen:

~~~text
AROS M68K driver
    ↓
Broadcom register access
    ↓
Emu68 MMU fault
    ↓
fault handler
    ↓
actual hardware access
~~~

If this is happening for normal/high-frequency Pi hardware accesses, it may explain part of the current performance loss.

Pay particular attention to:

- SD card;
- USB;
- VideoCore;
- mailbox;
- interrupt controller;
- timers.

If confirmed, determine which accesses can use a more appropriate direct/fast path instead of repeatedly going through the generic page-fault mechanism.

Do **not** change intentional fault handling used for emulated Amiga MMIO/Rigel merely because native Pi hardware accesses may need optimization.

---

## 2. Mesa M68K Alignment and Structure Packing

Check Mesa/Gallium code compiled for M68K for assumptions inherited from ARM32/AArch64 concerning:

- structure alignment;
- structure packing;
- 4-byte alignment;
- 8-byte alignment.

The VC4/V3D-related structures may have been written/tested assuming the alignment normally provided by ARM.

Those assumptions may not hold with the M68K ABI/toolchain.

Look especially at structures exchanged with VC4 or used by the shader compiler.

Fix explicit incorrect alignment/packing assumptions rather than applying global packing changes.

---

## 3. Big-Endian M68K vs Little-Endian VC4

Audit the VC4 Gallium path for endian assumptions.

The M68K environment is big-endian while VC4 is little-endian.

Check particularly:

- VC4 command structures;
- shader binaries;
- command lists;
- GPU-visible data;
- packed fields;
- direct structure-to-buffer writes.

Make sure data consumed by VC4 is explicitly represented in the byte order expected by the GPU rather than accidentally relying on native CPU endianess.

---

## 4. Mesa Shader Compiler Problems Before VC4

Do not assume that current Mesa crashes are necessarily VC4 driver bugs.

Check whether the problem already occurs in Mesa's internal compiler path, particularly:

- NIR;
- QIR where VC4 is involved;
- Mesa state trackers.

A useful discriminator is **softpipe**.

If the same shader/compiler failure occurs with softpipe, investigate the generic Mesa/M68K path before changing the VC4 hardware driver.

---

## 5. Mesa Memory Allocators

Check the AROS/Amiga-specific allocation code used by Mesa, including paths related to:

~~~text
r_alloc
~~~

Verify that the M68K implementation provides the alignment and allocation semantics expected by Mesa.

Also check whether this interacts with the existing TLSF work/problems.

Do not assume TLSF is responsible; verify the allocator boundary first.

---

## 6. Shared Library Runtime

If Mesa is intended to become an AROS `.library`, check for problems involving:

- `libm`;
- `clib`;
- runtime dependencies;
- linking/ABI assumptions.

Keep these problems separate from VC4/GPU failures.

---

## 7. Compare VC4 Shader Output Against Linux

If shader generation reaches the VC4 backend, compare the generated shader binary against a known-working Linux build using the **same Mesa version and equivalent shader**.

This can help expose:

- endian errors;
- packing errors;
- alignment errors;
- incorrect QIR/code generation on M68K.

The objective is not necessarily byte-for-byte identity if the compiler permits different valid output, but to determine whether the M68K output is structurally/semantically valid.

---

## 8. VC4 Gallium Is Not `vc4gfx`

Keep these components distinct:

~~~text
Mesa/Gallium VC4 driver
```

and:

~~~text
vc4gfx
```

Success or failure in `vc4gfx` does not by itself establish whether the Mesa VC4 path is correct.

---

# Priority

Check the suspects roughly in this order:

~~~text
1. Native Pi hardware accidentally going through Emu68 page faults
2. Mesa allocator/alignment problems
3. Mesa generic shader compiler / NIR problems
4. VC4 endian and packing problems
5. QIR / generated VC4 shader output
6. .library / libm / clib issues
~~~

The first item is particularly relevant to the current Bellatrix performance investigation.

The remaining items are primarily suspects for the current difficulty of getting Mesa/Gallium working correctly on M68K.

For each confirmed problem, atualize o ai e context e make the smallest targeted correction possible and report what was found before expanding the scope.
````
