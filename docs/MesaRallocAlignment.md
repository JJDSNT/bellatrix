# Mesa 26.2.5 ralloc alignment: investigation and agent task

Status: research / implementation proposal; NOT validated on Bellatrix hardware.
Scope: `aros/workbench/libs/mesa/bellatrix/mesa-26.2.5-bellatrix.diff`, specifically `src/util/ralloc.c`.
Goal: replace the Bellatrix-specific workaround with a small, portable, upstreamable fix without breaking ralloc's tree invariants.

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
