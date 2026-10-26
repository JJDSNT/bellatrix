**util/ralloc: don't assume malloc() aligns beyond max_align_t**

`ralloc_header` is declared `alignas(8)` on 32-bit targets and `alignas(16)` on
64-bit ones, because that is what glibc's `malloc()` returns. C11 only promises
`alignof(max_align_t)`, and some targets have less: on m68k it is 2, and
AROS's `malloc()` hands back blocks that are 4 modulo 8. The header then lands
misaligned, and the linear allocator's `SUBALLOC_ALIGNMENT` assertion fires
on the first context created.

This MR keeps `malloc()`/`realloc()`/`free()` wherever `alignof(max_align_t)` is
at least the header's alignment. That covers every platform Mesa CI builds.
Elsewhere, ralloc takes blocks from `os_malloc_aligned()` and resizes by
allocating, copying and freeing. `os_realloc_aligned()` is not used because
it frees the old block even when the new allocation fails.

The choice is a compile-time constant, so on mainstream targets the aligned
path compiles away. The copy needs the payload size, so the existing debug-only
`size` field is now always present. It fills padding the header already has:
`sizeof(ralloc_header)` is unchanged on x86-64 (48, debug and release) and on
32-bit (24 release, 32 debug).

Two defects come out on the way, both covered by the new `ralloc_test`:

- **Size overflow.** `ralloc_size(ctx, SIZE_MAX)` used to return a tiny block,
  because `size + sizeof(ralloc_header)` wrapped. The block size is now
  computed with an overflow check, and the allocation fails instead
  (first commit).
- **Stale debug size.** In debug builds, `ralloc_total_size()` reported the
  size from before a `reralloc`, because `resize()` never updated `size`.

Tested:
- `ralloc_test` (new), `linear_test` and `gc_alloc_tests` under ASan and UBSan
  on x86-64, debug and release.
- The same suite with the aligned path forced on x86-64.
- Allocation failures injected into `malloc`/`realloc`: a failed resize leaves
  the block, its contents and the parent, sibling and child links intact.
- The unmodified `ralloc.c` fails the new tests (overflow, total size).
- On AROS m68k (Mesa 26.2.5, softpipe, under QEMU): context creation and
  `glGetString` work, where the unpatched build asserts in the linear
  allocator.
