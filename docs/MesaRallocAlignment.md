# Mesa 26.2.5 ralloc alignment: investigation and agent task

Status: **implemented** in `mesa-26.2.5-bellatrix.diff` (2026-10-26). Verified
on x86-64 and under QEMU. Not yet run on a Pi. Not yet submitted upstream.
Scope: `aros/workbench/libs/mesa/bellatrix/mesa-26.2.5-bellatrix.diff`, specifically `src/util/ralloc.c`.
Goal: replace the Bellatrix-specific workaround with a small, portable, upstreamable fix without breaking ralloc's tree invariants.

## Result

The sections after this one are the task as it was written. This section
records what came out of it.

### Findings

- **The 8 is Mesa's own requirement.** `ralloc_header` is
  `alignas(HEADER_ALIGN)`: 8 on 32-bit targets, 16 on 64-bit ones. Measured
  with the AROS m68k compiler, `alignof(max_align_t)`, `alignof(double)`,
  `alignof(long long)` and `alignof(void *)` are all 2. So an AROS `malloc()`
  block at 4 modulo 8 is ABI-correct, and AROS needs no change.
- **What fires is the linear allocator.** It asserts
  `ptr % SUBALLOC_ALIGNMENT == 0` with `SUBALLOC_ALIGNMENT` fixed at 8, and
  the gc allocator asserts `alignment <= HEADER_ALIGN`. Lowering
  `HEADER_ALIGN` to what `malloc()` promises would therefore not be enough.
  The address has to be aligned.
- **`os_realloc_aligned()` frees the old block even when the new
  allocation fails.** It cannot stand in for `realloc()`.
- **The header already had a size field**, `unsigned size`, but only under
  `!NDEBUG`. Making it unconditional costs nothing, because it fills existing
  padding. The header stays 48 bytes on x86-64 (debug and release) and
  24/32 bytes (release/debug) on m68k. This field replaces `aros_size`.
- **Two upstream defects turned up on the way:**
  - `size + sizeof(ralloc_header)` is never checked for overflow:
    `ralloc_size(ctx, SIZE_MAX)` returns a tiny block.
  - `resize()` never updates `size`, so in debug builds
    `ralloc_total_size()` reports the size from before a `reralloc`.
- **Mesa `main` still uses `malloc`/`realloc`/`free`** (checked 2026-10-26).
  Nobody has solved this upstream.

### The change

- `MALLOC_ALIGNS_HEADER` is `alignof(max_align_t) >= alignof(ralloc_header)`,
  a compile-time constant. Where it is true, the code is upstream's
  `malloc`/`realloc`/`free`, and the aligned path compiles away. Checked on
  x86-64: `resize()` contains no `memcpy`.
- Where it is false (m68k):
  - blocks come from `os_malloc_aligned()`;
  - `resize()` allocates, copies `MIN(old size, new size)` bytes, repairs the
    links, then frees the old block;
  - a failed resize leaves the old block untouched.
- `block_size()` computes the block size with an overflow check. On the
  aligned path it also rejects sizes that do not fit in `unsigned`.
- No `#ifdef __AROS__` or `__m68k__`.

### Verification

- **Host (x86-64):**
  - the new `ralloc_test` (6 cases), `linear_test` and `gc_alloc_tests`, under
    ASan and UBSan;
  - malloc path and forced aligned path, each in debug and release;
  - allocation failures injected through `--wrap=malloc,realloc`;
  - the unmodified `ralloc.c` fails the overflow and total-size cases.
- **QEMU, Mesa 26.2.5, softpipe:**
  - `gl-info` reports `3.3 (Compatibility Profile) Mesa 26.2.5`, with no
    assertion.
  - A `gears` run of 90 s before Wanderer behaves the same with the old and
    the new library: no assertion, and the same idle PC profile. The probe
    cannot show whether gears draws before the desktop is up, with either
    library.
- **Pi 3: not run.**

### Upstream

One MR to gitlab.freedesktop.org/mesa/mesa, with two commits (kept in
`AI_context/upstream/mesa-ralloc-alignment/`, with the submission steps):

1. `util/ralloc: fail allocations whose block size overflows`
2. `util/ralloc: don't assume malloc() aligns beyond max_align_t`, which also
   adds `src/util/tests/ralloc_test.cpp`.

Both apply cleanly to Mesa `main` as of 2026-10-26. The argument made to
reviewers is C11 conformance, not AROS: no behaviour change and no header
growth on any CI target, plus two defect fixes with tests. Once it lands, the
`ralloc.c` hunk leaves `mesa-26.2.5-bellatrix.diff` at the next Mesa bump.

Kept apart from this change:

- **`HAVE_POSIX_MEMALIGN`** stays off in `mesa.cfg`. The comment there says
  Mesa 21 removed the path, but 26.2.5 still has it. Turning it on would drop
  the 12 extra bytes that `os_malloc_aligned()`'s fallback adds to each block,
  and it is to be measured on its own.
- **`nir_opt_varyings.c` and `cso_cache.h`** stay out of the MR.

## Observed issue

Mesa's `ralloc.c` aligns allocation sizes with `align64(..., alignof(ralloc_header))`, but uses ordinary `malloc` / `realloc` / `free`. Bellatrix reports that AROS m68k `malloc` may return addresses congruent to 4 mod 8, while `ralloc_header` needs 8-byte alignment. Rounding the requested *size* does not align the returned *address*.

Bellatrix's current second-stage Mesa diff:
- adds `util/os_memory.h`;
- allocates with `os_malloc_aligned(..., alignof(ralloc_header))`;
- stores payload size in an `aros_size` header member;
- replaces `realloc` with aligned allocation + `memcpy` + parent/sibling/child link repair + aligned free;
- replaces `free` with `os_free_aligned`.

This is a working hypothesis from the patch, not evidence of passing runtime regression tests.

## AROS allocator evidence

Upstream `compiler/crt/stdc/malloc.c` returns `AllocPooled(...)+AROS_ALIGN(sizeof(size_t))`. This offset may leave the returned pointer at 4 mod 8; do not assume the allocator is broken without establishing the target ABI's required fundamental alignment. `compiler/crt/stdc/realloc.c` already uses allocate/copy/free for growth and large shrinkage; `compiler/crt/stdc/aligned_alloc.c` supports aligned allocation, freed via `free`. Do not change AROS malloc as a prerequisite.

## Agent investigation (do before editing)

1. Inspect the exact Mesa 26.2.5 tarball, including `src/util/ralloc.c`, `src/util/os_memory.h`, `src/util/os_memory_aligned.h` (if present), and definitions of `os_malloc_aligned`, `os_free_aligned`, `os_realloc_aligned`. Record signatures, ownership, portability and failure semantics. **Do not assume `os_realloc_aligned` preserves the old allocation on OOM**; inspect it. If it frees on failure, do not use it as a drop-in replacement for `realloc`.
2. Determine why `ralloc_header` requires 8-byte alignment (its explicit `alignas` or members), verify `alignof(ralloc_header)`, `alignof(max_align_t)`, `AROS_ALIGN(sizeof(size_t))` and actual `malloc` pointer residues on Bellatrix m68k. Confirm that the returned payload `PTR_FROM_HEADER` is correctly aligned too.
3. Audit all `ralloc.c` allocation, resize, steal/reparent, destructor and free paths. Verify that all allocations are freed with the matching function and that failure leaves the original allocation and tree unchanged.
4. Investigate whether an existing Mesa allocator or upstream change already solves the issue; do not duplicate it.

## Implementation preferences

- Prefer a portable Mesa-level fix, not `#ifdef __AROS__` or `#ifdef __m68k__`, if feasible.
- Keep ordinary `malloc/realloc/free` where their alignment guarantee is sufficient; document how that guarantee is established at build time, not by wishful runtime checks.
- For the aligned path, consider the existing Mesa aligned allocator; if resize requires copying, track the original payload size generically (`allocation_size`, not `aros_size`) and preserve `realloc`-style failure semantics.
- Compare the cost of an additional header field with alternatives (e.g. storing a total allocation size); do not silently increase all allocations without justification.
- Check integer overflow for `size + sizeof(ralloc_header)` and alignment rounding; use overflow-safe arithmetic.
- If changing `os_realloc_aligned` failure behavior is necessary, separate that generic change from `ralloc` integration, with dedicated tests.
- Keep unrelated `cso_cache.h` and `nir_opt_varyings.c` changes out of this patch/PR.

## Acceptance tests

- 8-byte alignment for header and returned payload under Bellatrix/AROS m68k; normal builds on at least one other architecture.
- Allocate/resize grow/shrink (including zero-size semantics), preserve the correct minimum number of payload bytes.
- Inject allocation failures; original pointer, data, and parent/child/sibling links must remain valid.
- Parent-child-grandchild trees, sibling chains, steal/reparent, recursive frees and destructors remain correct after moving a block.
- Check no double free, invalid free, overflow or unaligned access; run sanitizers where available.
- Confirm build against Mesa 26.2.5 and record Bellatrix runtime test output.

## Expected agent deliverables

1. Brief findings with code locations and measured alignment values.
2. Minimal proposed Mesa patch (or two independent patches if generic aligned-realloc semantics need fixing), plus tests.
3. Explain portability, performance/header overhead, failure semantics and upstream suitability.
4. Only after tests, update `mesa-26.2.5-bellatrix.diff` and document how to submit the generic fix upstream.

References:
- https://github.com/JJDSNT/bellatrix/blob/main/aros/workbench/libs/mesa/bellatrix/mesa-26.2.5-bellatrix.diff
- https://github.com/aros-development-team/AROS/blob/master/compiler/crt/stdc/malloc.c
- https://github.com/aros-development-team/AROS/blob/master/compiler/crt/stdc/realloc.c
- https://github.com/aros-development-team/AROS/blob/master/compiler/crt/stdc/aligned_alloc.c
