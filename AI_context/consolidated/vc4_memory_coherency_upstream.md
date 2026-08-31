# VC4 shared-memory coherency against AROS upstream HEAD

Date: 2026-08-29

## Question

Should Bellatrix add an uncached `vid_base | 0x80000000` alias beside the
existing write-through identity mapping, or make the firmware VC4 pool itself
Normal Non-Cacheable?

## Upstream examined

The comparison is against fetched AROS `origin/master`, not Bellatrix's pinned
checkout:

```text
AROS origin/master = 1bc141e0d18167f23b1ddedca82fa8bdff2eb656
                     2026-08-29T18:03:56+01:00
Bellatrix pin      = fbea2d8b8d6beca257be82583aae1b389909ee7c
```

The checkout was not moved and no upstream file was edited.

## What upstream does

The current AArch64 V3D port uses one canonical identity mapping, not aliases:

- `arch/arm-native/soc/broadcom/2708/hidd/v3d/v3d_mem.c` allocates aligned
  system-RAM arenas, cleans and invalidates their previous cache contents, then
  remaps `base -> base` with `KrnMapGlobal(... MAP_WriteThrough)`.
- On AArch64 that misleading generic flag deliberately means **Normal
  Non-Cacheable**, not architectural write-through. This is explicit in
  `arch/aarch64-native/kernel/mmu.c`: `MAP_WriteThrough` selects MAIR index 2,
  whose byte is `0x44` (`MAIR_IDX_NORMAL_NC`).
- When an arena is released, upstream remaps the same identity address back to
  ordinary cacheable RAM before returning it to Exec.
- The rationale in the upstream source is the same one needed here: CPU and GPU
  agree on contents without per-submit cache maintenance.

Upstream's VC4 code still exposes firmware `ALLOCMEM/LOCKMEM` results as the
physical address masked with `0x3fffffff`. `GPU_BUS_ADDR()` independently adds
the VC4 `0xC0000000` bus alias. There is no CPU-side `| 0x80000000` contract and
no VC4 Gallium use of `vc4-mem` to discover one.

The upstream `vcgfx_offbitmap.c` independently documents the failure mode:
firmware allocations are CPU-cached while DMA reaches them through the
uncached VC alias, producing stale striped graphics. It disables GPU placement
until the CPU allocation is mapped uncached. That is evidence against treating
write-through or a GPU bus alias as bidirectional coherency.

## Bellatrix before this change

Emu68 publishes `vc4-mem = <vid_base size>` and maps only:

```text
CPU virtual vid_base -> physical vid_base -> MAIR 0xbb write-through
```

Bellatrix's VC4 Gallium `gpu_mem_alloc()` returns the firmware physical address
directly as the CPU pointer. Its submission path says all such buffers are
uncached and therefore needs only a barrier, but the platform mapping was
write-through. The code and its coherency contract disagreed.

## Decision and implementation

Do not introduce mixed-attribute aliases. Patch
`patches/emu68/0018-map-vc4-memory-normal-non-cacheable.patch` changes the one
identity mapping to Emu68's MAIR `0x44` `MMU_ATTR_UNCACHED` entry:

```text
CPU virtual vid_base -> physical vid_base -> Normal Non-Cacheable
VC4 bus address       -> 0xC0000000 | physical
```

This preserves every existing pointer and DT ABI while making the driver's
no-cache-maintenance statement true. It also follows upstream AArch64's
single-view policy and avoids mapping the same physical bytes simultaneously
with incompatible cacheability attributes.

## Why this is the better first implementation

For Bellatrix's current ownership model, one Normal-NC identity view is better
than a second `vid_base | 0x80000000` alias:

- correctness does not depend on every Mesa, vcgfx, DMA, or mailbox caller
  remembering which CPU alias is legal for a particular ownership transition;
- CPU and VC4 addresses retain the existing simple relationship: the CPU uses
  physical `X`, while VC4 uses bus address `0xC0000000 | X`;
- GPU-to-CPU visibility cannot be defeated by a stale write-through cache line;
- there is no mixed-cacheability alias of one physical allocation;
- `vc4-mem`, firmware allocation results, `gpu_mem_alloc()`, `MMAP_BO`, and
  scanout metadata need no ABI change;
- it matches the explicit AROS AArch64 HEAD policy for GPU arenas rather than
  creating a Bellatrix-only convention.

This is a correctness-first choice, not a claim that uncached access is free.
Normal-NC removes the cache-maintenance and stale-line problem by making every
CPU access reach the shared memory domain, so CPU-heavy rendering, clears, or
readback can be slower than the old write-through mapping. That cost is easier
to measure and optimise than intermittent corruption caused by an ambiguous
ownership/cache contract.

## If Normal-NC is too slow

The preferred evolution is to split physical memory by use, keeping exactly
one cacheability attribute for each physical byte:

1. a Normal-NC pool for bidirectional CPU/VC4 BOs, command lists, uniforms,
   shaders, textures updated by both agents, and GPU readback;
2. a separately allocated cacheable or write-through pool for workloads with
   explicit one-way ownership and cache maintenance, if measurements show it
   is worthwhile;
3. scanout treated according to its actual producer/consumer path rather than
   assumed to have the same policy as every Gallium BO.

Do not use simultaneous WT and NC virtual aliases as the performance fallback.
If a cacheable pool is introduced, ownership transitions must be explicit and
use the AROS `CachePreDMA`/`CachePostDMA` contract or an equivalently documented
driver boundary.

## Validation and remaining hardware check

Required local validation is setup-series verification, Emu68/Bellatrix build,
and QEMU boot. QEMU can validate mapping construction and driver startup, but
not cache coherency or the performance cost of CPU access to the pool.

Real Pi 3 validation must run the existing VC4 GL workload and check:

- no stale textures, partial buffers, or striped output;
- no regression in framebuffer handover and scanout;
- submit logs still show physical CPU pointers and `0xC...` GPU bus addresses;
- frame time impact from changing the whole firmware pool from write-through
  to Normal-NC.

Capture at least CPU submission time, GPU completion time, present/frame time,
and any CPU readback workload separately. A visual pass alone cannot establish
coherency or quantify whether a later physical-pool split is justified.

If CPU rendering into this pool becomes too expensive, the next design should
split physical pools by ownership. A mixed WT/NC alias of the same bytes is not
the fallback.
