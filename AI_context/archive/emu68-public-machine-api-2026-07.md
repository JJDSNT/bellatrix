# Emu68 "public machine API" — archived code (retired 2026-07-16)

Archived by request before removal, for calm future review. Not deleted
from git history either way — this file exists so the removed code is
conveniently reviewable in one place without needing to check out old
commits.

## What this was

An attempt (commits `c8599c8`..`f28b21a`, 2026-07-13, plus later
additions through `7b4f7c9` on 2026-07-15) to route M68K effective-address
and instruction-fetch access through an explicit "machine API"
(`emu68_machine_*` in `src/cpu/emu68/`) instead of Emu68's native
fault-driven MMU dispatch, plus a per-instruction-boundary "quantum
progress" report call intended to synchronize chipset time.

## Why it was retired (Jaime, 2026-07-16 session)

- "A API explícita ampla não é mais uma direção arquitetural do
  Bellatrix" (AI_context/issues/ISSUE-0058.md) — the explicit-access
  direction itself was abandoned architecturally.
- The goal was never to replace the fault handler (which only does
  address dispatch/routing) — it was meant to synchronize chipset
  progress. That got conflated: the API's progress-sync call
  (`emu68_machine_dispatch_quantum_progress`) was gated behind the same
  `BELLATRIX_EMU68_FAULT_DRIVEN` switch used for the (separate, and
  ultimately abandoned) access-routing question, and in the process the
  ONE proven, working progress driver
  (`bellatrix_emu68_report_jit_progress`, called from `MainLoop`) got
  deleted for fault-driven builds — the current default — with nothing
  replacing it. That silently broke chipset-time advancement for any
  guest phase that doesn't touch faulting memory (RAM-only loops, since
  chip RAM is direct-MMU-mapped, never faults). This was the root cause
  of the multi-day "hangs after [JIT] Let it go" regression investigated
  in this session -- see
  ../../.claude memory bellatrix-emu68-progress-fix-2026-07-16.md
  (outside this repo) for the full writeup. Fixed by restoring the
  original unconditional call in `patches/0003`.
- Separately, when the explicit-API access mode (`public`) was tested
  directly this session, it hit its own distinct, never-diagnosed crash
  (JIT-reported "opcode 00fc not implemented" at a bogus PC) unrelated
  to the progress-driver bug above. Never root-caused before retirement.
- Patches 0025-0034 were already excluded from `scripts/setup.sh`'s
  active patch list as of commit `3f43bca` (2026-07-15) -- this session
  found and fixed the remaining active pieces (in patches 0003, 0020,
  0035, and the `src/cpu/emu68/emu68_machine_*` files) that the earlier
  partial cleanup missed.

## Worth reconsidering later

Jaime specifically flagged **cycle accounting** (`CYCLE_COUNT` in
`M68KState`, patch 0035) as potentially useful for testing purposes even
though the rest of the API is retired. See the tracking issue this
archival created.

**Update (same session, during cleanup):** patch 0035 turned out to be
empirically load-bearing, not just a nice-to-have. Removing it from the
active patch set (while keeping the 0003/0020 progress-driver fix) caused
a reproducible boot regression -- confirmed by rebuilding with 0035 removed
(regressed) then restored (fixed), deterministically, twice each. It is
**still applied** (see `scripts/setup.sh`), not archived, despite living in
this document for narrative completeness.

Root cause not confirmed. Leading theory: 0035 adds ~15 lines to
`M68k_Translator.c`'s `EmitINSN` (the JIT code-generation function), and
removing them shifts that function's compiled code layout in a way that
re-triggers the same class of bug as ISSUE-0038's x12/v28 clobber issue
(GCC using a supposedly-fixed register as prologue/epilogue scratch,
sensitive to exact frame size) -- not the `CYCLE_COUNT` accounting itself,
which is inert data unrelated to the JIT's `v30` progress counter. This is
a hypothesis, not a proven mechanism. Whoever revisits patch 0035 should
re-verify this theory (e.g. diff the compiled `EmitINSN` disassembly with
and without the patch) rather than assume it's safe to remove again.

## Timeline of related commits

- `c8599c8` (07-13 10:16) — API cluster begins
- ... (dc140f5, bf443c0, e99e611, 1b671b1, 3b331e2, a104a8e, b70ff97,
  d46abce, 4320a42, c692770, 532fcd3 — the 11 patches archived below,
  each introduced within a single 90-minute window)
- `f28b21a` (07-13 12:55) — "finalize Emu68 public machine API"
- `3f43bca` (07-15) — patches 0025-0034 already excluded from active
  setup.sh list (partial walk-back, predates this session)
- `7b4f7c9` (07-15 09:36) — "rebaseline: keep native Emu68 on core 0";
  flipped default access mode to `fault`, and excluded the STOP and
  MainLoop-progress Bellatrix paths from fault-driven builds -- the
  actual regression, fixed in this session

---

## Archived patches (0025-0035)

### `0025-emu68-machine-continuation.patch`

```diff
diff --git a/src/ExecutionLoop.c b/src/ExecutionLoop.c
--- a/src/ExecutionLoop.c
+++ b/src/ExecutionLoop.c
@@ -439,6 +474,11 @@ void MainLoop()
                 node = M68K_GetTranslationUnit(copyPC);
                 /* Load CPU context */
                 M68K_LoadContext(getCTX());
+#if !defined(BELLATRIX_EMU68_FAULT_DRIVEN) || \
+    !BELLATRIX_EMU68_FAULT_DRIVEN
+                if (emu68_machine_native_bus_error_pending())
+                    continue;
+#endif
                 asm volatile("msr TPIDR_EL1, %0"::"r"(PC));
                 /* Prepare ARM pointer in x12 and call it */
                 ARM = node->mt_ARMEntryPoint;
@@ -467,4 +506,9 @@ void MainLoop()
             M68K_LoadContext(getCTX());
+#if !defined(BELLATRIX_EMU68_FAULT_DRIVEN) || \
+    !BELLATRIX_EMU68_FAULT_DRIVEN
+            if (emu68_machine_native_bus_error_pending())
+                continue;
+#endif
             ARM = node->mt_ARMEntryPoint;
             asm volatile("":"=r"(ARM):"0"(ARM));
             CallARMCode();
```

### `0026-emu68-mmu-unmap.patch`

```diff
diff --git a/src/aarch64/mmu.c b/src/aarch64/mmu.c
index 3c89731..8447bd0 100644
--- a/src/aarch64/mmu.c
+++ b/src/aarch64/mmu.c
@@ -550,7 +550,78 @@ void mmu_map(uintptr_t phys, uintptr_t virt, uintptr_t length, uint32_t attr_low
 
 void mmu_unmap(uintptr_t virt, uintptr_t length)
 {
-    (void)virt;
-    (void)length;
     DMAP(kprintf("mmu_unmap(%p, %x)\n", virt, length));
+
+    while (length >= 4096)
+    {
+        struct mmu_page *l1;
+        struct mmu_page *l2;
+        struct mmu_page *l3;
+        uint64_t entry;
+        unsigned idx_l1 = (virt >> 30) & 0x1ff;
+        unsigned idx_l2 = (virt >> 21) & 0x1ff;
+        unsigned idx_l3 = (virt >> 12) & 0x1ff;
+
+        if (virt & 0xffff000000000000) {
+            asm volatile("mrs %0, TTBR1_EL1":"=r"(l1));
+        } else {
+            asm volatile("mrs %0, TTBR0_EL1":"=r"(l1));
+        }
+        l1 = (struct mmu_page *)((uintptr_t)l1 + PHYS_VIRT_OFFSET);
+
+        entry = l1->mp_entries[idx_l1];
+        if ((entry & 3) == 1)
+        {
+            l2 = get_4k_page();
+            for (unsigned i = 0; i < 512; ++i)
+                l2->mp_entries[i] = (entry & 0x7fc0000fff) +
+                                    ((uint64_t)i << 21);
+            l1->mp_entries[idx_l1] =
+                3 | ((uintptr_t)l2 - PHYS_VIRT_OFFSET);
+            mirror_page(virt);
+        }
+        else if ((entry & 3) == 3)
+        {
+            l2 = (struct mmu_page *)((entry & 0x7ffffff000ULL) +
+                                     PHYS_VIRT_OFFSET);
+        }
+        else
+        {
+            virt += 4096;
+            length -= 4096;
+            continue;
+        }
+
+        entry = l2->mp_entries[idx_l2];
+        if ((entry & 3) == 1)
+        {
+            l3 = get_4k_page();
+            for (unsigned i = 0; i < 512; ++i)
+                l3->mp_entries[i] =
+                    3 | ((entry & 0x7fffe00fff) + ((uint64_t)i << 12));
+            l2->mp_entries[idx_l2] =
+                3 | ((uintptr_t)l3 - PHYS_VIRT_OFFSET);
+        }
+        else if ((entry & 3) == 3)
+        {
+            l3 = (struct mmu_page *)((entry & 0x7ffffff000ULL) +
+                                     PHYS_VIRT_OFFSET);
+        }
+        else
+        {
+            virt += 4096;
+            length -= 4096;
+            continue;
+        }
+
+        l3->mp_entries[idx_l3] = 0;
+        virt += 4096;
+        length -= 4096;
+    }
+
+    asm volatile(
+"       dsb     ish                 \n"
+"       tlbi    VMALLE1IS           \n"
+"       dsb     sy                  \n"
+"       isb                         \n");
 }
 
```

### `0027-emu68-explicit-ea-access.patch`

```diff
diff --git a/src/M68k_EA.c b/src/M68k_EA.c
index 4c8ef45..ff90049 100644
--- a/src/M68k_EA.c
+++ b/src/M68k_EA.c
@@ -12,6 +12,9 @@
 #include "M68k.h"
 #include "RegisterAllocator.h"
 #include "cache.h"
+#ifdef BELLATRIX
+#include "cpu/emu68/emu68_machine_emit.h"
+#endif
 
 static inline __attribute__((always_inline)) uint32_t * load_s16_ext32(uint32_t *ptr, uint8_t reg, int16_t s16)
 {
@@ -36,6 +39,29 @@ static inline __attribute__((always_inline)) uint32_t * load_reg_from_addr_offse
         *ptr++ = mov_reg(base, 31);
     }
 
+#ifdef BELLATRIX
+    if (size != 0)
+    {
+        uint8_t address = RA_AllocARMRegister(&ptr);
+        if (offset == 0)
+            *ptr++ = mov_reg(address, base);
+        else
+        {
+            *ptr++ = movw_immed_u16(reg_d16, (uint16_t)offset);
+            *ptr++ = movt_immed_u16(reg_d16, (uint16_t)((uint32_t)offset >> 16));
+            *ptr++ = add_reg(address, base, reg_d16, LSL, 0);
+        }
+        ptr = emu68_machine_emit_load(
+            ptr, address, reg, size, sign_ext,
+            emu68_machine_translation_metadata);
+        RA_FreeARMRegister(&ptr, address);
+        RA_FreeARMRegister(&ptr, reg_d16);
+        if (free_base)
+            RA_FreeARMRegister(&ptr, base);
+        return ptr;
+    }
+#endif
+
     switch (size)
     {
         case 4:
@@ -205,6 +231,24 @@ static inline __attribute__((always_inline)) uint32_t * load_reg_from_addr(uint3
         *ptr++ = mov_reg(base, 31);
     }
 
+#ifdef BELLATRIX
+    if (size != 0)
+    {
+        uint8_t address = RA_AllocARMRegister(&ptr);
+        if (index == 0xff)
+            *ptr++ = mov_reg(address, base);
+        else
+            *ptr++ = add_reg(address, base, index, LSL, shift);
+        ptr = emu68_machine_emit_load(
+            ptr, address, reg, size, sign_ext,
+            emu68_machine_translation_metadata);
+        RA_FreeARMRegister(&ptr, address);
+        if (free_base)
+            RA_FreeARMRegister(&ptr, base);
+        return ptr;
+    }
+#endif
+
     if (index == 0xff)
     {
         switch (size)
@@ -283,6 +327,28 @@ static inline __attribute__((always_inline)) uint32_t * store_reg_to_addr_offset
         *ptr++ = mov_reg(base, 31);
     }
 
+#ifdef BELLATRIX
+    if (size != 0)
+    {
+        uint8_t address = RA_AllocARMRegister(&ptr);
+        if (offset == 0)
+            *ptr++ = mov_reg(address, base);
+        else
+        {
+            *ptr++ = movw_immed_u16(reg_d16, (uint16_t)offset);
+            *ptr++ = movt_immed_u16(reg_d16, (uint16_t)((uint32_t)offset >> 16));
+            *ptr++ = add_reg(address, base, reg_d16, LSL, 0);
+        }
+        ptr = emu68_machine_emit_store(
+            ptr, address, reg, size, emu68_machine_translation_metadata);
+        RA_FreeARMRegister(&ptr, address);
+        RA_FreeARMRegister(&ptr, reg_d16);
+        if (free_base)
+            RA_FreeARMRegister(&ptr, base);
+        return ptr;
+    }
+#endif
+
     switch (size)
     {
         case 4:
@@ -431,6 +497,23 @@ static inline __attribute__((always_inline)) uint32_t * store_reg_to_addr(uint32
         *ptr++ = mov_reg(base, 31);
     }
 
+#ifdef BELLATRIX
+    if (size != 0)
+    {
+        uint8_t address = RA_AllocARMRegister(&ptr);
+        if (index == 0xff)
+            *ptr++ = mov_reg(address, base);
+        else
+            *ptr++ = add_reg(address, base, index, LSL, shift);
+        ptr = emu68_machine_emit_store(
+            ptr, address, reg, size, emu68_machine_translation_metadata);
+        RA_FreeARMRegister(&ptr, address);
+        if (free_base)
+            RA_FreeARMRegister(&ptr, base);
+        return ptr;
+    }
+#endif
+
     if (index == 0xff)
     {
         switch (size)
@@ -633,27 +716,32 @@ uint32_t *EMIT_LoadFromEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *ar
             else
             {
                 uint8_t reg_An = RA_MapM68kRegister(&ptr, src_reg + 8);
+#ifdef BELLATRIX
+                ptr = emu68_machine_emit_load(ptr, reg_An, *arm_reg, size,
+                                              sign_ext, 0u);
+#else
                 switch (size)
                 {
                     case 4:
-                        *ptr++ = ldr_offset(reg_An, *arm_reg, 0);
+                        EMU68_GUEST_LOAD(ptr, reg_An, *arm_reg, 4, 0);
                         break;
                     case 2:
                         if (sign_ext)
-                            *ptr++ = ldrsh_offset(reg_An, *arm_reg, 0);
+                            EMU68_GUEST_LOAD(ptr, reg_An, *arm_reg, 2, 1);
                         else
-                            *ptr++ = ldrh_offset(reg_An, *arm_reg, 0);
+                            EMU68_GUEST_LOAD(ptr, reg_An, *arm_reg, 2, 0);
                         break;
                     case 1:
                         if (sign_ext)
-                            *ptr++ = ldrsb_offset(reg_An, *arm_reg, 0);
+                            EMU68_GUEST_LOAD(ptr, reg_An, *arm_reg, 1, 1);
                         else
-                            *ptr++ = ldrb_offset(reg_An, *arm_reg, 0);
+                            EMU68_GUEST_LOAD(ptr, reg_An, *arm_reg, 1, 0);
                         break;
                     default:
                         kprintf("Unknown size opcode\n");
                         break;
                 }
+#endif
             }
         }
         else if (mode == 3) /* Mode 003: (An)+ */
@@ -669,39 +757,47 @@ uint32_t *EMIT_LoadFromEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *ar
                 RA_SetDirtyM68kRegister(&ptr, 8 + src_reg);
 
                 /* Rare case where source and dest are the same register and size == 4 */
+#ifdef BELLATRIX
+                ptr = emu68_machine_emit_load(ptr, reg_An, *arm_reg, size,
+                                              sign_ext, 0u);
+                if (!(size == 4 && reg_An == *arm_reg))
+                    *ptr++ = add_immed(reg_An, reg_An,
+                                       (size == 1 && src_reg == 7) ? 2 : size);
+#else
                 if (size == 4 && reg_An == *arm_reg) {
-                    *ptr++ = ldr_offset(reg_An, *arm_reg, 0);
+                    EMU68_GUEST_LOAD(ptr, reg_An, *arm_reg, 4, 0);
                 }
                 else
                 {
                     switch (size)
                     {
                         case 4:
-                            *ptr++ = ldr_offset_postindex(reg_An, *arm_reg, 4);
+                            EMU68_GUEST_POSTLOAD(ptr, reg_An, *arm_reg, 4, 0, 4);
                             break;
                         case 2:
                             if (sign_ext)
-                                *ptr++ = ldrsh_offset_postindex(reg_An, *arm_reg, 2);
+                                EMU68_GUEST_POSTLOAD(ptr, reg_An, *arm_reg, 2, 1, 2);
                             else
-                                *ptr++ = ldrh_offset_postindex(reg_An, *arm_reg, 2);
+                                EMU68_GUEST_POSTLOAD(ptr, reg_An, *arm_reg, 2, 0, 2);
                             break;
                         case 1:
                             if (src_reg == 7)
                                 if (sign_ext)
-                                    *ptr++ = ldrsb_offset_postindex(reg_An, *arm_reg, 2);
+                                    EMU68_GUEST_POSTLOAD(ptr, reg_An, *arm_reg, 1, 1, 2);
                                 else
-                                    *ptr++ = ldrb_offset_postindex(reg_An, *arm_reg, 2);
+                                    EMU68_GUEST_POSTLOAD(ptr, reg_An, *arm_reg, 1, 0, 2);
                             else
                                 if (sign_ext)
-                                    *ptr++ = ldrsb_offset_postindex(reg_An, *arm_reg, 1);
+                                    EMU68_GUEST_POSTLOAD(ptr, reg_An, *arm_reg, 1, 1, 1);
                                 else
-                                    *ptr++ = ldrb_offset_postindex(reg_An, *arm_reg, 1);
+                                    EMU68_GUEST_POSTLOAD(ptr, reg_An, *arm_reg, 1, 0, 1);
                             break;
                         default:
                             kprintf("Unknown size opcode\n");
                             break;
                     }
                 }
+#endif
             }
         }
         else if (mode == 4) /* Mode 004: -(An) */
@@ -716,39 +812,52 @@ uint32_t *EMIT_LoadFromEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *ar
                 RA_SetDirtyM68kRegister(&ptr, 8 + src_reg);
 
                 /* Rare case where source and dest are the same register and size == 4 */
+#ifdef BELLATRIX
+                if (size == 4 && reg_An == *arm_reg)
+                    ptr = load_reg_from_addr_offset(ptr, size, reg_An,
+                                                    *arm_reg, -4, 0, sign_ext);
+                else
+                {
+                    *ptr++ = sub_immed(reg_An, reg_An,
+                                       (size == 1 && src_reg == 7) ? 2 : size);
+                    ptr = emu68_machine_emit_load(ptr, reg_An, *arm_reg, size,
+                                                  sign_ext, 0u);
+                }
+#else
                 if (size == 4 && reg_An == *arm_reg) {
-                    *ptr++ = ldur_offset(reg_An, *arm_reg, -4);
+                    EMU68_GUEST_LOAD_OFFSET(ptr, reg_An, -4, *arm_reg, 4, 0);
                 }
                 else
                 {
                     switch (size)
                     {
                         case 4:
-                            *ptr++ = ldr_offset_preindex(reg_An, *arm_reg, -4);
+                            EMU68_GUEST_PRELOAD(ptr, reg_An, *arm_reg, 4, 0, -4);
                             break;
                         case 2:
                             if (sign_ext)
-                                *ptr++ = ldrsh_offset_preindex(reg_An, *arm_reg, -2);
+                                EMU68_GUEST_PRELOAD(ptr, reg_An, *arm_reg, 2, 1, -2);
                             else
-                                *ptr++ = ldrh_offset_preindex(reg_An, *arm_reg, -2);
+                                EMU68_GUEST_PRELOAD(ptr, reg_An, *arm_reg, 2, 0, -2);
                             break;
                         case 1:
                             if (src_reg == 7)
                                 if (sign_ext)
-                                    *ptr++ = ldrsb_offset_preindex(reg_An, *arm_reg, -2);
+                                    EMU68_GUEST_PRELOAD(ptr, reg_An, *arm_reg, 1, 1, -2);
                                 else
-                                    *ptr++ = ldrb_offset_preindex(reg_An, *arm_reg, -2);
+                                    EMU68_GUEST_PRELOAD(ptr, reg_An, *arm_reg, 1, 0, -2);
                             else
                                 if (sign_ext)
-                                    *ptr++ = ldrsb_offset_preindex(reg_An, *arm_reg, -1);
+                                    EMU68_GUEST_PRELOAD(ptr, reg_An, *arm_reg, 1, 1, -1);
                                 else
-                                    *ptr++ = ldrb_offset_preindex(reg_An, *arm_reg, -1);
+                                    EMU68_GUEST_PRELOAD(ptr, reg_An, *arm_reg, 1, 0, -1);
                             break;
                         default:
                             kprintf("Unknown size opcode\n");
                             break;
                     }
                 }
+#endif
             }
         }
         else if (mode == 5) /* Mode 005: (d16, An) */
@@ -932,9 +1041,9 @@ uint32_t *EMIT_LoadFromEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *ar
                     {
                         /* Fetch data from base reg */
                         if (base_reg == 0xff)
-                            *ptr++ = ldr_offset(bd_reg, bd_reg, 0);
+                            EMU68_GUEST_LOAD(ptr, bd_reg, bd_reg, 4, 0);
                         else
-                            *ptr++ = ldr_regoffset(bd_reg, bd_reg, base_reg, UXTW, 0);
+                            ptr = load_reg_from_addr(ptr, 4, bd_reg, bd_reg, base_reg, 0, 0);
                         
                         if (outer_reg != 0xff)
                             *ptr++ = add_reg(bd_reg, bd_reg, outer_reg, LSL, 0);
@@ -948,7 +1057,7 @@ uint32_t *EMIT_LoadFromEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *ar
                         {
                             if (bd_reg == 0xff) {
                                 bd_reg = RA_AllocARMRegister(&ptr);
-                                *ptr++ = ldr_offset(base_reg, bd_reg, 0);
+                                EMU68_GUEST_LOAD(ptr, base_reg, bd_reg, 4, 0);
                             }
                             else
                                 ptr = load_reg_from_addr(ptr, 4, base_reg, bd_reg, bd_reg, 0, 0);
@@ -1103,16 +1212,16 @@ uint32_t *EMIT_LoadFromEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *ar
                     {
                         case 2: /* Word displacement */
                             bd_reg = RA_AllocARMRegister(&ptr);
-                            *ptr++ = ldrsh_offset(REG_PC, bd_reg, pc_off);
+                            EMU68_GUEST_LOAD_OFFSET_META(
+                                ptr, REG_PC, pc_off, bd_reg, 2, 1,
+                                EMU68_MACHINE_META_PROGRAM);
                             (*ext_words)++;
                             break;
                         case 3: /* Long displacement */
                             bd_reg = RA_AllocARMRegister(&ptr);
-                            if (pc_off & 2) {
-                                *ptr++ = ldur_offset(REG_PC, bd_reg, pc_off);
-                            } else {
-                                *ptr++ = ldr_offset(REG_PC, bd_reg, pc_off);
-                            }
+                            EMU68_GUEST_LOAD_OFFSET_META(
+                                ptr, REG_PC, pc_off, bd_reg, 4, 0,
+                                EMU68_MACHINE_META_PROGRAM);
                             (*ext_words) += 2;
                             break;
                     }
@@ -1125,16 +1234,16 @@ uint32_t *EMIT_LoadFromEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *ar
                     {
                         case 2: /* Word outer displacement */
                             outer_reg = RA_AllocARMRegister(&ptr);
-                            *ptr++ = ldrsh_offset(REG_PC, outer_reg, pc_off);
+                            EMU68_GUEST_LOAD_OFFSET_META(
+                                ptr, REG_PC, pc_off, outer_reg, 2, 1,
+                                EMU68_MACHINE_META_PROGRAM);
                             (*ext_words)++;
                             break;
                         case 3: /* Long outer displacement */
                             outer_reg = RA_AllocARMRegister(&ptr);
-                            if (pc_off & 2) {
-                                *ptr++ = ldur_offset(REG_PC, outer_reg, pc_off);
-                            }
-                            else
-                                *ptr++ = ldr_offset(REG_PC, outer_reg, pc_off);
+                            EMU68_GUEST_LOAD_OFFSET_META(
+                                ptr, REG_PC, pc_off, outer_reg, 4, 0,
+                                EMU68_MACHINE_META_PROGRAM);
                             (*ext_words) += 2;
                             break;
                     }
@@ -1174,9 +1283,9 @@ uint32_t *EMIT_LoadFromEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *ar
                         {
                             /* Fetch data from base reg */
                             if (base_reg == 0xff)
-                                *ptr++ = ldr_offset(bd_reg, bd_reg, 0);
+                                EMU68_GUEST_LOAD(ptr, bd_reg, bd_reg, 4, 0);
                             else
-                                *ptr++ = ldr_regoffset(bd_reg, bd_reg, base_reg, UXTW, 0);
+                                ptr = load_reg_from_addr(ptr, 4, bd_reg, bd_reg, base_reg, 0, 0);
 
                             if (outer_reg != 0xff)
                                 *ptr++ = add_reg(bd_reg, bd_reg, outer_reg, LSL, 0);
@@ -1190,18 +1299,18 @@ uint32_t *EMIT_LoadFromEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *ar
                             {
                                 if (bd_reg == 0xff) {
                                     bd_reg = RA_AllocARMRegister(&ptr);
-                                    *ptr++ = ldr_offset(base_reg, bd_reg, 0);
+                                    EMU68_GUEST_LOAD(ptr, base_reg, bd_reg, 4, 0);
                                 }
                                 else
                                 {
                                     if (base_reg == 0xff) {
                                         uint8_t t = RA_AllocARMRegister(&ptr);
                                         *ptr++ = mov_reg(t, 31);
-                                        *ptr++ = ldr_regoffset(t, bd_reg, bd_reg, UXTW, 0);
+                                        ptr = load_reg_from_addr(ptr, 4, t, bd_reg, bd_reg, 0, 0);
                                         RA_FreeARMRegister(&ptr, t);
                                     }
                                     else {
-                                        *ptr++ = ldr_regoffset(base_reg, bd_reg, bd_reg, UXTW, 0);
+                                        ptr = load_reg_from_addr(ptr, 4, base_reg, bd_reg, bd_reg, 0, 0);
                                     }
                                 }
                             }
@@ -1292,19 +1401,15 @@ uint32_t *EMIT_LoadFromEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *ar
                     switch (size)
                     {
                         case 4:
-                            *ptr++ = ldr_offset(tmp_reg, *arm_reg, 0);
+                            EMU68_GUEST_LOAD(ptr, tmp_reg, *arm_reg, 4, 0);
                             break;
                         case 2:
-                            if (sign_ext)
-                                *ptr++ = ldrsh_offset(tmp_reg, *arm_reg, 0);
-                            else
-                                *ptr++ = ldrh_offset(tmp_reg, *arm_reg, 0);
+                            EMU68_GUEST_LOAD(ptr, tmp_reg, *arm_reg, 2,
+                                            sign_ext);
                             break;
                         case 1:
-                            if (sign_ext)
-                                *ptr++ = ldrsb_offset(tmp_reg, *arm_reg, 0);
-                            else
-                                *ptr++ = ldrb_offset(tmp_reg, *arm_reg, 0);
+                            EMU68_GUEST_LOAD(ptr, tmp_reg, *arm_reg, 1,
+                                            sign_ext);
                             break;
                     }
                     RA_FreeARMRegister(&ptr, tmp_reg);
@@ -1490,24 +1595,30 @@ uint32_t *EMIT_StoreToEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *arm
 
                 RA_SetDirtyM68kRegister(&ptr, 8 + src_reg);
 
+#ifdef BELLATRIX
+                ptr = emu68_machine_emit_store(ptr, reg_An, *arm_reg, size, 0u);
+                *ptr++ = add_immed(reg_An, reg_An,
+                                   (size == 1 && src_reg == 7) ? 2 : size);
+#else
                 switch (size)
                 {
                 case 4:
-                    *ptr++ = str_offset_postindex(reg_An, *arm_reg, 4);
+                    EMU68_GUEST_POSTSTORE(ptr, reg_An, *arm_reg, 4, 4);
                     break;
                 case 2:
-                    *ptr++ = strh_offset_postindex(reg_An, *arm_reg, 2);
+                    EMU68_GUEST_POSTSTORE(ptr, reg_An, *arm_reg, 2, 2);
                     break;
                 case 1:
                     if (src_reg == 7)
-                        *ptr++ = strb_offset_postindex(reg_An, *arm_reg, 2);
+                        EMU68_GUEST_POSTSTORE(ptr, reg_An, *arm_reg, 1, 2);
                     else
-                        *ptr++ = strb_offset_postindex(reg_An, *arm_reg, 1);
+                        EMU68_GUEST_POSTSTORE(ptr, reg_An, *arm_reg, 1, 1);
                     break;
                 default:
                     kprintf("Unknown size opcode\n");
                     break;
                 }
+#endif
             }
         }
         else if (mode == 4) /* Mode 004: -(An) */
@@ -1522,24 +1633,30 @@ uint32_t *EMIT_StoreToEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *arm
 
                 RA_SetDirtyM68kRegister(&ptr, 8 + src_reg);
 
+#ifdef BELLATRIX
+                *ptr++ = sub_immed(reg_An, reg_An,
+                                   (size == 1 && src_reg == 7) ? 2 : size);
+                ptr = emu68_machine_emit_store(ptr, reg_An, *arm_reg, size, 0u);
+#else
                 switch (size)
                 {
                 case 4:
-                    *ptr++ = str_offset_preindex(reg_An, *arm_reg, -4);
+                    EMU68_GUEST_PRESTORE(ptr, reg_An, *arm_reg, 4, -4);
                     break;
                 case 2:
-                    *ptr++ = strh_offset_preindex(reg_An, *arm_reg, -2);
+                    EMU68_GUEST_PRESTORE(ptr, reg_An, *arm_reg, 2, -2);
                     break;
                 case 1:
                     if (src_reg == 7)
-                        *ptr++ = strb_offset_preindex(reg_An, *arm_reg, -2);
+                        EMU68_GUEST_PRESTORE(ptr, reg_An, *arm_reg, 1, -2);
                     else
-                        *ptr++ = strb_offset_preindex(reg_An, *arm_reg, -1);
+                        EMU68_GUEST_PRESTORE(ptr, reg_An, *arm_reg, 1, -1);
                     break;
                 default:
                     kprintf("Unknown size opcode\n");
                     break;
                 }
+#endif
             }
         }
         else if (mode == 5) /* Mode 005: (d16, An) */
@@ -1711,9 +1828,9 @@ uint32_t *EMIT_StoreToEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *arm
                     {
                         /* Fetch data from base reg */
                         if (base_reg == 0xff)
-                            *ptr++ = ldr_offset(bd_reg, bd_reg, 0);
+                            EMU68_GUEST_LOAD(ptr, bd_reg, bd_reg, 4, 0);
                         else
-                            *ptr++ = ldr_regoffset(bd_reg, bd_reg, base_reg, UXTW, 0);
+                            ptr = load_reg_from_addr(ptr, 4, bd_reg, bd_reg, base_reg, 0, 0);
                         
                         if (outer_reg != 0xff)
                             *ptr++ = add_reg(bd_reg, bd_reg, outer_reg, LSL, 0);
@@ -1728,18 +1845,18 @@ uint32_t *EMIT_StoreToEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *arm
                             if (bd_reg == 0xff)
                             {
                                 bd_reg = RA_AllocARMRegister(&ptr);
-                                *ptr++ = ldr_offset(base_reg, bd_reg, 0);
+                                EMU68_GUEST_LOAD(ptr, base_reg, bd_reg, 4, 0);
                             }
                             else
                             {
                                 if (base_reg == 0xff) {
                                     uint8_t t = RA_AllocARMRegister(&ptr);
                                     *ptr++ = mov_reg(t, 31);
-                                    *ptr++ = ldr_regoffset(t, bd_reg, bd_reg, UXTW, 0);
+                                    ptr = load_reg_from_addr(ptr, 4, t, bd_reg, bd_reg, 0, 0);
                                     RA_FreeARMRegister(&ptr, t);
                                 }
                                 else
-                                    *ptr++ = ldr_regoffset(base_reg, bd_reg, bd_reg, UXTW, 0);
+                                    ptr = load_reg_from_addr(ptr, 4, base_reg, bd_reg, bd_reg, 0, 0);
                             }
                         }
                         else
@@ -1948,7 +2065,7 @@ uint32_t *EMIT_StoreToEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *arm
                         {
                             /* Fetch data from base reg */
                             if (base_reg == 0xff)
-                                *ptr++ = ldr_offset(bd_reg, bd_reg, 0);
+                                EMU68_GUEST_LOAD(ptr, bd_reg, bd_reg, 4, 0);
                             else
                                 ptr = load_reg_from_addr(ptr, 4, bd_reg, bd_reg, base_reg, 0, 0);
 
@@ -1964,7 +2081,7 @@ uint32_t *EMIT_StoreToEffectiveAddress(uint32_t *ptr, uint8_t size, uint8_t *arm
                             {
                                 if (bd_reg == 0xff) {
                                     bd_reg = RA_AllocARMRegister(&ptr);
-                                    *ptr++ = ldr_offset(base_reg, bd_reg, 0);
+                                    EMU68_GUEST_LOAD(ptr, base_reg, bd_reg, 4, 0);
                                 }
                                 else
                                     ptr = load_reg_from_addr(ptr, 4, base_reg, bd_reg, bd_reg, 0, 0);
```

### `0028-emu68-explicit-fetch-and-exception.patch`

```diff
diff --git a/src/M68k_Exception.c b/src/M68k_Exception.c
index cb13256..034d347 100644
--- a/src/M68k_Exception.c
+++ b/src/M68k_Exception.c
@@ -12,6 +12,9 @@
 #include "support.h"
 #include "M68k.h"
 #include "RegisterAllocator.h"
+#ifdef BELLATRIX
+#include "cpu/emu68/emu68_machine_emit.h"
+#endif
 
 uint32_t *EMIT_Exception(uint32_t *ptr, uint16_t exception, uint8_t format, ...)
 {
@@ -70,7 +73,12 @@ uint32_t *EMIT_Exception(uint32_t *ptr, uint16_t exception, uint8_t format, ...)
         *ptr++ = movw_immed_u16(vbr, ea & 0xffff);
         if ((ea >> 16) != 0)
             *ptr++ = movt_immed_u16(vbr, ea >> 16);
+#ifdef BELLATRIX
+        *ptr++ = sub_immed(sp, sp, 4);
+        ptr = emu68_machine_emit_store(ptr, sp, vbr, 4, 0u);
+#else
         *ptr++ = str_offset_preindex(sp, vbr, -4);
+#endif
     }
     else if (format == 4)
     {
@@ -81,12 +89,21 @@ uint32_t *EMIT_Exception(uint32_t *ptr, uint16_t exception, uint8_t format, ...)
         *ptr++ = movw_immed_u16(vbr, fault & 0xffff);
         if ((fault >> 16) != 0)
             *ptr++ = movt_immed_u16(vbr, fault >> 16);
+#ifdef BELLATRIX
+        *ptr++ = sub_immed(sp, sp, 8);
+        ptr = emu68_machine_emit_store(ptr, sp, vbr, 4, 0u);
+#else
         *ptr++ = str_offset_preindex(sp, vbr, -8);
+#endif
 
         *ptr++ = movw_immed_u16(vbr, ea & 0xffff);
         if ((ea >> 16) != 0)
             *ptr++ = movt_immed_u16(vbr, ea >> 16);
+#ifdef BELLATRIX
+        ptr = emu68_machine_emit_store_offset(ptr, sp, 4, vbr, 4, 0u);
+#else
         *ptr++ = str_offset(sp, vbr, 4);
+#endif
     }
 
     uint8_t cc_copy = RA_AllocARMRegister(&ptr);
@@ -95,16 +112,29 @@ uint32_t *EMIT_Exception(uint32_t *ptr, uint16_t exception, uint8_t format, ...)
     *ptr++ = rbit(vbr, cc);
     *ptr++ = bfxil(cc_copy, vbr, 30, 2);
     /* Store SR */
+#ifdef BELLATRIX
+    *ptr++ = sub_immed(sp, sp, 8);
+    ptr = emu68_machine_emit_store(ptr, sp, cc_copy, 2, 0u);
+#else
     *ptr++ = strh_offset_preindex(sp, cc_copy, -8);
+#endif
 
     RA_FreeARMRegister(&ptr, cc_copy);
 
     /* Store program counter */
+#ifdef BELLATRIX
+    ptr = emu68_machine_emit_store_offset(ptr, sp, 2, REG_PC, 4, 0u);
+#else
     *ptr++ = stur_offset(sp, REG_PC, 2);
+#endif
 
     /* Store exception vector and type */
     *ptr++ = mov_immed_u16(vbr, (format << 12) | (exception & 0xfff), 0);
+#ifdef BELLATRIX
+    ptr = emu68_machine_emit_store_offset(ptr, sp, 6, vbr, 2, 0u);
+#else
     *ptr++ = strh_offset(sp, vbr, 6);
+#endif
 
     /* Clear trace flags, set supervisor */
     *ptr++ = bic_immed(cc, cc, 2, 32 - SRB_T0);
@@ -112,7 +142,12 @@ uint32_t *EMIT_Exception(uint32_t *ptr, uint16_t exception, uint8_t format, ...)
 
     /* Load VBR */
     *ptr++ = ldr_offset(ctx, vbr, __builtin_offsetof(struct M68KState, VBR));
+#ifdef BELLATRIX
+    ptr = emu68_machine_emit_load_offset(ptr, vbr, exception, REG_PC, 4,
+                                         0, 0u);
+#else
     *ptr++ = ldr_offset(vbr, REG_PC, exception);
+#endif
 
     RA_FreeARMRegister(&ptr, vbr);
     
diff --git a/src/M68k_Translator.c b/src/M68k_Translator.c
index c95ee46..672e354 100644
--- a/src/M68k_Translator.c
+++ b/src/M68k_Translator.c
@@ -19,6 +19,10 @@
 #include "DuffCopy.h"
 #include "disasm.h"
 #include "cache.h"
+#ifdef BELLATRIX
+#include "cpu/emu68/emu68_machine_emit.h"
+#include "cpu/emu68/emu68_machine_internal.h"
+#endif
 
 #if SET_FEATURES_AT_RUNTIME
 features_t Features;
@@ -177,7 +181,13 @@ static inline uint32_t *EmitINSN(uint32_t *arm_ptr, uint16_t **m68k_ptr, uint16_
         {
             int8_t off = 0;
             ptr = EMIT_GetOffsetPC(ptr, &off);
+#ifdef BELLATRIX
+            ptr = emu68_machine_emit_load_offset(
+                ptr, REG_PC, off, 0, 2, 0,
+                EMU68_SPACE_PROGRAM << EMU68_BRIDGE_META_SPACE_SHIFT);
+#else
             *ptr++ = ldurh_offset(REG_PC, 0, off);
+#endif
         }
     }
 
diff --git a/src/cache.c b/src/cache.c
index 3e5b953..af95c51 100644
--- a/src/cache.c
+++ b/src/cache.c
@@ -2,6 +2,18 @@
 #include "tlsf.h"
 #include "support.h"
 #include "cache.h"
+#ifdef BELLATRIX
+extern int emu68_machine_runtime_active(void);
+extern int emu68_machine_instruction_fetch_allowed(uint32_t, uint8_t);
+#define CHECK_MACHINE_FETCH(type, address, width)                            \
+    do {                                                                      \
+        if ((type) == ICACHE && emu68_machine_runtime_active() &&             \
+            !emu68_machine_instruction_fetch_allowed((address), (width)))     \
+            return 0;                                                         \
+    } while (0)
+#else
+#define CHECK_MACHINE_FETCH(type, address, width) do { } while (0)
+#endif
 //#include "ps_protocol.h"
 #include "M68k.h"
 #include <stdint.h>
@@ -294,6 +306,7 @@ uint128_t cache_read_128(enum CacheType type, uint32_t address)
 
 uint64_t cache_read_64(enum CacheType type, uint32_t address)
 {
+    CHECK_MACHINE_FETCH(type, address, 8u);
     struct Cache *cache = (type == ICACHE) ? IC : DC;
     const uint32_t tag = GET_TAG(address);
     const uint32_t set = GET_SET(address);
@@ -425,6 +438,7 @@ uint64_t cache_read_64(enum CacheType type, uint32_t address)
 
 uint32_t cache_read_32(enum CacheType type, uint32_t address)
 {
+    CHECK_MACHINE_FETCH(type, address, 4u);
     struct Cache *cache = (type == ICACHE) ? IC : DC;
     const uint32_t tag = GET_TAG(address);
     const uint32_t set = GET_SET(address);
@@ -533,6 +547,7 @@ uint32_t cache_read_32(enum CacheType type, uint32_t address)
 
 uint16_t cache_read_16(enum CacheType type, uint32_t address)
 {
+    CHECK_MACHINE_FETCH(type, address, 2u);
     struct Cache *cache = (type == ICACHE) ? IC : DC;
     const uint32_t tag = GET_TAG(address);
     const uint32_t set = GET_SET(address);
@@ -626,6 +641,7 @@ uint16_t cache_read_16(enum CacheType type, uint32_t address)
 
 uint8_t cache_read_8(enum CacheType type, uint32_t address)
 {
+    CHECK_MACHINE_FETCH(type, address, 1u);
     struct Cache *cache = (type == ICACHE) ? IC : DC;
     const uint32_t tag = GET_TAG(address);
     const uint32_t set = GET_SET(address);
@@ -1281,4 +1297,3 @@ int cache_write_8(enum CacheType type, uint32_t address, uint8_t data, uint8_t w
 
     return 1;
 }
-

```

### `0029-emu68-explicit-special-access.patch`

```diff
diff --git a/include/M68k.h b/include/M68k.h
index 7372256..41cc7c0 100644
--- a/include/M68k.h
+++ b/include/M68k.h
@@ -17,6 +17,155 @@
 #include "md5.h"
 #include "lists.h"
 
+#ifdef BELLATRIX
+#include "cpu/emu68/emu68_machine_emit.h"
+#define BELLATRIX_MACHINE_API_ACTIVE 1
+#define EMU68_GUEST_LOAD(ptr, address, value, width, sign_extend)             \
+    do {                                                                      \
+        (ptr) = emu68_machine_emit_load((ptr), (address), (value), (width),   \
+                                        (sign_extend),                       \
+                                        emu68_machine_translation_metadata); \
+    } while (0)
+#define EMU68_GUEST_STORE(ptr, address, value, width)                         \
+    do {                                                                      \
+        (ptr) = emu68_machine_emit_store((ptr), (address), (value), (width), \
+                                         emu68_machine_translation_metadata); \
+    } while (0)
+#define EMU68_GUEST_LOAD_OFFSET(ptr, base, offset, value, width, sign_extend) \
+    do {                                                                      \
+        (ptr) = emu68_machine_emit_load_offset(                              \
+            (ptr), (base), (offset), (value), (width), (sign_extend),        \
+            emu68_machine_translation_metadata);                             \
+    } while (0)
+#define EMU68_GUEST_STORE_OFFSET(ptr, base, offset, value, width)             \
+    do {                                                                      \
+        (ptr) = emu68_machine_emit_store_offset(                             \
+            (ptr), (base), (offset), (value), (width),                       \
+            emu68_machine_translation_metadata);                             \
+    } while (0)
+#define EMU68_GUEST_LOAD_OFFSET_META(ptr, base, offset, value, width,         \
+                                     sign_extend, metadata)                  \
+    do {                                                                      \
+        (ptr) = emu68_machine_emit_load_offset(                              \
+            (ptr), (base), (offset), (value), (width), (sign_extend),        \
+            (metadata));                                                      \
+    } while (0)
+#else
+#define EMU68_MACHINE_META_PROGRAM 0u
+#define EMU68_MACHINE_META_CPU 0u
+#define BELLATRIX_MACHINE_API_ACTIVE 0
+#define EMU68_GUEST_LOAD(ptr, address, value, width, sign_extend)             \
+    do {                                                                      \
+        if ((width) == 4)                                                     \
+            *(ptr)++ = ldr_offset((address), (value), 0);                    \
+        else if ((width) == 2)                                                \
+            *(ptr)++ = (sign_extend) ?                                       \
+                ldrsh_offset((address), (value), 0) :                         \
+                ldrh_offset((address), (value), 0);                           \
+        else                                                                  \
+            *(ptr)++ = (sign_extend) ?                                       \
+                ldrsb_offset((address), (value), 0) :                         \
+                ldrb_offset((address), (value), 0);                           \
+    } while (0)
+#define EMU68_GUEST_STORE(ptr, address, value, width)                         \
+    do {                                                                      \
+        if ((width) == 4)                                                     \
+            *(ptr)++ = str_offset((address), (value), 0);                    \
+        else if ((width) == 2)                                                \
+            *(ptr)++ = strh_offset((address), (value), 0);                   \
+        else                                                                  \
+            *(ptr)++ = strb_offset((address), (value), 0);                   \
+    } while (0)
+#define EMU68_GUEST_LOAD_OFFSET(ptr, base, offset, value, width, sign_extend) \
+    do {                                                                      \
+        if ((width) == 4)                                                     \
+            *(ptr)++ = ldr_offset((base), (value), (offset));                \
+        else if ((width) == 2)                                                \
+            *(ptr)++ = (sign_extend) ?                                       \
+                ldrsh_offset((base), (value), (offset)) :                     \
+                ldrh_offset((base), (value), (offset));                       \
+        else                                                                  \
+            *(ptr)++ = (sign_extend) ?                                       \
+                ldrsb_offset((base), (value), (offset)) :                     \
+                ldrb_offset((base), (value), (offset));                       \
+    } while (0)
+#define EMU68_GUEST_STORE_OFFSET(ptr, base, offset, value, width)             \
+    do {                                                                      \
+        if ((width) == 4)                                                     \
+            *(ptr)++ = str_offset((base), (value), (offset));                \
+        else if ((width) == 2)                                                \
+            *(ptr)++ = strh_offset((base), (value), (offset));               \
+        else                                                                  \
+            *(ptr)++ = strb_offset((base), (value), (offset));               \
+    } while (0)
+#define EMU68_GUEST_LOAD_OFFSET_META(ptr, base, offset, value, width,         \
+                                     sign_extend, metadata)                  \
+    EMU68_GUEST_LOAD_OFFSET((ptr), (base), (offset), (value), (width),       \
+                            (sign_extend))
+#endif
+
+#define EMU68_GUEST_PRELOAD(ptr, address, value, width, sign_extend, delta)   \
+    do {                                                                      \
+        if (BELLATRIX_MACHINE_API_ACTIVE) {                                   \
+            *(ptr)++ = sub_immed((address), (address), (uint16_t)-(delta));  \
+            EMU68_GUEST_LOAD((ptr), (address), (value), (width),             \
+                             (sign_extend));                                  \
+        } else if ((width) == 4)                                              \
+            *(ptr)++ = ldr_offset_preindex((address), (value), (delta));     \
+        else if ((width) == 2)                                                \
+            *(ptr)++ = (sign_extend) ?                                       \
+                ldrsh_offset_preindex((address), (value), (delta)) :          \
+                ldrh_offset_preindex((address), (value), (delta));            \
+        else                                                                  \
+            *(ptr)++ = (sign_extend) ?                                       \
+                ldrsb_offset_preindex((address), (value), (delta)) :          \
+                ldrb_offset_preindex((address), (value), (delta));            \
+    } while (0)
+
+#define EMU68_GUEST_POSTLOAD(ptr, address, value, width, sign_extend, delta)  \
+    do {                                                                      \
+        if (BELLATRIX_MACHINE_API_ACTIVE) {                                   \
+            EMU68_GUEST_LOAD((ptr), (address), (value), (width),             \
+                             (sign_extend));                                  \
+            *(ptr)++ = add_immed((address), (address), (delta));             \
+        } else if ((width) == 4)                                              \
+            *(ptr)++ = ldr_offset_postindex((address), (value), (delta));    \
+        else if ((width) == 2)                                                \
+            *(ptr)++ = (sign_extend) ?                                       \
+                ldrsh_offset_postindex((address), (value), (delta)) :         \
+                ldrh_offset_postindex((address), (value), (delta));           \
+        else                                                                  \
+            *(ptr)++ = (sign_extend) ?                                       \
+                ldrsb_offset_postindex((address), (value), (delta)) :         \
+                ldrb_offset_postindex((address), (value), (delta));           \
+    } while (0)
+
+#define EMU68_GUEST_PRESTORE(ptr, address, value, width, delta)               \
+    do {                                                                      \
+        if (BELLATRIX_MACHINE_API_ACTIVE) {                                   \
+            *(ptr)++ = sub_immed((address), (address), (uint16_t)-(delta));  \
+            EMU68_GUEST_STORE((ptr), (address), (value), (width));           \
+        } else if ((width) == 4)                                              \
+            *(ptr)++ = str_offset_preindex((address), (value), (delta));     \
+        else if ((width) == 2)                                                \
+            *(ptr)++ = strh_offset_preindex((address), (value), (delta));    \
+        else                                                                  \
+            *(ptr)++ = strb_offset_preindex((address), (value), (delta));    \
+    } while (0)
+
+#define EMU68_GUEST_POSTSTORE(ptr, address, value, width, delta)              \
+    do {                                                                      \
+        if (BELLATRIX_MACHINE_API_ACTIVE) {                                   \
+            EMU68_GUEST_STORE((ptr), (address), (value), (width));           \
+            *(ptr)++ = add_immed((address), (address), (delta));             \
+        } else if ((width) == 4)                                              \
+            *(ptr)++ = str_offset_postindex((address), (value), (delta));    \
+        else if ((width) == 2)                                                \
+            *(ptr)++ = strh_offset_postindex((address), (value), (delta));   \
+        else                                                                  \
+            *(ptr)++ = strb_offset_postindex((address), (value), (delta));   \
+    } while (0)
+
 struct M68KLocalState {
     void *          mls_M68kPtr;
     uint32_t        mls_ARMOffset;
diff --git a/src/M68k_LINE5.c b/src/M68k_LINE5.c
index 205bcd1..5eca14f 100644
--- a/src/M68k_LINE5.c
+++ b/src/M68k_LINE5.c
@@ -117,11 +117,12 @@ uint32_t *EMIT_ADDQ(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             case 0: /* 8-bit */
                 if (mode == 4)
                 {
-                    *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                        (opcode & 7) == 7 ? -2 : -1);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldrb_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
                 
                 /* Perform calcualtion */
                 if (update_mask == 0 || update_mask == SR_Z || update_mask == SR_N) {
@@ -146,21 +147,22 @@ uint32_t *EMIT_ADDQ(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = strb_offset_postindex(dest, tmp, (opcode & 7) == 7 ? 2 : 1);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 1,
+                                         (opcode & 7) == 7 ? 2 : 1);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = strb_offset(dest, tmp, 0);
+                    EMU68_GUEST_STORE(ptr, dest, tmp, 1);
                 break;
             
             case 1: /* 16-bit */
                 if (mode == 4)
                 {
-                    *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldrh_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
 
                 /* Perform calcualtion */
                 if (update_mask == 0 || update_mask == SR_Z || update_mask == SR_N) {
@@ -185,21 +187,21 @@ uint32_t *EMIT_ADDQ(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = strh_offset_postindex(dest, tmp, 2);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = strh_offset(dest, tmp, 0);
+                    EMU68_GUEST_STORE(ptr, dest, tmp, 2);
                 break;
 
             case 2: /* 32-bit */
                 if (mode == 4)
                 {
-                    *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldr_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
                 /* Perform calcualtion */
                 *ptr++ = adds_immed(tmp, tmp, data);
@@ -207,11 +209,11 @@ uint32_t *EMIT_ADDQ(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = str_offset_postindex(dest, tmp, 4);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 4, 4);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = str_offset(dest, tmp, 0);
+                    EMU68_GUEST_STORE(ptr, dest, tmp, 4);
                 break;
         }
 
@@ -380,11 +382,12 @@ uint32_t *EMIT_SUBQ(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         case 0: /* 8-bit */
             if (mode == 4)
             {
-                *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrb_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
             
             /* Perform calcualtion */
             if (update_mask == 0 || update_mask == SR_Z || update_mask == SR_N) {
@@ -410,21 +413,22 @@ uint32_t *EMIT_SUBQ(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strb_offset_postindex(dest, tmp, (opcode & 7) == 7 ? 2 : 1);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strb_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 1);
             break;
         
         case 1: /* 16-bit */
             if (mode == 4)
             {
-                *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrh_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
 
             /* Perform calcualtion */
             if (update_mask == 0 || update_mask == SR_Z || update_mask == SR_N) {
@@ -450,21 +454,21 @@ uint32_t *EMIT_SUBQ(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strh_offset_postindex(dest, tmp, 2);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strh_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 2);
             break;
 
         case 2: /* 32-bit */
             if (mode == 4)
             {
-                *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldr_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
             /* Perform calcualtion */
             *ptr++ = subs_immed(tmp, tmp, data);
@@ -472,11 +476,11 @@ uint32_t *EMIT_SUBQ(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = str_offset_postindex(dest, tmp, 4);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 4, 4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = str_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 4);
             break;
         }
 
@@ -686,7 +690,8 @@ uint32_t *EMIT_DBcc(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         // Suggested by Paraj - a way to allow old code using DBF as busy loop work:
         // For busy loops (of the form l dbf dN,l) in chip mem add extra delay that is
         // at least 10 7MHz clocks (For old school replayer routines)
-        if (__m68k_state->JIT_CONTROL2 & JC2F_DBF_SLOWDOWN)
+        if (!BELLATRIX_MACHINE_API_ACTIVE &&
+            (__m68k_state->JIT_CONTROL2 & JC2F_DBF_SLOWDOWN))
         {
             if (m68k_condition == M_CC_F && branch_offset == 0 && (uintptr_t)*m68k_ptr < 0x200000)
             {
diff --git a/src/M68k_LINE6.c b/src/M68k_LINE6.c
index 6073ee6..772a077 100644
--- a/src/M68k_LINE6.c
+++ b/src/M68k_LINE6.c
@@ -70,9 +70,9 @@ uint32_t *EMIT_BRA(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         }
 
         if ((addend + abs_off))
-            *ptr++ = str_offset_preindex(sp, tmp, -4);
+            EMU68_GUEST_PRESTORE(ptr, sp, tmp, 4, -4);
         else
-            *ptr++ = str_offset_preindex(sp, REG_PC, -4);
+            EMU68_GUEST_PRESTORE(ptr, sp, REG_PC, 4, -4);
 
         bsr = 1;
 
@@ -399,4 +399,4 @@ int M68K_GetLine6Length(uint16_t *insn_stream)
     }
 
     return length;
-}
\ No newline at end of file
+}
diff --git a/src/M68k_LINE8.c b/src/M68k_LINE8.c
index 2e2dff6..53c6808 100644
--- a/src/M68k_LINE8.c
+++ b/src/M68k_LINE8.c
@@ -37,7 +37,7 @@ uint32_t *EMIT_PACK_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         uint8_t an_src = RA_MapM68kRegister(&ptr, 8 + (opcode & 7));
         tmp = RA_AllocARMRegister(&ptr);
         
-        *ptr++ = ldrsh_offset_preindex(an_src, tmp, -2);
+        EMU68_GUEST_PRELOAD(ptr, an_src, tmp, 2, 1, -2);
 
         RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
     }
@@ -62,10 +62,10 @@ uint32_t *EMIT_PACK_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
 
         *ptr++ = lsr(tmp, tmp, 4);
         if (((opcode >> 9) & 7) == 7) {
-            *ptr++ = strb_offset_preindex(dst, tmp, -2);
+            EMU68_GUEST_PRESTORE(ptr, dst, tmp, 1, -2);
         }
         else {
-            *ptr++ = strb_offset_preindex(dst, tmp, -1);
+            EMU68_GUEST_PRESTORE(ptr, dst, tmp, 1, -1);
         }
     }
     else
@@ -95,10 +95,10 @@ uint32_t *EMIT_UNPK_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         tmp = RA_AllocARMRegister(&ptr);
 
         if ((opcode & 7) == 7) {
-            *ptr++ = ldrb_offset_preindex(an_src, tmp, -2);
+            EMU68_GUEST_PRELOAD(ptr, an_src, tmp, 1, 0, -2);
         }
         else {
-            *ptr++ = ldrb_offset_preindex(an_src, tmp, -1);
+            EMU68_GUEST_PRELOAD(ptr, an_src, tmp, 1, 0, -1);
         }
 
         RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
@@ -124,7 +124,7 @@ uint32_t *EMIT_UNPK_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
     {
         uint8_t dst = RA_MapM68kRegister(&ptr, 8 + ((opcode >> 9) & 7));
         RA_SetDirtyM68kRegister(&ptr, 8 + ((opcode >> 9) & 7));
-        *ptr++ = strh_offset_preindex(dst, tmp, -2);
+        EMU68_GUEST_PRESTORE(ptr, dst, tmp, 2, -2);
     }
     else
     {
@@ -201,11 +201,11 @@ uint32_t *EMIT_OR_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         case 4:
             if (mode == 4)
             {
-                *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldr_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
             /* Perform calcualtion */
             *ptr++ = orr_reg(tmp, tmp, src, LSL, 0);
@@ -213,21 +213,21 @@ uint32_t *EMIT_OR_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = str_offset_postindex(dest, tmp, 4);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 4, 4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = str_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 4);
             break;
         
         case 2:
             if (mode == 4)
             {
-                *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrh_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
             
             /* Perform calcualtion */
             *ptr++ = orr_reg(tmp, tmp, src, LSL, 0);
@@ -235,21 +235,22 @@ uint32_t *EMIT_OR_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strh_offset_postindex(dest, tmp, 2);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strh_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 2);
             break;
         
         case 1:
             if (mode == 4)
             {
-                *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrb_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
 
             /* Perform calcualtion */
             *ptr++ = orr_reg(tmp, tmp, src, LSL, 0);
@@ -257,11 +258,12 @@ uint32_t *EMIT_OR_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strb_offset_postindex(dest, tmp, (opcode & 7) == 7 ? 2 : 1);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strb_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 1);
             break;
         }
 
@@ -416,13 +418,13 @@ kprintf("[ERROR] SBCD mem is not yet fixed!\n");
 
     /* predecremented address, special case if SP */
     if ((opcode & 7) == 7)
-        *ptr++ = ldrb_offset_preindex(an_src, src, -2);
+        EMU68_GUEST_PRELOAD(ptr, an_src, src, 1, 0, -2);
     else
-        *ptr++ = ldrb_offset_preindex(an_src, src, -1);
+        EMU68_GUEST_PRELOAD(ptr, an_src, src, 1, 0, -1);
     if (((opcode >> 9) & 7) == 7)
-        *ptr++ = ldrb_offset_preindex(an_dest, tmp_n, -2);
+        EMU68_GUEST_PRELOAD(ptr, an_dest, tmp_n, 1, 0, -2);
     else
-        *ptr++ = ldrb_offset_preindex(an_dest, tmp_n, -1);
+        EMU68_GUEST_PRELOAD(ptr, an_dest, tmp_n, 1, 0, -1);
 
     *ptr++ = and_immed(tmp_a, src, 4, 0);   // Fetch low nibbles
     *ptr++ = and_immed(tmp_b, tmp_n, 4, 0);
@@ -482,7 +484,7 @@ kprintf("[ERROR] SBCD mem is not yet fixed!\n");
     }
 
     // Insert result into target register
-    *ptr++ = strb_offset(an_dest, tmp_b, 0);
+    EMU68_GUEST_STORE(ptr, an_dest, tmp_b, 1);
 
     if (update_mask & SR_Z) {
         *ptr++ = ands_immed(31, tmp_b, 8, 0);
diff --git a/src/M68k_LINE9.c b/src/M68k_LINE9.c
index c225627..9d8146e 100644
--- a/src/M68k_LINE9.c
+++ b/src/M68k_LINE9.c
@@ -143,11 +143,11 @@ uint32_t *EMIT_SUB_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         case 4:
             if (mode == 4)
             {
-                *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldr_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
             /* Perform calcualtion */
             *ptr++ = subs_reg(tmp, tmp, src, LSL, 0);
@@ -155,21 +155,21 @@ uint32_t *EMIT_SUB_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = str_offset_postindex(dest, tmp, 4);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 4, 4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = str_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 4);
             break;
         
         case 2:
             if (mode == 4)
             {
-                *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrh_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
             
             /* Perform calcualtion */
             if (update_mask == 0 || update_mask == SR_Z || update_mask == SR_N) {
@@ -184,21 +184,22 @@ uint32_t *EMIT_SUB_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strh_offset_postindex(dest, tmp, 2);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strh_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 2);
             break;
         
         case 1:
             if (mode == 4)
             {
-                *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrb_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
 
             /* Perform calcualtion */
             if (update_mask == 0 || update_mask == SR_Z || update_mask == SR_N) {
@@ -213,11 +214,12 @@ uint32_t *EMIT_SUB_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strb_offset_postindex(dest, tmp, (opcode & 7) == 7 ? 2 : 1);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strb_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 1);
             break;
         }
 
@@ -526,8 +528,10 @@ uint32_t *EMIT_SUBX_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         switch (size)
         {
             case 0: /* Byte */
-                *ptr++ = ldrb_offset_preindex(regx, src, (opcode & 7) == 7 ? -2 : -1);
-                *ptr++ = ldrb_offset_preindex(regy, dest, ((opcode >> 9) & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, regx, src, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, regy, dest, 1, 0,
+                                    ((opcode >> 9) & 7) == 7 ? -2 : -1);
 
                 tmp = RA_AllocARMRegister(&ptr);
 
@@ -561,13 +565,13 @@ uint32_t *EMIT_SUBX_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                     *ptr++ = adds_reg(31, 31, tmp, LSL, 24);
                 }
 
-                *ptr++ = strb_offset(regy, tmp, 0);
+                EMU68_GUEST_STORE(ptr, regy, tmp, 1);
                 RA_FreeARMRegister(&ptr, tmp);
                 break;
 
             case 1: /* Word */
-                *ptr++ = ldrh_offset_preindex(regx, src, -2);
-                *ptr++ = ldrh_offset_preindex(regy, dest, -2);
+                EMU68_GUEST_PRELOAD(ptr, regx, src, 2, 0, -2);
+                EMU68_GUEST_PRELOAD(ptr, regy, dest, 2, 0, -2);
                 tmp = RA_AllocARMRegister(&ptr);
 
                 *ptr++ = sub_reg(tmp, dest, src, LSL, 0);
@@ -600,17 +604,17 @@ uint32_t *EMIT_SUBX_reg(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                     *ptr++ = adds_reg(31, 31, tmp, LSL, 16);
                 }
 
-                *ptr++ = strh_offset(regy, tmp, 0);
+                EMU68_GUEST_STORE(ptr, regy, tmp, 2);
                 RA_FreeARMRegister(&ptr, tmp);
                 break;
 
             case 2: /* Long */
-                *ptr++ = ldr_offset_preindex(regx, src, -4);
-                *ptr++ = ldr_offset_preindex(regy, dest, -4);
+                EMU68_GUEST_PRELOAD(ptr, regx, src, 4, 0, -4);
+                EMU68_GUEST_PRELOAD(ptr, regy, dest, 4, 0, -4);
 
                 tmp = RA_AllocARMRegister(&ptr);
                 *ptr++ = sbcs(dest, dest, src);
-                *ptr++ = str_offset(regy, dest, 0);
+                EMU68_GUEST_STORE(ptr, regy, dest, 4);
                 RA_FreeARMRegister(&ptr, tmp);
                 break;
         }
@@ -746,4 +750,4 @@ int M68K_GetLine9Length(uint16_t *insn_stream)
     }
 
     return length;
-}
\ No newline at end of file
+}
diff --git a/src/M68k_LINEB.c b/src/M68k_LINEB.c
index e5db39a..4b33cb8 100644
--- a/src/M68k_LINEB.c
+++ b/src/M68k_LINEB.c
@@ -243,11 +243,11 @@ static uint32_t *EMIT_EOR_ext(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
         case 4:
             if (mode == 4)
             {
-                *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldr_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
             /* Perform calcualtion */
             *ptr++ = eor_reg(tmp, tmp, src, LSL, 0);
@@ -255,21 +255,21 @@ static uint32_t *EMIT_EOR_ext(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = str_offset_postindex(dest, tmp, 4);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 4, 4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = str_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 4);
             break;
         
         case 2:
             if (mode == 4)
             {
-                *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrh_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
             
             /* Perform calcualtion */
             *ptr++ = eor_reg(tmp, tmp, src, LSL, 0);
@@ -277,20 +277,21 @@ static uint32_t *EMIT_EOR_ext(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strh_offset_postindex(dest, tmp, 2);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strh_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 2);
             break;
         case 1:
             if (mode == 4)
             {
-                *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrb_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
 
             /* Perform calcualtion */
             *ptr++ = eor_reg(tmp, tmp, src, LSL, 0);
@@ -298,11 +299,12 @@ static uint32_t *EMIT_EOR_ext(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strb_offset_postindex(dest, tmp, (opcode & 7) == 7 ? 2 : 1);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strb_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 1);
             break;
         }
 
@@ -433,4 +435,4 @@ int M68K_GetLineBLength(uint16_t *insn_stream)
     }
 
     return length;
-}
\ No newline at end of file
+}
diff --git a/src/M68k_LINEC.c b/src/M68k_LINEC.c
index 09aff47..00be384 100644
--- a/src/M68k_LINEC.c
+++ b/src/M68k_LINEC.c
@@ -80,11 +80,11 @@ static uint32_t *EMIT_AND_ext(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
         case 4:
             if (mode == 4)
             {
-                *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldr_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
             /* Perform calcualtion */
             *ptr++ = ands_reg(tmp, tmp, src, LSL, 0);
@@ -92,20 +92,20 @@ static uint32_t *EMIT_AND_ext(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = str_offset_postindex(dest, tmp, 4);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 4, 4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = str_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 4);
             break;
         case 2:
             if (mode == 4)
             {
-                *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrh_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
 
             /* Perform calcualtion */
             *ptr++ = and_reg(tmp, tmp, src, LSL, 0);
@@ -113,20 +113,21 @@ static uint32_t *EMIT_AND_ext(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strh_offset_postindex(dest, tmp, 2);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strh_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 2);
             break;
         case 1:
             if (mode == 4)
             {
-                *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrb_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
 
             /* Perform calcualtion */
             *ptr++ = and_reg(tmp, tmp, src, LSL, 0);
@@ -134,11 +135,12 @@ static uint32_t *EMIT_AND_ext(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strb_offset_postindex(dest, tmp, (opcode & 7) == 7 ? 2 : 1);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 1,
+                                      (opcode & 7) == 7 ? 2 : 1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strb_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 1);
             break;
         }
 
@@ -317,17 +319,17 @@ static uint32_t *EMIT_ABCD_mem(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_p
 
     // Fetch initial data into regs tmp_a and tmp_b
     if ((opcode & 7) == 7) {
-        *ptr++ = ldrb_offset_preindex(an_src, tmp_a, -2);
+        EMU68_GUEST_PRELOAD(ptr, an_src, tmp_a, 1, 0, -2);
     }
     else {
-        *ptr++ = ldrb_offset_preindex(an_src, tmp_a, -1);
+        EMU68_GUEST_PRELOAD(ptr, an_src, tmp_a, 1, 0, -1);
     }
 
     if (((opcode >> 9) & 7) == 7) {
-        *ptr++ = ldrb_offset_preindex(an_dst, tmp_b, -2);
+        EMU68_GUEST_PRELOAD(ptr, an_dst, tmp_b, 1, 0, -2);
     }
     else {
-        *ptr++ = ldrb_offset_preindex(an_dst, tmp_b, -1);
+        EMU68_GUEST_PRELOAD(ptr, an_dst, tmp_b, 1, 0, -1);
     }
 
     RA_SetDirtyM68kRegister(&ptr, 8 + ((opcode >> 9) & 7));
@@ -378,7 +380,7 @@ static uint32_t *EMIT_ABCD_mem(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_p
     }
     
     // Insert into result
-    *ptr++ = strb_offset(an_dst, tmp_b, 0);
+    EMU68_GUEST_STORE(ptr, an_dst, tmp_b, 1);
 
     if (update_mask & SR_Z)
     {
@@ -492,4 +494,4 @@ int M68K_GetLineCLength(uint16_t *insn_stream)
     }
 
     return length;
-}
\ No newline at end of file
+}
diff --git a/src/M68k_LINED.c b/src/M68k_LINED.c
index b75d4cd..eea45d5 100644
--- a/src/M68k_LINED.c
+++ b/src/M68k_LINED.c
@@ -132,11 +132,11 @@ static uint32_t *EMIT_ADD_ext(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
         case 4:
             if (mode == 4)
             {
-                *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldr_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
             /* Perform calcualtion */
             *ptr++ = adds_reg(tmp, tmp, src, LSL, 0);
@@ -144,21 +144,21 @@ static uint32_t *EMIT_ADD_ext(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = str_offset_postindex(dest, tmp, 4);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 4, 4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = str_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 4);
             break;
         
         case 2:
             if (mode == 4)
             {
-                *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrh_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
             
             /* Perform calcualtion */
             if (update_mask == 0 || update_mask == SR_Z || update_mask == SR_N) {
@@ -174,20 +174,21 @@ static uint32_t *EMIT_ADD_ext(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strh_offset_postindex(dest, tmp, 2);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strh_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 2);
             break;
         case 1:
             if (mode == 4)
             {
-                *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrb_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
 
             /* Perform calcualtion */
             if (update_mask == 0 || update_mask == SR_Z || update_mask == SR_N) {
@@ -203,11 +204,12 @@ static uint32_t *EMIT_ADD_ext(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strb_offset_postindex(dest, tmp, (opcode & 7) == 7 ? 2 : 1);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strb_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 1);
             break;
         }
 
@@ -490,8 +492,10 @@ static uint32_t *EMIT_ADDX_mem(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_p
         switch (size)
         {
             case 0: /* Byte */
-                *ptr++ = ldrb_offset_preindex(regx, src, (opcode & 7) == 7 ? -2 : -1);
-                *ptr++ = ldrb_offset_preindex(regy, dest, ((opcode >> 9) & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, regx, src, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, regy, dest, 1, 0,
+                                    ((opcode >> 9) & 7) == 7 ? -2 : -1);
 
                 tmp = RA_AllocARMRegister(&ptr);
                 *ptr++ = add_reg(tmp, dest, src, LSL, 0);
@@ -523,13 +527,13 @@ static uint32_t *EMIT_ADDX_mem(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_p
                     *ptr++ = adds_reg(31, 31, tmp, LSL, 24);
                 }
 
-                *ptr++ = strb_offset(regy, tmp, 0);
+                EMU68_GUEST_STORE(ptr, regy, tmp, 1);
                 RA_FreeARMRegister(&ptr, tmp);
                 break;
 
             case 1: /* Word */
-                *ptr++ = ldrh_offset_preindex(regx, src, -2);
-                *ptr++ = ldrh_offset_preindex(regy, dest, -2);
+                EMU68_GUEST_PRELOAD(ptr, regx, src, 2, 0, -2);
+                EMU68_GUEST_PRELOAD(ptr, regy, dest, 2, 0, -2);
 
                 tmp = RA_AllocARMRegister(&ptr);
 
@@ -562,17 +566,17 @@ static uint32_t *EMIT_ADDX_mem(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_p
                     *ptr++ = adds_reg(31, 31, tmp, LSL, 16);
                 }
 
-                *ptr++ = strh_offset(regy, tmp, 0);
+                EMU68_GUEST_STORE(ptr, regy, tmp, 2);
                 RA_FreeARMRegister(&ptr, tmp);
                 break;
 
             case 2: /* Long */
-                *ptr++ = ldr_offset_preindex(regx, src, -4);
-                *ptr++ = ldr_offset_preindex(regy, dest, -4);
+                EMU68_GUEST_PRELOAD(ptr, regx, src, 4, 0, -4);
+                EMU68_GUEST_PRELOAD(ptr, regy, dest, 4, 0, -4);
 
                 tmp = RA_AllocARMRegister(&ptr);
                 *ptr++ = adcs(dest, dest, src);
-                *ptr++ = str_offset(regy, dest, 0);
+                EMU68_GUEST_STORE(ptr, regy, dest, 4);
                 RA_FreeARMRegister(&ptr, tmp);
                 break;
         }
@@ -707,4 +711,4 @@ int M68K_GetLineDLength(uint16_t *insn_stream)
     }
 
     return length;
-}
\ No newline at end of file
+}
```

### `0030-emu68-explicit-stack-movem.patch`

```diff
diff --git a/src/M68k_LINE4.c b/src/M68k_LINE4.c
index 4ed0d5f..45bd702 100644
--- a/src/M68k_LINE4.c
+++ b/src/M68k_LINE4.c
@@ -166,59 +166,61 @@ uint32_t *EMIT_NOT(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, uint16_t
         case 4:
             if (mode == 4)
             {
-                *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldr_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
             *ptr++ = mvn_reg(tmp, tmp, LSL, 0);
 
             if (mode == 3)
             {
-                *ptr++ = str_offset_postindex(dest, tmp, 4);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 4, 4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = str_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 4);
             break;
         case 2:
             if (mode == 4)
             {
-                *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrh_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
 
             *ptr++ = mvn_reg(tmp, tmp, LSL, 0);
 
             if (mode == 3)
             {
-                *ptr++ = strh_offset_postindex(dest, tmp, 2);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strh_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 2);
             break;
         case 1:
             if (mode == 4)
             {
-                *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrb_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
 
             *ptr++ = mvn_reg(tmp, tmp, LSL, 0);
 
             if (mode == 3)
             {
-                *ptr++ = strb_offset_postindex(dest, tmp, (opcode & 7) == 7 ? 2 : 1);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strb_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 1);
             break;
         }
     }
@@ -344,30 +346,30 @@ uint32_t *EMIT_NEG(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, uint16_t
         case 4:
             if (mode == 4)
             {
-                *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldr_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
             *ptr++ = negs_reg(tmp, tmp, LSL, 0);
 
             if (mode == 3)
             {
-                *ptr++ = str_offset_postindex(dest, tmp, 4);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 4, 4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = str_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 4);
             break;
         case 2:
             if (mode == 4)
             {
-                *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrh_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
 
             if (update_mask == 0) {
                 *ptr++ = neg_reg(tmp, tmp, LSL, 0);
@@ -379,20 +381,21 @@ uint32_t *EMIT_NEG(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, uint16_t
 
             if (mode == 3)
             {
-                *ptr++ = strh_offset_postindex(dest, tmp, 2);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strh_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 2);
             break;
         case 1:
             if (mode == 4)
             {
-                *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrb_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
 
             if (update_mask == 0) {
                 *ptr++ = neg_reg(tmp, tmp, LSL, 0);
@@ -404,11 +407,12 @@ uint32_t *EMIT_NEG(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, uint16_t
 
             if (mode == 3)
             {
-                *ptr++ = strb_offset_postindex(dest, tmp, (opcode & 7) == 7 ? 2 : 1);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strb_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 1);
             break;
         }
 
@@ -622,30 +626,30 @@ uint32_t *EMIT_NEGX(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, uint16_
         case 4:
             if (mode == 4)
             {
-                *ptr++ = ldr_offset_preindex(dest, src, -4);
+                EMU68_GUEST_PRELOAD(ptr, dest, src, 4, 0, -4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldr_offset(dest, src, 0);
+                EMU68_GUEST_LOAD(ptr, dest, src, 4, 0);
 
             *ptr++ = ngcs(tmp, src);
 
             if (mode == 3)
             {
-                *ptr++ = str_offset_postindex(dest, tmp, 4);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 4, 4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = str_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 4);
             break;
         case 2:
             if (mode == 4)
             {
-                *ptr++ = ldrh_offset_preindex(dest, src, -2);
+                EMU68_GUEST_PRELOAD(ptr, dest, src, 2, 0, -2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrh_offset(dest, src, 0);
+                EMU68_GUEST_LOAD(ptr, dest, src, 2, 0);
 
             *ptr++ = neg_reg(tmp, src, LSL, 0);     // negate
             *ptr++ = b_cc(A64_CC_EQ, 2);            // Skip if X not set
@@ -691,20 +695,21 @@ uint32_t *EMIT_NEGX(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, uint16_
 
             if (mode == 3)
             {
-                *ptr++ = strh_offset_postindex(dest, tmp, 2);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strh_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 2);
             break;
         case 1:
             if (mode == 4)
             {
-                *ptr++ = ldrb_offset_preindex(dest, src, (opcode & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, dest, src, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrb_offset(dest, src, 0);
+                EMU68_GUEST_LOAD(ptr, dest, src, 1, 0);
 
             *ptr++ = neg_reg(tmp, src, LSL, 0);     // negate
             *ptr++ = b_cc(A64_CC_EQ, 2);            // Skip if X not set
@@ -750,11 +755,12 @@ uint32_t *EMIT_NEGX(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, uint16_
 
             if (mode == 3)
             {
-                *ptr++ = strb_offset_postindex(dest, tmp, (opcode & 7) == 7 ? 2 : 1);
+                EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strb_offset(dest, tmp, 0);
+                EMU68_GUEST_STORE(ptr, dest, tmp, 1);
             break;
         }
 
@@ -1238,7 +1244,7 @@ static uint32_t *EMIT_LINK32(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr
     else {
         reg = RA_MapM68kRegister(&ptr, 8 + (opcode & 7));
     }
-    *ptr++ = str_offset_preindex(sp, reg, -4);  /* SP = SP - 4; An -> (SP) */
+    EMU68_GUEST_PRESTORE(ptr, sp, reg, 4, -4); /* SP = SP - 4; An -> (SP) */
     *ptr++ = mov_reg(reg, sp);
     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
 
@@ -1272,7 +1278,7 @@ static uint32_t *EMIT_LINK16(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr
     else {
         reg = RA_MapM68kRegister(&ptr, 8 + (opcode & 7));
     }
-    *ptr++ = str_offset_preindex(sp, reg, -4);  /* SP = SP - 4; An -> (SP) */
+    EMU68_GUEST_PRESTORE(ptr, sp, reg, 4, -4); /* SP = SP - 4; An -> (SP) */
     *ptr++ = mov_reg(reg, sp);
     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
 
@@ -1385,7 +1391,7 @@ static uint32_t *EMIT_UNLK(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr,
     reg = RA_MapM68kRegister(&ptr, 8 + (opcode & 7));
 
     *ptr++ = mov_reg(sp, reg);
-    *ptr++ = ldr_offset_postindex(sp, reg, 4);
+    EMU68_GUEST_POSTLOAD(ptr, sp, reg, 4, 0, 4);
 
     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
     RA_SetDirtyM68kRegister(&ptr, 15);
@@ -1610,7 +1633,7 @@ static uint32_t *EMIT_RTE(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, u
     ptr++;
 
     // Now check frame format
-    *ptr++ = ldrh_offset(sp, tmp, 6);
+    EMU68_GUEST_LOAD_OFFSET(ptr, sp, 6, tmp, 2, 0);
     *ptr++ = lsr(tmp, tmp, 12);
 
     // Is format valid?
@@ -1632,12 +1655,12 @@ static uint32_t *EMIT_RTE(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, u
     *tmpptr = b_cc(A64_CC_EQ, ptr - tmpptr);
 
     /* Fetch sr from stack */
-    *ptr++ = ldrh_offset_postindex(sp, changed, 2);
+    EMU68_GUEST_POSTLOAD(ptr, sp, changed, 2, 0, 2);
     /* Reverse C and V */
     *ptr++ = rbit(orig, changed);
     *ptr++ = bfxil(changed, orig, 30, 2);
     /* Fetch PC from stack, advance sp so that format word is skipped */
-    *ptr++ = ldr_offset_postindex(sp, REG_PC, 6);
+    EMU68_GUEST_POSTLOAD(ptr, sp, REG_PC, 4, 0, 6);
 
     /* In case of format 2, skip subsequent longword on stack */
     *ptr++ = cmp_immed(tmp, 2);
@@ -1704,7 +1727,7 @@ static uint32_t *EMIT_RTD(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, u
     int16_t addend = cache_read_16(ICACHE, (uintptr_t)&(*m68k_ptr)[0]);
 
     /* Fetch return address from stack */
-    *ptr++ = ldr_offset_postindex(sp, tmp2, 4);
+    EMU68_GUEST_POSTLOAD(ptr, sp, tmp2, 4, 0, 4);
 
     if (addend > -4096 && addend < 4096)
     {
@@ -1748,7 +1771,7 @@ static uint32_t *EMIT_RTS(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, u
     uint8_t sp = RA_MapM68kRegister(&ptr, 15);
 
     /* Fetch return address from stack */
-    *ptr++ = ldr_offset_postindex(sp, REG_PC, 4);
+    EMU68_GUEST_POSTLOAD(ptr, sp, REG_PC, 4, 0, 4);
     ptr = EMIT_ResetOffsetPC(ptr);
     RA_SetDirtyM68kRegister(&ptr, 15);
 
@@ -1824,7 +1847,7 @@ static uint32_t *EMIT_RTR(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, u
     uint8_t sp = RA_MapM68kRegister(&ptr, 15);
 
     /* Fetch status byte from stack */
-    *ptr++ = ldrh_offset_postindex(sp, tmp, 2);
+    EMU68_GUEST_POSTLOAD(ptr, sp, tmp, 2, 0, 2);
     /* Reverse C and V */
     *ptr++ = rbit(0, tmp);
     *ptr++ = bfxil(tmp, 0, 30, 2);
@@ -1833,7 +1856,7 @@ static uint32_t *EMIT_RTR(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, u
     *ptr++ = bfi(cc, tmp, 0, 5);
 
     /* Fetch return address from stack */
-    *ptr++ = ldr_offset_postindex(sp, REG_PC, 4);
+    EMU68_GUEST_POSTLOAD(ptr, sp, REG_PC, 4, 0, 4);
     ptr = EMIT_ResetOffsetPC(ptr);
     RA_SetDirtyM68kRegister(&ptr, 15);
     *ptr++ = INSN_TO_LE(0xffffffff);
@@ -2295,7 +2318,7 @@ static uint32_t *EMIT_JSR(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, u
     ptr = EMIT_LoadFromEffectiveAddress(ptr, 0, &ea, opcode & 0x3f, (*m68k_ptr), &ext_words, 1, NULL);
     ptr = EMIT_AdvancePC(ptr, 2 * (ext_words + 1));
     ptr = EMIT_FlushPC(ptr);
-    *ptr++ = str_offset_preindex(sp, REG_PC, -4);
+    EMU68_GUEST_PRESTORE(ptr, sp, REG_PC, 4, -4);
     RA_SetDirtyM68kRegister(&ptr, 15);
     ptr = EMIT_ResetOffsetPC(ptr);
     *ptr++ = mov_reg(REG_PC, ea);
@@ -2353,14 +2376,14 @@ static uint32_t *EMIT_NBCD(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr,
         if ((opcode & 0x38) == 0x20)
         {
             if ((opcode & 7) == 7) {
-                *ptr++ = ldrb_offset_preindex(ea, t, -2);
+                EMU68_GUEST_PRELOAD(ptr, ea, t, 1, 0, -2);
             }
             else {
-                *ptr++ = ldrb_offset_preindex(ea, t, -1);
+                EMU68_GUEST_PRELOAD(ptr, ea, t, 1, 0, -1);
             }
         }
         else {
-            *ptr++ = ldrb_offset(ea, t, 0);
+            EMU68_GUEST_LOAD(ptr, ea, t, 1, 0);
         }
 
         *ptr++ = and_immed(lo, t, 4, 0);
@@ -2440,14 +2463,14 @@ static uint32_t *EMIT_NBCD(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr,
         if ((opcode & 0x38) == 0x18)
         {
             if ((opcode & 7) == 7) {
-                *ptr++ = strb_offset_postindex(ea, result, 2);
+                EMU68_GUEST_POSTSTORE(ptr, ea, result, 1, 2);
             }
             else {
-                *ptr++ = strb_offset_postindex(ea, result, 1);
+                EMU68_GUEST_POSTSTORE(ptr, ea, result, 1, 1);
             }
         }
         else {
-            *ptr++ = strb_offset(ea, result, 0);
+            EMU68_GUEST_STORE(ptr, ea, result, 1);
         }
     }
 
@@ -2476,7 +2499,7 @@ static uint32_t *EMIT_PEA(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr, u
     sp = RA_MapM68kRegister(&ptr, 15);
     RA_SetDirtyM68kRegister(&ptr, 15);
 
-    *ptr++ = str_offset_preindex(sp, ea, -4);
+    EMU68_GUEST_PRESTORE(ptr, sp, ea, 4, -4);
 
     RA_FreeARMRegister(&ptr, sp);
     RA_FreeARMRegister(&ptr, ea);
@@ -2528,75 +2551,36 @@ static uint32_t *EMIT_MOVEM(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr,
                 *ptr++ = sub_immed(tmp_base_reg, base, size ? 4 : 2);
             }
 
-            /* In pre-decrement the register order is reversed */
-            uint8_t rt1 = 0xff;
+            *ptr++ = sub_immed(base, base, block_size);
 
+            /* In pre-decrement the register order is reversed. */
             for (int i=0; i < 16; i++)
             {
                 if (mask & (0x8000 >> i))
                 {
                     uint8_t reg = (i == ((opcode & 7) + 8) ? tmp_base_reg : RA_MapM68kRegister(&ptr, i));
-
-                    if (size) {
-                        if (rt1 == 0xff)
-                            rt1 = reg;
-                        else {
-                            if (offset == 0)
-                                *ptr++ = stp_preindex(base, rt1, reg, -block_size);
-                            else
-                                *ptr++ = stp(base, rt1, reg, offset);
-                            offset += 8;
-                            rt1 = 0xff;
-                        }
-                    }
-                    else
-                    {
-                        if (offset == 0)
-                            *ptr++ = strh_offset_preindex(base, reg, -block_size);
-                        else
-                            *ptr++ = strh_offset(base, reg, offset);
-
-                        offset += 2;
-                    }
+                    EMU68_GUEST_STORE_OFFSET(ptr, base, offset, reg,
+                                             size ? 4 : 2);
+                    offset += size ? 4 : 2;
                 }
             }
-            if (rt1 != 0xff) {
-                if (offset == 0)
-                    *ptr++ = str_offset_preindex(base, rt1, -block_size);
-                else
-                    *ptr++ = str_offset(base, rt1, offset);
-            }
 
             RA_FreeARMRegister(&ptr, tmp_base_reg);
         }
         else
         {
             uint8_t offset = 0;
-            uint8_t rt1 = 0xff;
 
             for (int i=0; i < 16; i++)
             {
                 if (mask & (1 << i))
                 {
                     uint8_t reg = RA_MapM68kRegister(&ptr, i);
-                    if (size) {
-                        if (rt1 == 0xff)
-                            rt1 = reg;
-                        else {
-                            *ptr++ = stp(base, rt1, reg, offset);
-                            offset += 8;
-                            rt1 = 0xff;
-                        }
-                    }
-                    else
-                    {
-                        *ptr++ = strh_offset(base, reg, offset);
-                        offset += 2;
-                    }
+                    EMU68_GUEST_STORE_OFFSET(ptr, base, offset, reg,
+                                             size ? 4 : 2);
+                    offset += size ? 4 : 2;
                 }
             }
-            if (rt1 != 0xff)
-                *ptr++ = str_offset(base, rt1, offset);
         }
 
         RA_FreeARMRegister(&ptr, base);
@@ -2608,8 +2592,6 @@ static uint32_t *EMIT_MOVEM(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr,
 
         ptr = EMIT_LoadFromEffectiveAddress(ptr, 0, &base, opcode & 0x3f, *m68k_ptr, &ext_words, 0, NULL);
 
-        uint8_t rt1 = 0xff;
-
         for (int i=0; i < 16; i++)
         {
             if (mask & (1 << i))
@@ -2618,43 +2600,16 @@ static uint32_t *EMIT_MOVEM(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr,
                 if (((opcode & 0x38) == 0x18)) RA_MapM68kRegister(&ptr, (opcode & 7) + 8);
 
                 uint8_t reg = RA_MapM68kRegisterForWrite(&ptr, i);
-                if (size) {
-                    if ((((opcode & 0x38) == 0x18) && (i == (opcode & 7) + 8))) {
-                        /* If rt1 was set, flush it now and reset, skip the base register */
-                        if (rt1 != 0xff) {
-                            *ptr++ = ldr_offset(base, rt1, offset);
-                            rt1 = 0xff;
-                            offset += 4;
-                        }
-                        offset += 4;
-                        continue;
-                    }
-
-                    if (rt1 == 0xff)
-                        rt1 = reg;
-                    else {
-                        if (block_size == 8 && (opcode & 0x38) == 0x18)
-                            *ptr++ = ldp_postindex(base, rt1, reg, 8);
-                        else 
-                            *ptr++ = ldp(base, rt1, reg, offset);
-                        offset += 8;
-                        rt1 = 0xff;
-                    }
-                }
-                else
-                {
-                    if (!(((opcode & 0x38) == 0x18) && (i == (opcode & 7) + 8)))
-                        *ptr++ = ldrsh_offset(base, reg, offset);
-                    offset += 2;
-                }
+                if (!(((opcode & 0x38) == 0x18) &&
+                      (i == (opcode & 7) + 8)))
+                    EMU68_GUEST_LOAD_OFFSET(ptr, base, offset, reg,
+                                            size ? 4 : 2, !size);
+                offset += size ? 4 : 2;
             }
         }
-        if (rt1 != 0xff) {
-            *ptr++ = ldr_offset(base, rt1, offset);
-        }
 
         /* Post-increment mode? Increase the base now */
-        if ((opcode & 0x38) == 0x18 && !(block_size == 8 && size))
+        if ((opcode & 0x38) == 0x18)
         {
             *ptr++ = add_immed(base, base, block_size);
             RA_SetDirtyM68kRegister(&ptr, (opcode & 7) + 8);
```

### `0031-emu68-explicit-line0-access.patch`

```diff
diff --git a/src/M68k_LINE0.c b/src/M68k_LINE0.c
index 557cf63..da22389 100644
--- a/src/M68k_LINE0.c
+++ b/src/M68k_LINE0.c
@@ -262,11 +262,11 @@ uint32_t *EMIT_SUBI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         case 4:
             if (mode == 4)
             {
-                *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldr_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
             /* Perform calcualtion */
             if (immediate)
@@ -277,20 +277,20 @@ uint32_t *EMIT_SUBI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = str_offset_postindex(dest, immed, 4);
+                EMU68_GUEST_POSTSTORE(ptr, dest, immed, 4, 4);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = str_offset(dest, immed, 0);
+                EMU68_GUEST_STORE(ptr, dest, immed, 4);
             break;
         case 2:
             if (mode == 4)
             {
-                *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrh_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
             
             /* Perform calcualtion */
             if (update_mask == 0) {               
@@ -318,20 +318,21 @@ uint32_t *EMIT_SUBI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strh_offset_postindex(dest, immed, 2);
+                EMU68_GUEST_POSTSTORE(ptr, dest, immed, 2, 2);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strh_offset(dest, immed, 0);
+                EMU68_GUEST_STORE(ptr, dest, immed, 2);
             break;
         case 1:
             if (mode == 4)
             {
-                *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = ldrb_offset(dest, tmp, 0);
+                EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
 
             /* Perform calcualtion */
             if (update_mask == 0) {
@@ -352,11 +353,12 @@ uint32_t *EMIT_SUBI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             /* Store back */
             if (mode == 3)
             {
-                *ptr++ = strb_offset_postindex(dest, immed, (opcode & 7) == 7 ? 2 : 1);
+                EMU68_GUEST_POSTSTORE(ptr, dest, immed, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             }
             else
-                *ptr++ = strb_offset(dest, immed, 0);
+                EMU68_GUEST_STORE(ptr, dest, immed, 1);
             break;
         }
 
@@ -521,11 +523,11 @@ uint32_t *EMIT_ADDI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             case 4:
                 if (mode == 4)
                 {
-                    *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldr_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
                 /* Perform calcualtion */
                 if (add_immediate)
@@ -536,20 +538,20 @@ uint32_t *EMIT_ADDI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = str_offset_postindex(dest, immed, 4);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, immed, 4, 4);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = str_offset(dest, immed, 0);
+                    EMU68_GUEST_STORE(ptr, dest, immed, 4);
                 break;
             case 2:
                 if (mode == 4)
                 {
-                    *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldrh_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
                 
                 /* Perform calcualtion */
                 if (update_mask == 0) {
@@ -576,20 +578,21 @@ uint32_t *EMIT_ADDI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = strh_offset_postindex(dest, immed, 2);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, immed, 2, 2);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = strh_offset(dest, immed, 0);
+                    EMU68_GUEST_STORE(ptr, dest, immed, 2);
                 break;
             case 1:
                 if (mode == 4)
                 {
-                    *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldrb_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
                 
                 /* Perform calcualtion */
                 if (update_mask == 0) {
@@ -609,11 +612,12 @@ uint32_t *EMIT_ADDI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = strb_offset_postindex(dest, immed, (opcode & 7) == 7 ? 2 : 1);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, immed, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = strb_offset(dest, immed, 0);
+                    EMU68_GUEST_STORE(ptr, dest, immed, 1);
                 break;
         }
 
@@ -897,11 +901,11 @@ uint32_t *EMIT_ORI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             case 4:
                 if (mode == 4)
                 {
-                    *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldr_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
                 /* Perform calcualtion */
                 if (mask32 == 0)
@@ -914,20 +918,20 @@ uint32_t *EMIT_ORI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = str_offset_postindex(dest, immed, 4);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, immed, 4, 4);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = str_offset(dest, immed, 0);
+                    EMU68_GUEST_STORE(ptr, dest, immed, 4);
                 break;
             case 2:
                 if (mode == 4)
                 {
-                    *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldrh_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
 
                 /* Perform calcualtion */
                 if (update_mask == 0) {
@@ -952,20 +956,21 @@ uint32_t *EMIT_ORI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = strh_offset_postindex(dest, immed, 2);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, immed, 2, 2);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = strh_offset(dest, immed, 0);
+                    EMU68_GUEST_STORE(ptr, dest, immed, 2);
                 break;
             case 1:
                 if (mode == 4)
                 {
-                    *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldrb_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
                 
                 /* Perform calcualtion */
                 if (update_mask == 0) {
@@ -990,11 +995,12 @@ uint32_t *EMIT_ORI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = strb_offset_postindex(dest, immed, (opcode & 7) == 7 ? 2 : 1);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, immed, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = strb_offset(dest, immed, 0);
+                    EMU68_GUEST_STORE(ptr, dest, immed, 1);
                 break;
         }
 
@@ -1274,11 +1280,11 @@ uint32_t *EMIT_ANDI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             case 4:
                 if (mode == 4)
                 {
-                    *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldr_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
                 /* Perform calcualtion */
                 if (mask32 == 0)
@@ -1289,20 +1295,20 @@ uint32_t *EMIT_ANDI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = str_offset_postindex(dest, immed, 4);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, immed, 4, 4);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = str_offset(dest, immed, 0);
+                    EMU68_GUEST_STORE(ptr, dest, immed, 4);
                 break;
             case 2:
                 if (mode == 4)
                 {
-                    *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldrh_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
                 
                 /* Perform calcualtion */
                 if (update_mask == 0) {
@@ -1319,20 +1325,21 @@ uint32_t *EMIT_ANDI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = strh_offset_postindex(dest, immed, 2);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, immed, 2, 2);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = strh_offset(dest, immed, 0);
+                    EMU68_GUEST_STORE(ptr, dest, immed, 2);
                 break;
             case 1:
                 if (mode == 4)
                 {
-                    *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldrb_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
                 
                 /* Perform calcualtion */
                 if (update_mask == 0) {
@@ -1349,11 +1356,12 @@ uint32_t *EMIT_ANDI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = strb_offset_postindex(dest, immed, (opcode & 7) == 7 ? 2 : 1);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, immed, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = strb_offset(dest, immed, 0);
+                    EMU68_GUEST_STORE(ptr, dest, immed, 1);
                 break;
         }
 
@@ -1572,11 +1580,11 @@ uint32_t *EMIT_EORI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             case 4:
                 if (mode == 4)
                 {
-                    *ptr++ = ldr_offset_preindex(dest, tmp, -4);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 4, 0, -4);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldr_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 4, 0);
 
                 /* Perform calcualtion */
                 if (mask32 == 0)
@@ -1589,20 +1597,20 @@ uint32_t *EMIT_EORI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = str_offset_postindex(dest, immed, 4);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, immed, 4, 4);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = str_offset(dest, immed, 0);
+                    EMU68_GUEST_STORE(ptr, dest, immed, 4);
                 break;
             case 2:
                 if (mode == 4)
                 {
-                    *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldrh_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
                 
                 /* Perform calcualtion */
                 *ptr++ = eor_reg(immed, immed, tmp, LSL, 16);
@@ -1613,21 +1621,22 @@ uint32_t *EMIT_EORI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = strh_offset_postindex(dest, immed, 2);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, immed, 2, 2);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = strh_offset(dest, immed, 0);
+                    EMU68_GUEST_STORE(ptr, dest, immed, 2);
                 break;
             
             case 1:
                 if (mode == 4)
                 {
-                    *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+                    EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = ldrb_offset(dest, tmp, 0);
+                    EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
                 
                 /* Perform calcualtion */
                 *ptr++ = eor_reg(immed, immed, tmp, LSL, 24);
@@ -1638,11 +1647,12 @@ uint32_t *EMIT_EORI(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
                 /* Store back */
                 if (mode == 3)
                 {
-                    *ptr++ = strb_offset_postindex(dest, immed, (opcode & 7) == 7 ? 2 : 1);
+                    EMU68_GUEST_POSTSTORE(ptr, dest, immed, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
                     RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
                 }
                 else
-                    *ptr++ = strb_offset(dest, immed, 0);
+                    EMU68_GUEST_STORE(ptr, dest, immed, 1);
                 break;
         }
 
@@ -1810,11 +1820,12 @@ uint32_t *EMIT_BCHG(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         /* Fetch data into temporary register, perform bit flip, store it back */
         if (mode == 4)
         {
-            *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+            EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
             RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
         }
         else
-            *ptr++ = ldrb_offset(dest, tmp, 0);
+            EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
 
         if (immediate)
         {
@@ -1835,11 +1846,12 @@ uint32_t *EMIT_BCHG(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         /* Store back */
         if (mode == 3)
         {
-            *ptr++ = strb_offset_postindex(dest, tmp, (opcode & 7) == 7 ? 2 : 1);
+            EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
             RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
         }
         else
-            *ptr++ = strb_offset(dest, tmp, 0);
+            EMU68_GUEST_STORE(ptr, dest, tmp, 1);
 
         RA_FreeARMRegister(&ptr, tmp);
     }
@@ -1926,11 +1938,12 @@ uint32_t *EMIT_BCLR(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         /* Fetch data into temporary register, perform bit flip, store it back */
         if (mode == 4)
         {
-            *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+            EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
             RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
         }
         else
-            *ptr++ = ldrb_offset(dest, tmp, 0);
+            EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
 
         if (immediate)
         {
@@ -1951,11 +1964,12 @@ uint32_t *EMIT_BCLR(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         /* Store back */
         if (mode == 3)
         {
-            *ptr++ = strb_offset_postindex(dest, tmp, (opcode & 7) == 7 ? 2 : 1);
+            EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
             RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
         }
         else
-            *ptr++ = strb_offset(dest, tmp, 0);
+            EMU68_GUEST_STORE(ptr, dest, tmp, 1);
 
         RA_FreeARMRegister(&ptr, tmp);
     }
@@ -2005,15 +2019,16 @@ uint32_t *EMIT_CMP2(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
     switch ((opcode >> 9) & 3)
     {
         case 0:
-            *ptr++ = ldrsb_offset(ea, lower, 0);
-            *ptr++ = ldrsb_offset(ea, higher, 1);
+            EMU68_GUEST_LOAD_OFFSET(ptr, ea, 0, lower, 1, 1);
+            EMU68_GUEST_LOAD_OFFSET(ptr, ea, 1, higher, 1, 1);
             break;
         case 1:
-            *ptr++ = ldrsh_offset(ea, lower, 0);
-            *ptr++ = ldrsh_offset(ea, higher, 2);
+            EMU68_GUEST_LOAD_OFFSET(ptr, ea, 0, lower, 2, 1);
+            EMU68_GUEST_LOAD_OFFSET(ptr, ea, 2, higher, 2, 1);
             break;
         case 2:
-            *ptr++ = ldp(ea, lower, higher, 0);
+            EMU68_GUEST_LOAD_OFFSET(ptr, ea, 0, lower, 4, 0);
+            EMU68_GUEST_LOAD_OFFSET(ptr, ea, 4, higher, 4, 0);
             break;
     }
 
@@ -2163,11 +2178,12 @@ uint32_t *EMIT_BSET(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         /* Fetch data into temporary register, perform bit flip, store it back */
         if (mode == 4)
         {
-            *ptr++ = ldrb_offset_preindex(dest, tmp, (opcode & 7) == 7 ? -2 : -1);
+            EMU68_GUEST_PRELOAD(ptr, dest, tmp, 1, 0,
+                                    (opcode & 7) == 7 ? -2 : -1);
             RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
         }
         else
-            *ptr++ = ldrb_offset(dest, tmp, 0);
+            EMU68_GUEST_LOAD(ptr, dest, tmp, 1, 0);
 
         if (immediate)
         {
@@ -2188,11 +2204,12 @@ uint32_t *EMIT_BSET(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         /* Store back */
         if (mode == 3)
         {
-            *ptr++ = strb_offset_postindex(dest, tmp, (opcode & 7) == 7 ? 2 : 1);
+            EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 1,
+                                     (opcode & 7) == 7 ? 2 : 1);
             RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
         }
         else
-            *ptr++ = strb_offset(dest, tmp, 0);
+            EMU68_GUEST_STORE(ptr, dest, tmp, 1);
 
         RA_FreeARMRegister(&ptr, tmp);
     }
@@ -2282,17 +2299,17 @@ uint32_t *EMIT_CAS(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         switch (size) \
         { \
             case 1:\
-                *ptr++ = ldrb_offset(ea, tmp, 0);\
+                EMU68_GUEST_LOAD(ptr, ea, tmp, 1, 0);\
                 *ptr++ = lsl(tmp, tmp, 24);\
                 *ptr++ = subs_reg(31, tmp, dc, LSL, 24);\
                 break;\
             case 2:\
-                *ptr++ = ldrh_offset(ea, tmp, 0);\
+                EMU68_GUEST_LOAD(ptr, ea, tmp, 2, 0);\
                 *ptr++ = lsl(tmp, tmp, 16);\
                 *ptr++ = subs_reg(31, tmp, dc, LSL, 16);\
                 break;\
             case 3:\
-                *ptr++ = ldr_offset(ea, tmp, 0);\
+                EMU68_GUEST_LOAD(ptr, ea, tmp, 4, 0);\
                 *ptr++ = subs_reg(31, tmp, dc, LSL, 0);\
                 break;\
         }\
@@ -2301,13 +2318,13 @@ uint32_t *EMIT_CAS(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         switch (size)\
         {\
             case 1:\
-                *ptr++ = strb_offset(ea, du, 0);\
+                EMU68_GUEST_STORE(ptr, ea, du, 1);\
                 break;\
             case 2:\
-                *ptr++ = strh_offset(ea, du, 0);\
+                EMU68_GUEST_STORE(ptr, ea, du, 2);\
                 break;\
             case 3:\
-                *ptr++ = str_offset(ea, du, 0);\
+                EMU68_GUEST_STORE(ptr, ea, du, 4);\
                 break;\
         }\
         *ptr++ = b(2);\
@@ -2351,37 +2368,47 @@ uint32_t *EMIT_CAS(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         {
             uint8_t tmp1 = RA_AllocARMRegister(&ptr);
             uint8_t tmp2 = RA_AllocARMRegister(&ptr);
-            *ptr++ = ldrh_offset(rn1, val1, 0);
-            *ptr++ = ldrh_offset(rn2, val2, 0);
+            EMU68_GUEST_LOAD(ptr, rn1, val1, 2, 0);
+            EMU68_GUEST_LOAD(ptr, rn2, val2, 2, 0);
             *ptr++ = lsl(val1, val1, 16);
             *ptr++ = lsl(val2, val2, 16);
             *ptr++ = subs_reg(31, val1, dc1, LSL, 16);
-            *ptr++ = b_cc(A64_CC_NE, 6);
+            uint32_t *first_failed = ptr++;
             *ptr++ = subs_reg(31, val2, dc2, LSL, 16);
-            *ptr++ = b_cc(A64_CC_NE, 4);
+            uint32_t *second_failed = ptr++;
             // 68040 stores du2 first, then du1
-            *ptr++ = strh_offset(rn2, du2, 0);
-            *ptr++ = strh_offset(rn1, du1, 0);
-            *ptr++ = b(3);
+            EMU68_GUEST_STORE(ptr, rn2, du2, 2);
+            EMU68_GUEST_STORE(ptr, rn1, du1, 2);
+            uint32_t *success = ptr++;
+            uint32_t *failed = ptr;
             *ptr++ = bfxil(dc1, val1, 16, 16);
             *ptr++ = bfxil(dc2, val2, 16, 16);
+            uint32_t *done = ptr;
+            *first_failed = b_cc(A64_CC_NE, failed - first_failed);
+            *second_failed = b_cc(A64_CC_NE, failed - second_failed);
+            *success = b(done - success);
             RA_FreeARMRegister(&ptr, tmp1);
             RA_FreeARMRegister(&ptr, tmp2);
         }
         else
         {
-            *ptr++ = ldr_offset(rn1, val1, 0);
-            *ptr++ = ldr_offset(rn2, val2, 0);
+            EMU68_GUEST_LOAD(ptr, rn1, val1, 4, 0);
+            EMU68_GUEST_LOAD(ptr, rn2, val2, 4, 0);
             *ptr++ = subs_reg(31, val1, dc1, LSL, 0);
-            *ptr++ = b_cc(A64_CC_NE, 6);
+            uint32_t *first_failed = ptr++;
             *ptr++ = subs_reg(31, val2, dc2, LSL, 0);
-            *ptr++ = b_cc(A64_CC_NE, 4);
+            uint32_t *second_failed = ptr++;
             // 68040 stores du2 first, then du1
-            *ptr++ = str_offset(rn2, du2, 0);
-            *ptr++ = str_offset(rn1, du1, 0);
-            *ptr++ = b(3);
+            EMU68_GUEST_STORE(ptr, rn2, du2, 4);
+            EMU68_GUEST_STORE(ptr, rn1, du1, 4);
+            uint32_t *success = ptr++;
+            uint32_t *failed = ptr;
             *ptr++ = mov_reg(dc1, val1);
             *ptr++ = mov_reg(dc2, val2);
+            uint32_t *done = ptr;
+            *first_failed = b_cc(A64_CC_NE, failed - first_failed);
+            *second_failed = b_cc(A64_CC_NE, failed - second_failed);
+            *success = b(done - success);
         }
 
         *ptr++ = dmb_ish();
@@ -2450,6 +2477,9 @@ uint32_t *EMIT_CAS(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
         }
 
+#ifdef BELLATRIX
+        CAS_UNSAFE();
+#else
         if (size == 1)
         {
             CAS_ATOMIC();
@@ -2514,6 +2544,7 @@ uint32_t *EMIT_CAS(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
             *b_ = b(ptr - b_);
             *b_eq = b_cc(A64_CC_EQ, 1 + b_ - b_eq);
         }
+#endif
 
         *ptr++ = dmb_ish();
 
@@ -2623,18 +2654,18 @@ uint32_t *EMIT_MOVEP(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         /* Long mode */
         if (opcode & 0x40) {
             *ptr++ = lsr(tmp, dn, 24);
-            *ptr++ = strb_offset(addr, tmp, offset);
+            EMU68_GUEST_STORE_OFFSET(ptr, addr, offset, tmp, 1);
             *ptr++ = lsr(tmp, dn, 16);
-            *ptr++ = strb_offset(addr, tmp, offset + 2);
+            EMU68_GUEST_STORE_OFFSET(ptr, addr, offset + 2, tmp, 1);
             *ptr++ = lsr(tmp, dn, 8);
-            *ptr++ = strb_offset(addr, tmp, offset + 4);
-            *ptr++ = strb_offset(addr, dn, offset + 6);
+            EMU68_GUEST_STORE_OFFSET(ptr, addr, offset + 4, tmp, 1);
+            EMU68_GUEST_STORE_OFFSET(ptr, addr, offset + 6, dn, 1);
         }
         /* Word mode */
         else {
             *ptr++ = lsr(tmp, dn, 8);
-            *ptr++ = strb_offset(addr, tmp, offset);
-            *ptr++ = strb_offset(addr, dn, offset + 2);
+            EMU68_GUEST_STORE_OFFSET(ptr, addr, offset, tmp, 1);
+            EMU68_GUEST_STORE_OFFSET(ptr, addr, offset + 2, dn, 1);
         }
     }
     /* Memory to register transfer */
@@ -2643,21 +2674,21 @@ uint32_t *EMIT_MOVEP(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         
         /* Long mode */
         if (opcode & 0x40) {
-            *ptr++ = ldrb_offset(addr, dn, offset);
-            *ptr++ = ldrb_offset(addr, tmp, offset + 2);
+            EMU68_GUEST_LOAD_OFFSET(ptr, addr, offset, dn, 1, 0);
+            EMU68_GUEST_LOAD_OFFSET(ptr, addr, offset + 2, tmp, 1, 0);
             *ptr++ = lsl(dn, dn, 24);
             *ptr++ = orr_reg(dn, dn, tmp, LSL, 16);
-            *ptr++ = ldrb_offset(addr, tmp, offset + 4);
+            EMU68_GUEST_LOAD_OFFSET(ptr, addr, offset + 4, tmp, 1, 0);
             *ptr++ = orr_reg(dn, dn, tmp, LSL, 8);
-            *ptr++ = ldrb_offset(addr, tmp, offset + 6);
+            EMU68_GUEST_LOAD_OFFSET(ptr, addr, offset + 6, tmp, 1, 0);
             *ptr++ = orr_reg(dn, dn, tmp, LSL, 0);
         }
         /* Word mode */
         else {
             *ptr++ = bic_immed(dn, dn, 16, 0);
-            *ptr++ = ldrb_offset(addr, tmp, offset);
+            EMU68_GUEST_LOAD_OFFSET(ptr, addr, offset, tmp, 1, 0);
             *ptr++ = orr_reg(dn, dn, tmp, LSL, 8);
-            *ptr++ = ldrb_offset(addr, tmp, offset + 2);
+            EMU68_GUEST_LOAD_OFFSET(ptr, addr, offset + 2, tmp, 1, 0);
             *ptr++ = orr_reg(dn, dn, tmp, LSL, 0);
         }
     }
@@ -2694,6 +2725,10 @@ uint32_t *EMIT_MOVES(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
     // Transfer from Register to EA
     if (opcode2 & (1 << 11)) {
 
+#ifdef BELLATRIX
+        emu68_machine_translation_metadata = EMU68_MACHINE_META_USE_DFC;
+#endif
+
         if (((opcode & 0x38) == 0x18) && (8 + (opcode & 7)) == (opcode2 >> 12))
         {
             uint8_t tmpreg = RA_AllocARMRegister(&ptr);
@@ -2715,6 +2750,9 @@ uint32_t *EMIT_MOVES(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
     }
     // Transfer from EA to Register
     else {
+#ifdef BELLATRIX
+        emu68_machine_translation_metadata = EMU68_MACHINE_META_USE_SFC;
+#endif
         RA_SetDirtyM68kRegister(&ptr, opcode2 >> 12);
         if (size == 4)
             ptr = EMIT_LoadFromEffectiveAddress(ptr, size, &reg, opcode & 0x3f, *m68k_ptr, &ext_count, 0, NULL);
@@ -2750,6 +2788,10 @@ uint32_t *EMIT_MOVES(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr)
         }
     }
 
+#ifdef BELLATRIX
+    emu68_machine_translation_metadata = 0u;
+#endif
+
     RA_FreeARMRegister(&ptr, reg);
 
     *ptr++ = add_immed(REG_PC, REG_PC, 2 * (ext_count + 1));
```

### `0032-emu68-explicit-bitfield-access.patch`

```diff
diff --git a/src/M68k_LINEE.c b/src/M68k_LINEE.c
index ec68621..f6dce91 100644
--- a/src/M68k_LINEE.c
+++ b/src/M68k_LINEE.c
@@ -24,10 +24,10 @@ static uint32_t *EMIT_ASL_mem(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
 
     /* Pre-decrement mode */
     if ((opcode & 0x38) == 0x20) {
-        *ptr++ = ldrsh_offset_preindex(dest, tmp, -2);
+        EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 1, -2);
     }
     else {
-        *ptr++ = ldrsh_offset(dest, tmp, 0);
+        EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 1);
     }
 
     if (update_mask & (SR_C | SR_X)) {
@@ -49,10 +49,10 @@ static uint32_t *EMIT_ASL_mem(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
     }
 
     if ((opcode & 0x38) == 0x18) {
-        *ptr++ = strh_offset_postindex(dest, tmp, 2);
+        EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
     }
     else {
-        *ptr++ = strh_offset(dest, tmp, 0);
+        EMU68_GUEST_STORE(ptr, dest, tmp, 2);
     }
 
     ptr = EMIT_AdvancePC(ptr, 2 * (ext_words + 1));
@@ -116,10 +116,10 @@ static uint32_t *EMIT_LSL_mem(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
 
     /* Pre-decrement mode */
     if ((opcode & 0x38) == 0x20) {
-        *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+        EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
     }
     else {
-        *ptr++ = ldrh_offset(dest, tmp, 0);
+        EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
     }
 
     if (update_mask & (SR_C | SR_X)) {
@@ -141,10 +141,10 @@ static uint32_t *EMIT_LSL_mem(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
     }
 
     if ((opcode & 0x38) == 0x18) {
-        *ptr++ = strh_offset_postindex(dest, tmp, 2);
+        EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
     }
     else {
-        *ptr++ = strh_offset(dest, tmp, 0);
+        EMU68_GUEST_STORE(ptr, dest, tmp, 2);
     }
         
     ptr = EMIT_AdvancePC(ptr, 2 * (ext_words + 1));
@@ -201,10 +201,10 @@ static uint32_t *EMIT_ROXL_mem(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_p
     uint8_t cc = RA_ModifyCC(&ptr);
 
     if ((opcode & 0x38) == 0x20) {
-        *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+        EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
     }
     else {
-        *ptr++ = ldrh_offset(dest, tmp, 0);
+        EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
     }
 
     /* Test X flag, push the flag value into tmp register */
@@ -221,10 +221,10 @@ static uint32_t *EMIT_ROXL_mem(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_p
     }
 
     if ((opcode & 0x38) == 0x18) {
-        *ptr++ = strh_offset_postindex(dest, tmp, 2);
+        EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
     }
     else {
-        *ptr++ = strh_offset(dest, tmp, 0);
+        EMU68_GUEST_STORE(ptr, dest, tmp, 2);
     }
 
     ptr = EMIT_AdvancePC(ptr, 2 * (ext_words + 1));
@@ -279,10 +279,10 @@ static uint32_t *EMIT_ROL_mem(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
     ptr = EMIT_LoadFromEffectiveAddress(ptr, 0, &dest, opcode & 0x3f, *m68k_ptr, &ext_words, 1, NULL);
 
     if ((opcode & 0x38) == 0x20) {
-        *ptr++ = ldrh_offset_preindex(dest, tmp, -2);
+        EMU68_GUEST_PRELOAD(ptr, dest, tmp, 2, 0, -2);
     }
     else {
-        *ptr++ = ldrh_offset(dest, tmp, 0);
+        EMU68_GUEST_LOAD(ptr, dest, tmp, 2, 0);
     }
     *ptr++ = bfi(tmp, tmp, 16, 16);
 
@@ -296,10 +296,10 @@ static uint32_t *EMIT_ROL_mem(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_pt
     }
 
     if ((opcode & 0x38) == 0x18) {
-        *ptr++ = strh_offset_postindex(dest, tmp, 2);
+        EMU68_GUEST_POSTSTORE(ptr, dest, tmp, 2, 2);
     }
     else {
-        *ptr++ = strh_offset(dest, tmp, 0);
+        EMU68_GUEST_STORE(ptr, dest, tmp, 2);
     }
 
     ptr = EMIT_AdvancePC(ptr, 2 * (ext_words + 1));
@@ -1625,28 +1625,28 @@ static inline uint32_t *EMIT_BFxxx_II(uint32_t *ptr, uint8_t base, enum BF_OP op
     /* IF bit offset + width <= 8, fetch a byte */
     if ((bit_offset + width) <= 8)
     {
-        *ptr++ = ldurb_offset(base, data_reg, base_offset);
+        EMU68_GUEST_LOAD_OFFSET(ptr, base, base_offset, data_reg, 1, 0);
         fetched_size = 1;
         data_offset = 56;
     }
     /* bit offset + width <= 16: fetch a word */
     else if ((bit_offset + width) <= 16)
     {
-        *ptr++ = ldurh_offset(base, data_reg, base_offset);
+        EMU68_GUEST_LOAD_OFFSET(ptr, base, base_offset, data_reg, 2, 0);
         fetched_size = 2;
         data_offset = 48;
     }
     /* bit offset + width <= 32: fetch a long */
     else if ((bit_offset + width) <= 32)
     {
-        *ptr++ = ldur_offset(base, data_reg, base_offset);
+        EMU68_GUEST_LOAD_OFFSET(ptr, base, base_offset, data_reg, 4, 0);
         fetched_size = 4;
         data_offset = 32;
     }
     /* Worst case otherwise - fetch 64bit */
     else
     {
-        *ptr++ = ldur64_offset(base, data_reg, base_offset);
+        EMU68_GUEST_LOAD_OFFSET(ptr, base, base_offset, data_reg, 8, 0);
         fetched_size = 8;
     }
 
@@ -1753,16 +1753,20 @@ static inline uint32_t *EMIT_BFxxx_II(uint32_t *ptr, uint8_t base, enum BF_OP op
             switch (fetched_size)
             {
                 case 1:
-                    *ptr++ = sturb_offset(base, data_reg, base_offset);
+                    EMU68_GUEST_STORE_OFFSET(ptr, base, base_offset,
+                                             data_reg, 1);
                     break;
                 case 2:
-                    *ptr++ = sturh_offset(base, data_reg, base_offset);
+                    EMU68_GUEST_STORE_OFFSET(ptr, base, base_offset,
+                                             data_reg, 2);
                     break;
                 case 4:
-                    *ptr++ = stur_offset(base, data_reg, base_offset);
+                    EMU68_GUEST_STORE_OFFSET(ptr, base, base_offset,
+                                             data_reg, 4);
                     break;
                 case 8:
-                    *ptr++ = stur64_offset(base, data_reg, base_offset);
+                    EMU68_GUEST_STORE_OFFSET(ptr, base, base_offset,
+                                             data_reg, 8);
                     break;
             }
         }
@@ -1793,26 +1797,36 @@ static inline uint32_t *EMIT_BFxxx_IR(uint32_t *ptr, uint8_t base, enum BF_OP op
     /* Move mask to the topmost bits of 64-bit mask_reg */
     *ptr++ = rbit64(mask_reg, mask_reg);
 
-    /* Fetch the data */
-    /* Width == 1? Fetch byte */
+    /* Fetch the smallest container that covers the runtime width. */
     *ptr++ = cmp_immed(width_reg, 1);
-    *ptr++ = b_cc(A64_CC_NE, 4);
-    *ptr++ = ldurb_offset(base, data_reg, base_offset);
+    uint32_t *not_byte = ptr++;
+    EMU68_GUEST_LOAD_OFFSET(ptr, base, base_offset, data_reg, 1, 0);
     *ptr++ = ror64(data_reg, data_reg, 8);
-    *ptr++ = b(12);
-    /* Width <= 8? Fetch half word */
+    uint32_t *byte_done = ptr++;
+
+    uint32_t *half_label = ptr;
     *ptr++ = cmp_immed(width_reg, 8);
-    *ptr++ = b_cc(A64_CC_GT, 4);
-    *ptr++ = ldurh_offset(base, data_reg, base_offset);
+    uint32_t *not_half = ptr++;
+    EMU68_GUEST_LOAD_OFFSET(ptr, base, base_offset, data_reg, 2, 0);
     *ptr++ = ror64(data_reg, data_reg, 16);
-    *ptr++ = b(7);
-    /* Width <= 24? Fetch long word */
+    uint32_t *half_done = ptr++;
+
+    uint32_t *long_label = ptr;
     *ptr++ = cmp_immed(width_reg, 24);
-    *ptr++ = b_cc(A64_CC_GT, 4);
-    *ptr++ = ldur_offset(base, data_reg, base_offset);
+    uint32_t *not_long = ptr++;
+    EMU68_GUEST_LOAD_OFFSET(ptr, base, base_offset, data_reg, 4, 0);
     *ptr++ = ror64(data_reg, data_reg, 32);
-    *ptr++ = b(2);
-    *ptr++ = ldur64_offset(base, data_reg, base_offset);
+    uint32_t *long_done = ptr++;
+
+    uint32_t *quad_label = ptr;
+    EMU68_GUEST_LOAD_OFFSET(ptr, base, base_offset, data_reg, 8, 0);
+    uint32_t *load_done = ptr;
+    *not_byte = b_cc(A64_CC_NE, half_label - not_byte);
+    *not_half = b_cc(A64_CC_GT, long_label - not_half);
+    *not_long = b_cc(A64_CC_GT, quad_label - not_long);
+    *byte_done = b(load_done - byte_done);
+    *half_done = b(load_done - half_done);
+    *long_done = b(load_done - long_done);
 
     /* In case of INS, prepare the source data accordingly */
     if (op == OP_INS)
@@ -1920,26 +1934,39 @@ static inline uint32_t *EMIT_BFxxx_IR(uint32_t *ptr, uint8_t base, enum BF_OP op
 
         if (op != OP_EXTS && op != OP_EXTU && op != OP_FFO)
         {
-            /* Store the data back */
-            /* Width == 1? Fetch byte */
+            /* Store the selected container back. */
             *ptr++ = cmp_immed(width_reg, 1);
-            *ptr++ = b_cc(A64_CC_NE, 4);
+            uint32_t *store_not_byte = ptr++;
             *ptr++ = ror64(data_reg, data_reg, 64 - 8);
-            *ptr++ = sturb_offset(base, data_reg, base_offset);
-            *ptr++ = b(12);
-            /* Width <= 8? Fetch half word */
+            EMU68_GUEST_STORE_OFFSET(ptr, base, base_offset, data_reg, 1);
+            uint32_t *store_byte_done = ptr++;
+
+            uint32_t *store_half_label = ptr;
             *ptr++ = cmp_immed(width_reg, 8);
-            *ptr++ = b_cc(A64_CC_GT, 4);
+            uint32_t *store_not_half = ptr++;
             *ptr++ = ror64(data_reg, data_reg, 64 - 16);
-            *ptr++ = sturh_offset(base, data_reg, base_offset);
-            *ptr++ = b(7);
-            /* Width <= 24? Fetch long word */
+            EMU68_GUEST_STORE_OFFSET(ptr, base, base_offset, data_reg, 2);
+            uint32_t *store_half_done = ptr++;
+
+            uint32_t *store_long_label = ptr;
             *ptr++ = cmp_immed(width_reg, 24);
-            *ptr++ = b_cc(A64_CC_GT, 4);
+            uint32_t *store_not_long = ptr++;
             *ptr++ = ror64(data_reg, data_reg, 32);
-            *ptr++ = stur_offset(base, data_reg, base_offset);
-            *ptr++ = b(2);
-            *ptr++ = stur64_offset(base, data_reg, base_offset);
+            EMU68_GUEST_STORE_OFFSET(ptr, base, base_offset, data_reg, 4);
+            uint32_t *store_long_done = ptr++;
+
+            uint32_t *store_quad_label = ptr;
+            EMU68_GUEST_STORE_OFFSET(ptr, base, base_offset, data_reg, 8);
+            uint32_t *store_done = ptr;
+            *store_not_byte = b_cc(A64_CC_NE,
+                                   store_half_label - store_not_byte);
+            *store_not_half = b_cc(A64_CC_GT,
+                                   store_long_label - store_not_half);
+            *store_not_long = b_cc(A64_CC_GT,
+                                   store_quad_label - store_not_long);
+            *store_byte_done = b(store_done - store_byte_done);
+            *store_half_done = b(store_done - store_half_done);
+            *store_long_done = b(store_done - store_long_done);
         }
     }
 
@@ -1982,7 +2009,7 @@ static inline uint32_t *EMIT_BFxxx_RI(uint32_t *ptr, uint8_t base, enum BF_OP op
         *ptr++ = orr_immed(mask_reg, 31, width, 24 + width);
 
         // Load data 
-        *ptr++ = ldrb_offset(base, tmp, 0);
+        EMU68_GUEST_LOAD(ptr, base, tmp, 1, 0);
 
         // Shift mask to correct position
         *ptr++ = lsrv(mask_reg, mask_reg, off_reg);
@@ -2114,7 +2141,7 @@ static inline uint32_t *EMIT_BFxxx_RI(uint32_t *ptr, uint8_t base, enum BF_OP op
             if (op != OP_EXTS && op != OP_EXTU && op != OP_FFO)
             {
                 // Store back
-                *ptr++ = strb_offset(base, tmp, 0);
+                EMU68_GUEST_STORE(ptr, base, tmp, 1);
             }
         }
     }
@@ -2124,7 +2151,7 @@ static inline uint32_t *EMIT_BFxxx_RI(uint32_t *ptr, uint8_t base, enum BF_OP op
         *ptr++ = orr_immed(mask_reg, 31, width, 16 + width);
 
         // Load data 
-        *ptr++ = ldrh_offset(base, tmp, 0);
+        EMU68_GUEST_LOAD(ptr, base, tmp, 2, 0);
 
         if (op == OP_INS)
         {
@@ -2259,7 +2286,7 @@ static inline uint32_t *EMIT_BFxxx_RI(uint32_t *ptr, uint8_t base, enum BF_OP op
             if (op != OP_EXTS && op != OP_EXTU && op != OP_FFO)
             {
                 // Store back
-                *ptr++ = strh_offset(base, tmp, 0);
+                EMU68_GUEST_STORE(ptr, base, tmp, 2);
             }
         }
     }
@@ -2269,7 +2296,7 @@ static inline uint32_t *EMIT_BFxxx_RI(uint32_t *ptr, uint8_t base, enum BF_OP op
         *ptr++ = orr_immed(mask_reg, 31, width, width);
 
         // Load data 
-        *ptr++ = ldr_offset(base, tmp, 0);
+        EMU68_GUEST_LOAD(ptr, base, tmp, 4, 0);
 
         if (op == OP_INS)
         {
@@ -2401,7 +2428,7 @@ static inline uint32_t *EMIT_BFxxx_RI(uint32_t *ptr, uint8_t base, enum BF_OP op
             if (op != OP_EXTS && op != OP_EXTU && op != OP_FFO)
             {
                 // Store back
-                *ptr++ = str_offset(base, tmp, 0);
+                EMU68_GUEST_STORE(ptr, base, tmp, 4);
             }
         }
     }
@@ -2411,7 +2438,7 @@ static inline uint32_t *EMIT_BFxxx_RI(uint32_t *ptr, uint8_t base, enum BF_OP op
         *ptr++ = orr64_immed(mask_reg, 31, width, width, 1);
 
         // Load data and shift it left according to reminder in offset reg
-        *ptr++ = ldr64_offset(base, tmp, 0);
+        EMU68_GUEST_LOAD(ptr, base, tmp, 8, 0);
 
         if (op == OP_INS)
         {
@@ -2550,7 +2577,7 @@ static inline uint32_t *EMIT_BFxxx_RI(uint32_t *ptr, uint8_t base, enum BF_OP op
             if (op != OP_EXTS && op != OP_EXTU && op != OP_FFO)
             {
                 // Store back
-                *ptr++ = str64_offset(base, tmp, 0);
+                EMU68_GUEST_STORE(ptr, base, tmp, 8);
             }
         }
     }
@@ -2601,26 +2628,36 @@ static inline uint32_t *EMIT_BFxxx_RR(uint32_t *ptr, uint8_t base, enum BF_OP op
     *ptr++ = add_reg(base, base_orig, off_reg_orig, ASR, 3);
     *ptr++ = and_immed(off_reg, off_reg_orig, 3, 0);
     
-    /* Fetch the data */
-    /* Width == 1? Fetch byte */
+    /* Fetch the smallest container that covers the runtime width. */
     *ptr++ = cmp_immed(width_reg, 1);
-    *ptr++ = b_cc(A64_CC_NE, 4);
-    *ptr++ = ldrb_offset(base, data_reg, 0);
+    uint32_t *not_byte = ptr++;
+    EMU68_GUEST_LOAD(ptr, base, data_reg, 1, 0);
     *ptr++ = ror64(data_reg, data_reg, 8);
-    *ptr++ = b(12);
-    /* Width <= 8? Fetch half word */
+    uint32_t *byte_done = ptr++;
+
+    uint32_t *half_label = ptr;
     *ptr++ = cmp_immed(width_reg, 8);
-    *ptr++ = b_cc(A64_CC_GT, 4);
-    *ptr++ = ldrh_offset(base, data_reg, 0);
+    uint32_t *not_half = ptr++;
+    EMU68_GUEST_LOAD(ptr, base, data_reg, 2, 0);
     *ptr++ = ror64(data_reg, data_reg, 16);
-    *ptr++ = b(7);
-    /* Width <= 24? Fetch long word */
+    uint32_t *half_done = ptr++;
+
+    uint32_t *long_label = ptr;
     *ptr++ = cmp_immed(width_reg, 24);
-    *ptr++ = b_cc(A64_CC_GT, 4);
-    *ptr++ = ldr_offset(base, data_reg, 0);
+    uint32_t *not_long = ptr++;
+    EMU68_GUEST_LOAD(ptr, base, data_reg, 4, 0);
     *ptr++ = ror64(data_reg, data_reg, 32);
-    *ptr++ = b(2);
-    *ptr++ = ldr64_offset(base, data_reg, 0);
+    uint32_t *long_done = ptr++;
+
+    uint32_t *quad_label = ptr;
+    EMU68_GUEST_LOAD(ptr, base, data_reg, 8, 0);
+    uint32_t *load_done = ptr;
+    *not_byte = b_cc(A64_CC_NE, half_label - not_byte);
+    *not_half = b_cc(A64_CC_GT, long_label - not_half);
+    *not_long = b_cc(A64_CC_GT, quad_label - not_long);
+    *byte_done = b(load_done - byte_done);
+    *half_done = b(load_done - half_done);
+    *long_done = b(load_done - long_done);
 
     /* In case of INS, prepare the source data accordingly */
     if (op == OP_INS)
@@ -2736,26 +2773,39 @@ static inline uint32_t *EMIT_BFxxx_RR(uint32_t *ptr, uint8_t base, enum BF_OP op
 
         if (op != OP_EXTS && op != OP_EXTU && op != OP_FFO)
         {
-            /* Store the data back */
-            /* Width == 1? Fetch byte */
+            /* Store the selected container back. */
             *ptr++ = cmp_immed(width_reg, 1);
-            *ptr++ = b_cc(A64_CC_NE, 4);
+            uint32_t *store_not_byte = ptr++;
             *ptr++ = ror64(data_reg, data_reg, 64 - 8);
-            *ptr++ = strb_offset(base, data_reg, 0);
-            *ptr++ = b(12);
-            /* Width <= 8? Fetch half word */
+            EMU68_GUEST_STORE(ptr, base, data_reg, 1);
+            uint32_t *store_byte_done = ptr++;
+
+            uint32_t *store_half_label = ptr;
             *ptr++ = cmp_immed(width_reg, 8);
-            *ptr++ = b_cc(A64_CC_GT, 4);
+            uint32_t *store_not_half = ptr++;
             *ptr++ = ror64(data_reg, data_reg, 64 - 16);
-            *ptr++ = strh_offset(base, data_reg, 0);
-            *ptr++ = b(7);
-            /* Width <= 24? Fetch long word */
+            EMU68_GUEST_STORE(ptr, base, data_reg, 2);
+            uint32_t *store_half_done = ptr++;
+
+            uint32_t *store_long_label = ptr;
             *ptr++ = cmp_immed(width_reg, 24);
-            *ptr++ = b_cc(A64_CC_GT, 4);
+            uint32_t *store_not_long = ptr++;
             *ptr++ = ror64(data_reg, data_reg, 32);
-            *ptr++ = str_offset(base, data_reg, 0);
-            *ptr++ = b(2);
-            *ptr++ = str64_offset(base, data_reg, 0);
+            EMU68_GUEST_STORE(ptr, base, data_reg, 4);
+            uint32_t *store_long_done = ptr++;
+
+            uint32_t *store_quad_label = ptr;
+            EMU68_GUEST_STORE(ptr, base, data_reg, 8);
+            uint32_t *store_done = ptr;
+            *store_not_byte = b_cc(A64_CC_NE,
+                                   store_half_label - store_not_byte);
+            *store_not_half = b_cc(A64_CC_GT,
+                                   store_long_label - store_not_half);
+            *store_not_long = b_cc(A64_CC_GT,
+                                   store_quad_label - store_not_long);
+            *store_byte_done = b(store_done - store_byte_done);
+            *store_half_done = b(store_done - store_half_done);
+            *store_long_done = b(store_done - store_long_done);
         }
     }
 
@@ -5420,4 +5470,4 @@ int M68K_GetLineELength(uint16_t *insn_stream)
     }
 
     return length;
-}
\ No newline at end of file
+}
```

### `0033-emu68-explicit-fpu-access.patch`

```diff
diff --git a/src/M68k_LINEF.c b/src/M68k_LINEF.c
index 0b6690e..1de22ba 100644
--- a/src/M68k_LINEF.c
+++ b/src/M68k_LINEF.c
@@ -22,12 +22,65 @@ extern uint32_t val_FPIAR;
 
 uint64_t Load96bit(uintptr_t __ignore, uintptr_t base);
 uint64_t Store96bit(uintptr_t value, uintptr_t base);
+uint64_t Load96Values(uint16_t exp, uint64_t mant);
+struct FP96Values {
+    uint64_t first;
+    uint32_t last;
+};
+struct FP96Values Store96Values(uint64_t value);
+
+static uint32_t *emit_adjust_guest_address(uint32_t *ptr, uint8_t address,
+                                           int32_t delta)
+{
+    if (delta < 0)
+        *ptr++ = sub_immed(address, address, (uint16_t)-delta);
+    else if (delta > 0)
+        *ptr++ = add_immed(address, address, (uint16_t)delta);
+    return ptr;
+}
+
+static uint32_t *emit_guest_fp_load(uint32_t *ptr, uint8_t address,
+                                    int32_t offset, uint8_t fp_reg,
+                                    uint8_t width, int32_t pre,
+                                    int32_t post, uint32_t metadata)
+{
+    uint8_t value = RA_AllocARMRegister(&ptr);
+
+    ptr = emit_adjust_guest_address(ptr, address, pre);
+#ifdef BELLATRIX
+    ptr = emu68_machine_emit_load_offset(ptr, address, offset, value, width,
+                                          0, metadata);
+#else
+    (void)metadata;
+    EMU68_GUEST_LOAD_OFFSET(ptr, address, offset, value, width, 0);
+#endif
+    *ptr++ = mov_reg_to_simd(fp_reg, width == 8 ? TS_D : TS_S, 0, value);
+    ptr = emit_adjust_guest_address(ptr, address, post);
+    RA_FreeARMRegister(&ptr, value);
+    return ptr;
+}
+
+static uint32_t *emit_guest_fp_store(uint32_t *ptr, uint8_t address,
+                                     int32_t offset, uint8_t fp_reg,
+                                     uint8_t width, int32_t pre,
+                                     int32_t post)
+{
+    uint8_t value = RA_AllocARMRegister(&ptr);
+
+    *ptr++ = mov_simd_to_reg(value, fp_reg,
+                             width == 8 ? TS_D : TS_S, 0);
+    ptr = emit_adjust_guest_address(ptr, address, pre);
+    EMU68_GUEST_STORE_OFFSET(ptr, address, offset, value, width);
+    ptr = emit_adjust_guest_address(ptr, address, post);
+    RA_FreeARMRegister(&ptr, value);
+    return ptr;
+}
 
 uint32_t * get_Load96(uint32_t *ptr)
 {
     if (reg_Load96 == 0xff) {
         reg_Load96 = RA_AllocARMRegister(&ptr);
-        uint32_t val = (uintptr_t)Load96bit;
+        uint32_t val = (uintptr_t)Load96Values;
 
         *ptr++ = mov_immed_u16(reg_Load96, val & 0xffff, 0);
         *ptr++ = movk_immed_u16(reg_Load96, val >> 16, 1);
@@ -41,7 +94,7 @@ uint32_t * get_Save96(uint32_t *ptr)
 {
     if (reg_Save96 == 0xff) {
         reg_Save96 = RA_AllocARMRegister(&ptr);
-        uint32_t val = (uintptr_t)Store96bit;
+        uint32_t val = (uintptr_t)Store96Values;
 
         *ptr++ = mov_immed_u16(reg_Save96, val & 0xffff, 0);
         *ptr++ = movk_immed_u16(reg_Save96, val >> 16, 1);
@@ -50,6 +103,32 @@ uint32_t * get_Save96(uint32_t *ptr)
     return ptr;
 }
 
+static uint32_t *emit_guest_load96(uint32_t *ptr, uint8_t address,
+                                   int32_t offset, uint8_t fp_reg)
+{
+    ptr = get_Load96(ptr);
+    EMU68_GUEST_LOAD_OFFSET(ptr, address, offset, 0, 2, 0);
+    EMU68_GUEST_LOAD_OFFSET(ptr, address, offset + 4, 1, 8, 0);
+    *ptr++ = str64_offset_preindex(31, 30, -16);
+    *ptr++ = blr(reg_Load96);
+    *ptr++ = mov_reg_to_simd(fp_reg, TS_D, 0, 0);
+    *ptr++ = ldr64_offset_postindex(31, 30, 16);
+    return ptr;
+}
+
+static uint32_t *emit_guest_store96(uint32_t *ptr, uint8_t address,
+                                    int32_t offset, uint8_t fp_reg)
+{
+    ptr = get_Save96(ptr);
+    *ptr++ = stp64_preindex(31, address, 30, -16);
+    *ptr++ = mov_simd_to_reg(0, fp_reg, TS_D, 0);
+    *ptr++ = blr(reg_Save96);
+    *ptr++ = ldp64_postindex(31, address, 30, 16);
+    EMU68_GUEST_STORE_OFFSET(ptr, address, offset, 0, 8);
+    EMU68_GUEST_STORE_OFFSET(ptr, address, offset + 8, 1, 4);
+    return ptr;
+}
+
 enum {
     C_PI = 0,
     C_PI_2,
@@ -615,7 +694,9 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
                 {
                     int8_t off = 4;
                     ptr = EMIT_GetOffsetPC(ptr, &off);
-                    *ptr++ = flds(*reg, REG_PC, off);
+                    ptr = emit_guest_fp_load(
+                        ptr, REG_PC, off, *reg, 4, 0, 0,
+                        EMU68_MACHINE_META_PROGRAM);
                     *ptr++ = fcvtds(*reg, *reg);
                     *ext_count += 2;
                     break;
@@ -652,7 +733,9 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
                 {
                     int8_t off = 4;
                     ptr = EMIT_GetOffsetPC(ptr, &off);
-                    *ptr++ = fldd(*reg, REG_PC, off);
+                    ptr = emit_guest_fp_load(
+                        ptr, REG_PC, off, *reg, 8, 0, 0,
+                        EMU68_MACHINE_META_PROGRAM);
                     *ext_count += 4;
                     break;
                 }
@@ -669,17 +752,13 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
                 switch(size)
                 {
                     case SIZE_D:
-                        *ptr++ = fldd(*reg, int_reg, 0);
+                        ptr = emit_guest_fp_load(ptr, int_reg, 0, *reg, 8,
+                                                 0, 0, 0u);
                         *ext_count += 4;
                         break;
 
                     case SIZE_X:
-                        ptr = get_Load96(ptr);
-                        *ptr++ = str64_offset_preindex(31, 30, -16);
-                        *ptr++ = mov_reg(1, int_reg);
-                        *ptr++ = blr(reg_Load96);
-                        *ptr++ = mov_reg_to_simd(*reg, TS_D, 0, 0);
-                        *ptr++ = ldr64_offset_postindex(31, 30, 16);
+                        ptr = emit_guest_load96(ptr, int_reg, 0, *reg);
                         *ext_count += 6;
                         break;
 
@@ -688,8 +767,8 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
 
                         ptr = EMIT_SaveRegFrame(ptr, (RA_GetTempAllocMask() | REG_PROTECT | 7));
 
-                        *ptr++ = ldr64_offset(int_reg, 0, 0);
-                        *ptr++ = ldr64_offset(int_reg, 1, 8);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, int_reg, 0, 0, 8, 0);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, int_reg, 8, 1, 4, 0);
                         
                         *ptr++ = mov64_immed_u16(2, u.u16[3], 0);
                         *ptr++ = movk64_immed_u16(2, u.u16[2], 1);
@@ -778,8 +857,10 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
 
                         ptr = EMIT_SaveRegFrame(ptr, (RA_GetTempAllocMask() | REG_PROTECT | 7));
 
-                        *ptr++ = ldur64_offset(int_reg, 0, imm_offset);
-                        *ptr++ = ldur64_offset(int_reg, 1, imm_offset + 8);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, int_reg, imm_offset,
+                                                0, 8, 0);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, int_reg, imm_offset + 8,
+                                                1, 4, 0);
                         
                         *ptr++ = mov64_immed_u16(2, u.u16[3], 0);
                         *ptr++ = movk64_immed_u16(2, u.u16[2], 1);
@@ -829,15 +910,8 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
                             imm_offset = 0;
                         }
 
-                        ptr = get_Load96(ptr);
-                        *ptr++ = str64_offset_preindex(31, 30, -16);
-                        if (imm_offset < 0)
-                            *ptr++ = sub_immed(1, int_reg, -imm_offset);
-                        else
-                            *ptr++ = add_immed(1, int_reg, imm_offset);
-                        *ptr++ = blr(reg_Load96);
-                        *ptr++ = mov_reg_to_simd(*reg, TS_D, 0, 0);
-                        *ptr++ = ldr64_offset_postindex(31, 30, 16);
+                        ptr = emit_guest_load96(ptr, int_reg, imm_offset,
+                                                *reg);
 
                         //ptr = EMIT_Load96bitFP(ptr, *reg, int_reg, imm_offset);
 
@@ -851,19 +925,23 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
                     {
                         if (pre_sz)
                         {
-                            *ptr++ = fldd_preindex(*reg, int_reg, pre_sz);
+                            ptr = emit_guest_fp_load(ptr, int_reg, 0, *reg, 8,
+                                                     pre_sz, 0, 0u);
                         }
                         else if (post_sz)
                         {
-                            *ptr++ = fldd_postindex(*reg, int_reg, post_sz);
+                            ptr = emit_guest_fp_load(ptr, int_reg, 0, *reg, 8,
+                                                     0, post_sz, 0u);
                         }
                         else if (imm_offset >= -255 && imm_offset <= 255)
                         {
-                            *ptr++ = fldd(*reg, int_reg, imm_offset);
+                            ptr = emit_guest_fp_load(ptr, int_reg, imm_offset,
+                                                     *reg, 8, 0, 0, 0u);
                         }
                         else if (imm_offset >= 0 && imm_offset < 32760 && !(imm_offset & 7))
                         {
-                            *ptr++ = fldd_pimm(*reg, int_reg, imm_offset >> 3);
+                            ptr = emit_guest_fp_load(ptr, int_reg, imm_offset,
+                                                     *reg, 8, 0, 0, 0u);
                         }
                         else
                         {
@@ -884,7 +962,8 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
                                     *ptr++ = movt_immed_u16(off, (imm_offset) & 0xffff);
                                 *ptr++ = add_reg(off, int_reg, off, LSL, 0);
                             }
-                            *ptr++ = fldd(*reg, off, 0);
+                            ptr = emit_guest_fp_load(ptr, off, 0, *reg, 8,
+                                                     0, 0, 0u);
                             RA_FreeARMRegister(&ptr, off);
                         }
                     }
@@ -892,19 +971,23 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
                 case SIZE_S:
                     if (pre_sz)
                     {
-                        *ptr++ = flds_preindex(*reg, int_reg, pre_sz);
+                        ptr = emit_guest_fp_load(ptr, int_reg, 0, *reg, 4,
+                                                 pre_sz, 0, 0u);
                     }
                     else if (post_sz)
                     {
-                        *ptr++ = flds_postindex(*reg, int_reg, post_sz);
+                        ptr = emit_guest_fp_load(ptr, int_reg, 0, *reg, 4,
+                                                 0, post_sz, 0u);
                     }
                     else if (imm_offset >= -255 && imm_offset <= 255)
                     {
-                        *ptr++ = flds(*reg, int_reg, imm_offset);
+                        ptr = emit_guest_fp_load(ptr, int_reg, imm_offset,
+                                                 *reg, 4, 0, 0, 0u);
                     }
                     else if (imm_offset >= 0 && imm_offset < 16380 && !(imm_offset & 3))
                     {
-                        *ptr++ = flds_pimm(*reg, int_reg, imm_offset >> 2);
+                        ptr = emit_guest_fp_load(ptr, int_reg, imm_offset,
+                                                 *reg, 4, 0, 0, 0u);
                     }
                     else
                     {
@@ -925,7 +1008,8 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
                                 *ptr++ = movt_immed_u16(off, (imm_offset) & 0xffff);
                             *ptr++ = add_reg(off, int_reg, off, LSL, 0);
                         }
-                        *ptr++ = flds(*reg, off, 0);
+                        ptr = emit_guest_fp_load(ptr, off, 0, *reg, 4,
+                                                 0, 0, 0u);
                         RA_FreeARMRegister(&ptr, off);
                     }
                     *ptr++ = fcvtds(*reg, *reg);
@@ -935,19 +1019,23 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
 
                     if (pre_sz)
                     {
-                        *ptr++ = ldr_offset_preindex(int_reg, val_reg, pre_sz);
+                        EMU68_GUEST_PRELOAD(ptr, int_reg, val_reg, 4, 0,
+                                            pre_sz);
                     }
                     else if (post_sz)
                     {
-                        *ptr++ = ldr_offset_postindex(int_reg, val_reg, post_sz);
+                        EMU68_GUEST_POSTLOAD(ptr, int_reg, val_reg, 4, 0,
+                                             post_sz);
                     }
                     else if (imm_offset >= -255 && imm_offset <= 255)
                     {
-                        *ptr++ = ldur_offset(int_reg, val_reg, imm_offset);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, int_reg, imm_offset,
+                                                val_reg, 4, 0);
                     }
                     else if (imm_offset >= 0 && imm_offset < 16380 && !(imm_offset & 3))
                     {
-                        *ptr++ = ldr_offset(int_reg, val_reg, imm_offset);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, int_reg, imm_offset,
+                                                val_reg, 4, 0);
                     }
                     else
                     {
@@ -968,7 +1056,7 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
                                 *ptr++ = movt_immed_u16(off, (imm_offset) & 0xffff);
                             *ptr++ = add_reg(off, int_reg, off, LSL, 0);
                         }
-                        *ptr++ = ldr_offset(off, val_reg, 0);
+                        EMU68_GUEST_LOAD(ptr, off, val_reg, 4, 0);
                         RA_FreeARMRegister(&ptr, off);
                     }
                     *ptr++ = scvtf_32toD(*reg, val_reg);
@@ -978,19 +1066,23 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
 
                     if (pre_sz)
                     {
-                        *ptr++ = ldrsh_offset_preindex(int_reg, val_reg, pre_sz);
+                        EMU68_GUEST_PRELOAD(ptr, int_reg, val_reg, 2, 1,
+                                            pre_sz);
                     }
                     else if (post_sz)
                     {
-                        *ptr++ = ldrsh_offset_postindex(int_reg, val_reg, post_sz);
+                        EMU68_GUEST_POSTLOAD(ptr, int_reg, val_reg, 2, 1,
+                                             post_sz);
                     }
                     else if (imm_offset >= -255 && imm_offset <= 255)
                     {
-                        *ptr++ = ldursh_offset(int_reg, val_reg, imm_offset);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, int_reg, imm_offset,
+                                                val_reg, 2, 1);
                     }
                     else if (imm_offset >= 0 && imm_offset < 8190 && !(imm_offset & 1))
                     {
-                        *ptr++ = ldrsh_offset(int_reg, val_reg, imm_offset);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, int_reg, imm_offset,
+                                                val_reg, 2, 1);
                     }
                     else
                     {
@@ -1011,7 +1103,7 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
                                 *ptr++ = movt_immed_u16(off, (imm_offset) & 0xffff);
                             *ptr++ = add_reg(off, int_reg, off, LSL, 0);
                         }
-                        *ptr++ = ldrsh_offset(off, val_reg, 0);
+                        EMU68_GUEST_LOAD(ptr, off, val_reg, 2, 1);
                         RA_FreeARMRegister(&ptr, off);
                     }
                     *ptr++ = scvtf_32toD(*reg, val_reg);
@@ -1021,19 +1113,23 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
 
                     if (pre_sz)
                     {
-                        *ptr++ = ldrsb_offset_preindex(int_reg, val_reg, pre_sz);
+                        EMU68_GUEST_PRELOAD(ptr, int_reg, val_reg, 1, 1,
+                                            pre_sz);
                     }
                     else if (post_sz)
                     {
-                        *ptr++ = ldrsb_offset_postindex(int_reg, val_reg, post_sz);
+                        EMU68_GUEST_POSTLOAD(ptr, int_reg, val_reg, 1, 1,
+                                             post_sz);
                     }
                     else if (imm_offset >= -255 && imm_offset <= 255)
                     {
-                        *ptr++ = ldursb_offset(int_reg, val_reg, imm_offset);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, int_reg, imm_offset,
+                                                val_reg, 1, 1);
                     }
                     else if (imm_offset >= 0 && imm_offset < 4096)
                     {
-                        *ptr++ = ldrsb_offset(int_reg, val_reg, imm_offset);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, int_reg, imm_offset,
+                                                val_reg, 1, 1);
                     }
                     else
                     {
@@ -1054,7 +1150,7 @@ uint32_t *FPU_FetchData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t *reg, uint16
                                 *ptr++ = movt_immed_u16(off, (imm_offset) & 0xffff);
                             *ptr++ = add_reg(off, int_reg, off, LSL, 0);
                         }
-                        *ptr++ = ldrsb_offset(off, val_reg, 0);
+                        EMU68_GUEST_LOAD(ptr, off, val_reg, 1, 1);
                         RA_FreeARMRegister(&ptr, off);
                     }
                     *ptr++ = scvtf_32toD(*reg, val_reg);
@@ -1231,8 +1327,8 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
                     *ptr++ = blr(1);
                 
                     *ptr++ = ror64(1, 1, 32);
-                    *ptr++ = stur64_offset(19, 0, imm_offset);
-                    *ptr++ = stur_offset(19, 1, imm_offset + 8);
+                    EMU68_GUEST_STORE_OFFSET(ptr, 19, imm_offset, 0, 8);
+                    EMU68_GUEST_STORE_OFFSET(ptr, 19, imm_offset + 8, 1, 4);
 
                     ptr = EMIT_RestoreRegFrame(ptr, (RA_GetTempAllocMask() | REG_PROTECT | 3 | (1 << 19)));
                 }
@@ -1269,8 +1365,8 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
                     *ptr++ = blr(1);
 
                     *ptr++ = ror64(1, 1, 32);
-                    *ptr++ = stur64_offset(19, 0, 0);
-                    *ptr++ = stur_offset(19, 1, 8);
+                    EMU68_GUEST_STORE_OFFSET(ptr, 19, 0, 0, 8);
+                    EMU68_GUEST_STORE_OFFSET(ptr, 19, 8, 1, 4);
 
                     ptr = EMIT_RestoreRegFrame(ptr, (RA_GetTempAllocMask() | REG_PROTECT | 3 | (1 << 19)));
                 }
@@ -1309,8 +1405,8 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
                     *ptr++ = blr(1);
 
                     *ptr++ = ror64(1, 1, 32);
-                    *ptr++ = stur64_offset(19, 0, imm_offset);
-                    *ptr++ = stur_offset(19, 1, imm_offset + 8);
+                    EMU68_GUEST_STORE_OFFSET(ptr, 19, imm_offset, 0, 8);
+                    EMU68_GUEST_STORE_OFFSET(ptr, 19, imm_offset + 8, 1, 4);
 
                     ptr = EMIT_RestoreRegFrame(ptr, (RA_GetTempAllocMask() | REG_PROTECT | 3 | (1 << 19)));
                 }
@@ -1347,8 +1443,8 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
                     *ptr++ = blr(1);
 
                     *ptr++ = ror64(1, 1, 32);
-                    *ptr++ = stur64_offset(19, 0, 0);
-                    *ptr++ = stur_offset(19, 1, 8);
+                    EMU68_GUEST_STORE_OFFSET(ptr, 19, 0, 0, 8);
+                    EMU68_GUEST_STORE_OFFSET(ptr, 19, 8, 1, 4);
 
                     ptr = EMIT_RestoreRegFrame(ptr, (RA_GetTempAllocMask() | REG_PROTECT | 3 | (1 << 19)));
                 }
@@ -1367,15 +1463,8 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
                     }
                     if (imm_offset >= -255 && imm_offset <= 251)
                     {
-                        ptr = get_Save96(ptr);
-                        *ptr++ = str64_offset_preindex(31, 30, -16);
-                        if (imm_offset < 0)
-                            *ptr++ = sub_immed(1, int_reg, -imm_offset);
-                        else
-                            *ptr++ = add_immed(1, int_reg, imm_offset);
-                        *ptr++ = mov_simd_to_reg(0, reg, TS_D, 0);
-                        *ptr++ = blr(reg_Save96);
-                        *ptr++ = ldr64_offset_postindex(31, 30, 16);
+                        ptr = emit_guest_store96(ptr, int_reg, imm_offset,
+                                                 reg);
 
                         //ptr = EMIT_Store96bitFP(ptr, reg, int_reg, imm_offset);
                     }
@@ -1399,12 +1488,7 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
                             *ptr++ = add_reg(off, int_reg, off, LSL, 0);
                         }
 
-                        ptr = get_Save96(ptr);
-                        *ptr++ = str64_offset_preindex(31, 30, -16);
-                        *ptr++ = mov_reg(1, off);
-                        *ptr++ = mov_simd_to_reg(0, reg, TS_D, 0);
-                        *ptr++ = blr(reg_Save96);
-                        *ptr++ = ldr64_offset_postindex(31, 30, 16);
+                        ptr = emit_guest_store96(ptr, off, 0, reg);
 
                         //ptr = EMIT_Store96bitFP(ptr, reg, off, 0);
                         RA_FreeARMRegister(&ptr, off);
@@ -1419,19 +1503,23 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
                 {
                     if (pre_sz)
                     {
-                        *ptr++ = fstd_preindex(reg, int_reg, pre_sz);
+                        ptr = emit_guest_fp_store(ptr, int_reg, 0, reg, 8,
+                                                  pre_sz, 0);
                     }
                     else if (post_sz)
                     {
-                        *ptr++ = fstd_postindex(reg, int_reg, post_sz);
+                        ptr = emit_guest_fp_store(ptr, int_reg, 0, reg, 8,
+                                                  0, post_sz);
                     }
                     else if (imm_offset >= -255 && imm_offset <= 255)
                     {
-                        *ptr++ = fstd(reg, int_reg, imm_offset);
+                        ptr = emit_guest_fp_store(ptr, int_reg, imm_offset,
+                                                  reg, 8, 0, 0);
                     }
                     else if (imm_offset >= 0 && imm_offset < 32760 && !(imm_offset & 7))
                     {
-                        *ptr++ = fstd_pimm(reg, int_reg, imm_offset >> 3);
+                        ptr = emit_guest_fp_store(ptr, int_reg, imm_offset,
+                                                  reg, 8, 0, 0);
                     }
                     else
                     {
@@ -1452,7 +1540,7 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
                                 *ptr++ = movt_immed_u16(off, (imm_offset) & 0xffff);
                             *ptr++ = add_reg(off, int_reg, off, LSL, 0);
                         }
-                        *ptr++ = fstd(reg, off, 0);
+                        ptr = emit_guest_fp_store(ptr, off, 0, reg, 8, 0, 0);
                         RA_FreeARMRegister(&ptr, off);
                     }
                 }
@@ -1461,19 +1549,23 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
                 *ptr++ = fcvtsd(vfp_reg, reg);
                 if (pre_sz)
                 {
-                    *ptr++ = fsts_preindex(vfp_reg, int_reg, pre_sz);
+                    ptr = emit_guest_fp_store(ptr, int_reg, 0, vfp_reg, 4,
+                                              pre_sz, 0);
                 }
                 else if (post_sz)
                 {
-                    *ptr++ = fsts_postindex(vfp_reg, int_reg, post_sz);
+                    ptr = emit_guest_fp_store(ptr, int_reg, 0, vfp_reg, 4,
+                                              0, post_sz);
                 }
                 else if (imm_offset >= -255 && imm_offset <= 255)
                 {
-                    *ptr++ = fsts(vfp_reg, int_reg, imm_offset);
+                    ptr = emit_guest_fp_store(ptr, int_reg, imm_offset,
+                                              vfp_reg, 4, 0, 0);
                 }
                 else if (imm_offset >= 0 && imm_offset < 16380 && !(imm_offset & 3))
                 {
-                    *ptr++ = fsts_pimm(vfp_reg, int_reg, imm_offset >> 2);
+                    ptr = emit_guest_fp_store(ptr, int_reg, imm_offset,
+                                              vfp_reg, 4, 0, 0);
                 }
                 else
                 {
@@ -1494,7 +1586,7 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
                             *ptr++ = movt_immed_u16(off, (imm_offset) & 0xffff);
                         *ptr++ = add_reg(off, int_reg, off, LSL, 0);
                     }
-                    *ptr++ = fsts(vfp_reg, off, 0);
+                    ptr = emit_guest_fp_store(ptr, off, 0, vfp_reg, 4, 0, 0);
                     RA_FreeARMRegister(&ptr, off);
                 }
                 break;
@@ -1505,19 +1597,21 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
 
                 if (pre_sz)
                 {
-                    *ptr++ = str_offset_preindex(int_reg, val_reg, pre_sz);
+                    EMU68_GUEST_PRESTORE(ptr, int_reg, val_reg, 4, pre_sz);
                 }
                 else if (post_sz)
                 {
-                    *ptr++ = str_offset_postindex(int_reg, val_reg, post_sz);
+                    EMU68_GUEST_POSTSTORE(ptr, int_reg, val_reg, 4, post_sz);
                 }
                 else if (imm_offset >= -255 && imm_offset <= 255)
                 {
-                    *ptr++ = stur_offset(int_reg, val_reg, imm_offset);
+                    EMU68_GUEST_STORE_OFFSET(ptr, int_reg, imm_offset,
+                                             val_reg, 4);
                 }
                 else if (imm_offset >= 0 && imm_offset < 16380 && !(imm_offset & 3))
                 {
-                    *ptr++ = str_offset(int_reg, val_reg, imm_offset);
+                    EMU68_GUEST_STORE_OFFSET(ptr, int_reg, imm_offset,
+                                             val_reg, 4);
                 }
                 else
                 {
@@ -1538,7 +1632,7 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
                             *ptr++ = movt_immed_u16(off, (imm_offset) & 0xffff);
                         *ptr++ = add_reg(off, int_reg, off, LSL, 0);
                     }
-                    *ptr++ = str_offset(off, val_reg, 0);
+                    EMU68_GUEST_STORE(ptr, off, val_reg, 4);
                     RA_FreeARMRegister(&ptr, off);
                 }
                 break;
@@ -1557,19 +1651,21 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
 
                 if (pre_sz)
                 {
-                    *ptr++ = strh_offset_preindex(int_reg, val_reg, pre_sz);
+                    EMU68_GUEST_PRESTORE(ptr, int_reg, val_reg, 2, pre_sz);
                 }
                 else if (post_sz)
                 {
-                    *ptr++ = strh_offset_postindex(int_reg, val_reg, post_sz);
+                    EMU68_GUEST_POSTSTORE(ptr, int_reg, val_reg, 2, post_sz);
                 }
                 else if (imm_offset >= -255 && imm_offset <= 255)
                 {
-                    *ptr++ = sturh_offset(int_reg, val_reg, imm_offset);
+                    EMU68_GUEST_STORE_OFFSET(ptr, int_reg, imm_offset,
+                                             val_reg, 2);
                 }
                 else if (imm_offset >= 0 && imm_offset < 8190 && !(imm_offset & 1))
                 {
-                    *ptr++ = strh_offset(int_reg, val_reg, imm_offset);
+                    EMU68_GUEST_STORE_OFFSET(ptr, int_reg, imm_offset,
+                                             val_reg, 2);
                 }
                 else
                 {
@@ -1590,7 +1686,7 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
                             *ptr++ = movt_immed_u16(off, (imm_offset) & 0xffff);
                         *ptr++ = add_reg(off, int_reg, off, LSL, 0);
                     }
-                    *ptr++ = strh_offset(off, val_reg, 0);
+                    EMU68_GUEST_STORE(ptr, off, val_reg, 2);
                     RA_FreeARMRegister(&ptr, off);
                 }
                 break;
@@ -1609,19 +1705,21 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
 
                 if (pre_sz)
                 {
-                    *ptr++ = strb_offset_preindex(int_reg, val_reg, pre_sz);
+                    EMU68_GUEST_PRESTORE(ptr, int_reg, val_reg, 1, pre_sz);
                 }
                 else if (post_sz)
                 {
-                    *ptr++ = strb_offset_postindex(int_reg, val_reg, post_sz);
+                    EMU68_GUEST_POSTSTORE(ptr, int_reg, val_reg, 1, post_sz);
                 }
                 else if (imm_offset >= -255 && imm_offset <= 255)
                 {
-                    *ptr++ = sturb_offset(int_reg, val_reg, imm_offset);
+                    EMU68_GUEST_STORE_OFFSET(ptr, int_reg, imm_offset,
+                                             val_reg, 1);
                 }
                 else if (imm_offset >= 0 && imm_offset < 4096)
                 {
-                    *ptr++ = strb_offset(int_reg, val_reg, imm_offset);
+                    EMU68_GUEST_STORE_OFFSET(ptr, int_reg, imm_offset,
+                                             val_reg, 1);
                 }
                 else
                 {
@@ -1642,7 +1740,7 @@ uint32_t *FPU_StoreData(uint32_t *ptr, uint16_t **m68k_ptr, uint8_t reg, uint16_
                             *ptr++ = movt_immed_u16(off, (imm_offset) & 0xffff);
                         *ptr++ = add_reg(off, int_reg, off, LSL, 0);
                     }
-                    *ptr++ = strb_offset(off, val_reg, 0);
+                    EMU68_GUEST_STORE(ptr, off, val_reg, 1);
                     RA_FreeARMRegister(&ptr, off);
                 }
                 break;
@@ -3783,14 +3881,14 @@ uint32_t *EMIT_FPU(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
             {
                 reg = RA_GetFPCR(&ptr);
                 
-                *ptr++ = str_offset(dst, reg, offset);
+                EMU68_GUEST_STORE_OFFSET(ptr, dst, offset, reg, 4);
                 offset += 4;
             }
 
             if (opcode2 & 0x0800)
             {
                 reg = RA_GetFPSR(&ptr);
-                *ptr++ = str_offset(dst, reg, offset);
+                EMU68_GUEST_STORE_OFFSET(ptr, dst, offset, reg, 4);
                 offset += 4;
             }
 
@@ -3804,7 +3902,7 @@ uint32_t *EMIT_FPU(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
                 else {
                     *ptr++ = mov_simd_to_reg(reg, 29, TS_S, 1);
                 }
-                *ptr++ = str_offset(dst, reg, offset);
+                EMU68_GUEST_STORE_OFFSET(ptr, dst, offset, reg, 4);
                 RA_FreeARMRegister(&ptr, reg);
                 reg = 0xff;
             }
@@ -3909,7 +4007,7 @@ uint32_t *EMIT_FPU(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
                 uint8_t round = RA_AllocARMRegister(&ptr);
                 reg = RA_ModifyFPCR(&ptr);
                 
-                *ptr++ = ldr_offset(src, tmp, offset);
+                EMU68_GUEST_LOAD_OFFSET(ptr, src, offset, tmp, 4, 0);
                 *ptr++ = mov_reg(reg, tmp);
 
                 *ptr++ = get_fpcr(tmp);
@@ -3927,7 +4025,7 @@ uint32_t *EMIT_FPU(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
             if (opcode2 & 0x0800)
             {
                 reg = RA_ModifyFPSR(&ptr);
-                *ptr++ = ldr_offset(src, tmp, offset);
+                EMU68_GUEST_LOAD_OFFSET(ptr, src, offset, tmp, 4, 0);
                 *ptr++ = mov_reg(reg, tmp);
                 offset += 4;
             }
@@ -3935,7 +4033,7 @@ uint32_t *EMIT_FPU(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
             if (opcode2 & 0x0400)
             {
                 val_FPIAR = 0xffffffff;
-                *ptr++ = ldr_offset(src, tmp, offset);
+                EMU68_GUEST_LOAD_OFFSET(ptr, src, offset, tmp, 4, 0);
                 *ptr++ = mov_reg_to_simd(29, TS_S, 1, tmp);
             }
 
@@ -3981,17 +4079,13 @@ uint32_t *EMIT_FPU(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
                         size++;
                 *ptr++ = sub_immed(base_reg, base_reg, 12*size);
 
-                ptr = get_Save96(ptr);
-                *ptr++ = str64_offset_preindex(31, 30, -16);
-
                 for (int i=0; i < 8; i++) {
                     if ((opcode2 & (1 << i)) != 0) {
                         uint8_t fp_reg = RA_MapFPURegister(&ptr, i);
                         //*ptr++ = sub_immed(base_reg, base_reg, 12);
 
-                        *ptr++ = add_immed(1, base_reg, 12*cnt);
-                        *ptr++ = mov_simd_to_reg(0, fp_reg, TS_D, 0);
-                        *ptr++ = blr(reg_Save96);
+                        ptr = emit_guest_store96(ptr, base_reg, 12 * cnt,
+                                                 fp_reg);
                         //ptr = EMIT_Store96bitFP(ptr, fp_reg, base_reg, 12*cnt++);
                         //*ptr++ = fstd(fp_reg, base_reg, 12*cnt++);
 
@@ -3999,22 +4093,17 @@ uint32_t *EMIT_FPU(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
                         RA_FreeFPURegister(&ptr, fp_reg);
                     }
                 }
-                *ptr++ = ldr64_offset_postindex(31, 30, 16);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             } else if (mode == 3) {
                 kprintf("[JIT] Unsupported FMOVEM operation (REG to MEM postindex)\n");
             } else {
-                ptr = get_Save96(ptr);
-                *ptr++ = str64_offset_preindex(31, 30, -16);
-
                 int cnt = 0;
                 for (int i=0; i < 8; i++) {
                     if ((opcode2 & (0x80 >> i)) != 0) {
                         uint8_t fp_reg = RA_MapFPURegister(&ptr, i);
 
-                        *ptr++ = add_immed(1, base_reg, 12*cnt);
-                        *ptr++ = mov_simd_to_reg(0, fp_reg, TS_D, 0);
-                        *ptr++ = blr(reg_Save96);
+                        ptr = emit_guest_store96(ptr, base_reg, 12 * cnt,
+                                                 fp_reg);
 
                         //ptr = EMIT_Store96bitFP(ptr, fp_reg, base_reg, 12*cnt);
                         //*ptr++ = fstd(fp_reg, base_reg, cnt*12);
@@ -4023,7 +4112,6 @@ uint32_t *EMIT_FPU(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
                     }
                 }
 
-                *ptr++ = ldr64_offset_postindex(31, 30, 16);
             }
         } else { /* memory to FPn */
             uint8_t mode = (opcode & 0x0038) >> 3;
@@ -4035,17 +4123,13 @@ uint32_t *EMIT_FPU(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
 
             /* Post index? Note - dynamic mode not supported yet! using double mode instead of extended! */
             if (mode == 3) {
-                ptr = get_Load96(ptr);
-                *ptr++ = str64_offset_preindex(31, 30, -16);
-
                 int cnt = 0;
                 for (int i=0; i < 8; i++) {
                     if ((opcode2 & (0x80 >> i)) != 0) {
                         uint8_t fp_reg = RA_MapFPURegisterForWrite(&ptr, i);
 
-                        *ptr++ = add_immed(1, base_reg, 12*cnt);
-                        *ptr++ = blr(reg_Load96);
-                        *ptr++ = mov_reg_to_simd(fp_reg, TS_D, 0, 0);
+                        ptr = emit_guest_load96(ptr, base_reg, 12 * cnt,
+                                                fp_reg);
 
                         //ptr = EMIT_Load96bitFP(ptr, fp_reg, base_reg, 12*cnt++);
                         //*ptr++ = fldd(fp_reg, base_reg, 12*cnt++);
@@ -4055,24 +4139,18 @@ uint32_t *EMIT_FPU(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
                     }
                 }
 
-                *ptr++ = ldr64_offset_postindex(31, 30, 16);
-
                 *ptr++ = add_immed(base_reg, base_reg, 12*cnt);
                 RA_SetDirtyM68kRegister(&ptr, 8 + (opcode & 7));
             } else if (mode == 4) {
                 kprintf("[JIT] Unsupported FMOVEM operation (REG to MEM preindex)\n");
             } else {
-                ptr = get_Load96(ptr);
-                *ptr++ = str64_offset_preindex(31, 30, -16);
-
                 int cnt = 0;
                 for (int i=0; i < 8; i++) {
                     if ((opcode2 & (0x80 >> i)) != 0) {
                         uint8_t fp_reg = RA_MapFPURegisterForWrite(&ptr, i);
 
-                        *ptr++ = add_immed(1, base_reg, 12*cnt);
-                        *ptr++ = blr(reg_Load96);
-                        *ptr++ = mov_reg_to_simd(fp_reg, TS_D, 0, 0);
+                        ptr = emit_guest_load96(ptr, base_reg, 12 * cnt,
+                                                fp_reg);
                         //ptr = EMIT_Load96bitFP(ptr, fp_reg, base_reg, 12*cnt);
                         //*ptr++ = fldd(fp_reg, base_reg, cnt*12);
                         cnt++;
@@ -4080,7 +4158,6 @@ uint32_t *EMIT_FPU(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
                     }
                 }
 
-                *ptr++ = ldr64_offset_postindex(31, 30, 16);
             }
         }
 
@@ -4699,9 +4776,11 @@ uint32_t *EMIT_lineF(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed
 
         *ptr++ = bic_immed(aligned_src, src, 4, 0);
         *ptr++ = bic_immed(aligned_dst, dst, 4, 0);
-        *ptr++ = ldp64(aligned_src, buf1, buf2, 0);
+        EMU68_GUEST_LOAD_OFFSET(ptr, aligned_src, 0, buf1, 8, 0);
+        EMU68_GUEST_LOAD_OFFSET(ptr, aligned_src, 8, buf2, 8, 0);
         *ptr++ = add_immed(src, src, 16);
-        *ptr++ = stp64(aligned_dst, buf1, buf2, 0);
+        EMU68_GUEST_STORE_OFFSET(ptr, aligned_dst, 0, buf1, 8);
+        EMU68_GUEST_STORE_OFFSET(ptr, aligned_dst, 8, buf2, 8);
 
         // Update dst only if it is not the same as src!
         if (dst != src) {
@@ -4740,12 +4819,16 @@ uint32_t *EMIT_lineF(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed
         *ptr++ = bic_immed(aligned_reg, reg, 4, 0);
 
         if (opcode & 8) {
-            *ptr++ = ldp64(aligned_mem, buf1, buf2, 0);
-            *ptr++ = stp64(aligned_reg, buf1, buf2, 0);
+            EMU68_GUEST_LOAD_OFFSET(ptr, aligned_mem, 0, buf1, 8, 0);
+            EMU68_GUEST_LOAD_OFFSET(ptr, aligned_mem, 8, buf2, 8, 0);
+            EMU68_GUEST_STORE_OFFSET(ptr, aligned_reg, 0, buf1, 8);
+            EMU68_GUEST_STORE_OFFSET(ptr, aligned_reg, 8, buf2, 8);
         }
         else {
-            *ptr++ = ldp64(aligned_reg, buf1, buf2, 0);
-            *ptr++ = stp64(aligned_mem, buf1, buf2, 0);
+            EMU68_GUEST_LOAD_OFFSET(ptr, aligned_reg, 0, buf1, 8, 0);
+            EMU68_GUEST_LOAD_OFFSET(ptr, aligned_reg, 8, buf2, 8, 0);
+            EMU68_GUEST_STORE_OFFSET(ptr, aligned_mem, 0, buf1, 8);
+            EMU68_GUEST_STORE_OFFSET(ptr, aligned_mem, 8, buf2, 8);
         }
 
         if (!(opcode & 0x10))
diff --git a/src/math/96bit.c b/src/math/96bit.c
index 843ec11..963fca4 100644
--- a/src/math/96bit.c
+++ b/src/math/96bit.c
@@ -2,12 +2,8 @@
 
 #define unlikely(x)    __builtin_expect(!!(x), 0)
 
-uint64_t Load96bit(uintptr_t __ignore, uintptr_t base)
+uint64_t Load96Values(uint16_t exp, uint64_t mant)
 {
-    (void)__ignore;
-    
-    uint16_t exp = *(uint16_t *)base;
-    uint64_t mant = *(uint64_t *)(base + 4);
     uint64_t ret;
 
     /* Load zero, positive or negative */
@@ -41,38 +37,55 @@ uint64_t Load96bit(uintptr_t __ignore, uintptr_t base)
     return ret;
 }
 
-void Store96bit(uint64_t value, uintptr_t base)
+uint64_t Load96bit(uintptr_t __ignore, uintptr_t base)
+{
+    (void)__ignore;
+    return Load96Values(*(uint16_t *)base, *(uint64_t *)(base + 4));
+}
+
+struct FP96Values {
+    uint64_t first;
+    uint32_t last;
+};
+
+struct FP96Values Store96Values(uint64_t value)
 {
-    uint32_t exp = (value & 0x8000000000000000ULL) >> 32;
+    struct FP96Values result;
+    uint32_t exp = (uint32_t)((value & 0x8000000000000000ULL) >> 32);
     uint64_t mant = 0x8000000000000000ULL;
 
-    /* Store zero, positive or negative */
-    if (unlikely((value & 0x7fffffffffffffffULL) == 0))
-    {
-        *(uint64_t *)base = value;
-        *(uint32_t *)(base + 8) = 0;
-        return;
+    if (unlikely((value & 0x7fffffffffffffffULL) == 0)) {
+        result.first = value;
+        result.last = 0;
+        return result;
     }
-
-    /* Store plus/minus infinity */
-    if (unlikely((value & 0x7fffffffffffffffULL) == 0x7ff0000000000000ULL))
-    {
-        *(uint64_t *)base = value | 0x000f000000000000ULL;
-        *(uint32_t *)(base + 8) = 0;
-        return;
+    if (unlikely((value & 0x7fffffffffffffffULL) ==
+                 0x7ff0000000000000ULL)) {
+        result.first = value | 0x000f000000000000ULL;
+        result.last = 0;
+        return result;
     }
-
-    /* Store NaN */
-    if (unlikely((value & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL && (value & 0x000fffffffffffff) != 0))
-    {
-        *(uint32_t *)base = ((value >> 32) | 0x000f0000ULL) & 0xffff0000;
-        *(uint64_t *)(base + 4) = 0xffffffffffffffffULL;
-        return;
+    if (unlikely((value & 0x7ff0000000000000ULL) ==
+                     0x7ff0000000000000ULL &&
+                 (value & 0x000fffffffffffffULL) != 0)) {
+        uint32_t header =
+            (uint32_t)(((value >> 32) | 0x000f0000ULL) & 0xffff0000ULL);
+        result.first = ((uint64_t)header << 32) | 0xffffffffULL;
+        result.last = 0xffffffffU;
+        return result;
     }
 
-    exp |= (((value >> 52) & 0x7ff) - 0x3ff + 0x3fff) << 16;
+    exp |= (uint32_t)((((value >> 52) & 0x7ff) - 0x3ff + 0x3fff)
+                      << 16);
     mant |= (value & 0x000fffffffffffffULL) << 11;
+    result.first = ((uint64_t)exp << 32) | (mant >> 32);
+    result.last = (uint32_t)mant;
+    return result;
+}
 
-    *(uint32_t *)base = exp;
-    *(uint64_t *)(base + 4) = mant;
+void Store96bit(uint64_t value, uintptr_t base)
+{
+    struct FP96Values result = Store96Values(value);
+    *(uint64_t *)base = result.first;
+    *(uint32_t *)(base + 8) = result.last;
 }
```

### `0034-emu68-explicit-paired-move.patch`

```diff
diff --git a/src/M68k_MOVE.c b/src/M68k_MOVE.c
index f0ad21b..60a5349 100644
--- a/src/M68k_MOVE.c
+++ b/src/M68k_MOVE.c
@@ -170,7 +170,9 @@ uint32_t *EMIT_move(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
                         *ptr++ = cmn_reg(31, src_reg_2, LSL, 0);
                     }
 
-                    *ptr++ = stp_preindex(addr_reg, src_reg_2, src_reg_1, -8);
+                    *ptr++ = sub_immed(addr_reg, addr_reg, 8);
+                    EMU68_GUEST_STORE_OFFSET(ptr, addr_reg, 0, src_reg_2, 4);
+                    EMU68_GUEST_STORE_OFFSET(ptr, addr_reg, 4, src_reg_1, 4);
 
                     tmp_reg = src_reg_2;
                 
@@ -208,7 +210,9 @@ uint32_t *EMIT_move(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
                         *ptr++ = cmn_reg(31, src_reg_2, LSL, 0);
                     }
 
-                    *ptr++ = stp_postindex(addr_reg, src_reg_1, src_reg_2, 8);
+                    EMU68_GUEST_STORE_OFFSET(ptr, addr_reg, 0, src_reg_1, 4);
+                    EMU68_GUEST_STORE_OFFSET(ptr, addr_reg, 4, src_reg_2, 4);
+                    *ptr++ = add_immed(addr_reg, addr_reg, 8);
 
                     tmp_reg = src_reg_2;
                 
@@ -244,7 +248,11 @@ uint32_t *EMIT_move(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
                         /* Two subsequent register moves from (An)+ */
                         (*m68k_ptr)+=2;
                         
-                        *ptr++ = ldp_postindex(addr_reg, dst_reg_1, dst_reg_2, 8);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, addr_reg, 0, dst_reg_1,
+                                                4, 0);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, addr_reg, 4, dst_reg_2,
+                                                4, 0);
+                        *ptr++ = add_immed(addr_reg, addr_reg, 8);
 
                         if (!is_movea2) {
                             update_mask = M68K_GetSRMask(*m68k_ptr - 1);
@@ -295,7 +303,11 @@ uint32_t *EMIT_move(uint32_t *ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
                         /* Two subsequent register moves to (An)+ */
                         (*m68k_ptr)+=2;
 
-                        *ptr++ = ldp_preindex(addr_reg, dst_reg_2, dst_reg_1, -8);
+                        *ptr++ = sub_immed(addr_reg, addr_reg, 8);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, addr_reg, 0, dst_reg_2,
+                                                4, 0);
+                        EMU68_GUEST_LOAD_OFFSET(ptr, addr_reg, 4, dst_reg_1,
+                                                4, 0);
 
                         if (!is_movea2) {
                             update_mask = M68K_GetSRMask(*m68k_ptr - 1);
```

### `0035-emu68-modeled-cycles.patch`

```diff
diff --git a/src/M68k_Translator.c b/src/M68k_Translator.c
--- a/src/M68k_Translator.c
+++ b/src/M68k_Translator.c
@@ -148,6 +148,61 @@ static uint32_t * (*line_array[16])(uint32_t *arm_ptr, uint16_t **m68k_ptr, uint
     EMIT_lineF
 };
 
+#ifdef BELLATRIX
+static uint16_t M68K_OpcodeCycles(uint16_t opcode)
+{
+    static const uint8_t ea_cycles[8] = { 0u, 0u, 4u, 4u, 6u, 8u, 10u, 8u };
+    uint8_t group = (uint8_t)(opcode >> 12);
+    uint8_t mode = (uint8_t)((opcode >> 3) & 7u);
+    uint16_t ea = ea_cycles[mode];
+
+    if (mode == 7u) {
+        switch (opcode & 7u) {
+        case 0u: ea = 8u; break;
+        case 1u: ea = 12u; break;
+        case 2u: ea = 8u; break;
+        case 3u: ea = 10u; break;
+        case 4u: ea = 4u; break;
+        default: ea = 0u; break;
+        }
+    }
+
+    switch (group) {
+    case 0x0u: return (uint16_t)(8u + ea);
+    case 0x1u:
+    case 0x2u:
+    case 0x3u: return (uint16_t)(4u + ea);
+    case 0x4u:
+        if (opcode == 0x4e70u) return 132u;
+        if (opcode == 0x4e71u || opcode == 0x4e72u) return 4u;
+        if (opcode == 0x4e73u) return 20u;
+        if (opcode == 0x4e75u) return 16u;
+        if ((opcode & 0xffc0u) == 0x4e80u) return (uint16_t)(16u + ea);
+        if ((opcode & 0xffc0u) == 0x4ec0u) return (uint16_t)(8u + ea);
+        if ((opcode & 0xfff0u) == 0x4e40u) return 34u;
+        return (uint16_t)(4u + ea);
+    case 0x5u:
+        if ((opcode & 0x00f8u) == 0x00c8u) return 10u;
+        return (uint16_t)(4u + ea);
+    case 0x6u: return (opcode & 0x0f00u) == 0x0100u ? 18u : 10u;
+    case 0x7u: return 4u;
+    case 0x8u:
+        if ((opcode & 0x01c0u) == 0x00c0u) return (uint16_t)(140u + ea);
+        return (uint16_t)(4u + ea);
+    case 0x9u:
+    case 0xbu: return (uint16_t)(4u + ea);
+    case 0xcu:
+        if ((opcode & 0x01c0u) == 0x00c0u) return (uint16_t)(70u + ea);
+        return (uint16_t)(4u + ea);
+    case 0xdu: return (uint16_t)(4u + ea);
+    case 0xeu:
+        return (uint16_t)(((opcode & 0x00c0u) == 0x00c0u ? 8u : 6u) + ea);
+    case 0xfu: return 20u;
+    default: return 4u;
+    }
+}
+#endif
+
 extern struct M68KState *__m68k_state;
 
 static inline uint32_t *EmitINSN(uint32_t *arm_ptr, uint16_t **m68k_ptr, uint16_t *insn_consumed)
@@ -156,6 +211,19 @@ static inline uint32_t *EmitINSN(uint32_t *arm_ptr, uint16_t **m68k_ptr, uint16_
     uint16_t opcode = cache_read_16(ICACHE, (uint32_t)(uintptr_t)*m68k_ptr);
     uint8_t group = opcode >> 12;
 
+#ifdef BELLATRIX
+    {
+        uint8_t ctx = RA_GetCTX(&ptr);
+        uint8_t cycles = RA_AllocARMRegister(&ptr);
+        *ptr++ = ldr64_offset(ctx, cycles,
+            __builtin_offsetof(struct M68KState, CYCLE_COUNT));
+        *ptr++ = add64_immed(cycles, cycles, M68K_OpcodeCycles(opcode));
+        *ptr++ = str64_offset(ctx, cycles,
+            __builtin_offsetof(struct M68KState, CYCLE_COUNT));
+        RA_FreeARMRegister(&ptr, cycles);
+    }
+#endif
+
     if (debug > 2)
     {
         *ptr++ = hint(0);
```

## Archived src/cpu/emu68/emu68_machine_* files (Bellatrix-side API implementation)

### `src/cpu/emu68/emu68_machine.h`

```c
/*
 * Public machine-host API for Emu68.
 *
 * This interface deliberately contains no platform, MMU, exception-vector or
 * JIT implementation details.  See docs/emu68_public_api.md in Bellatrix for
 * the normative contract.
 */
#ifndef EMU68_MACHINE_H
#define EMU68_MACHINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMU68_MACHINE_ABI_VERSION 1u

typedef struct emu68_cpu emu68_cpu_t;

typedef enum emu68_execution_mode {
    EMU68_EXEC_SYNCHRONOUS = 0,
    EMU68_EXEC_COOPERATIVE = 1
} emu68_execution_mode_t;

typedef enum emu68_status {
    EMU68_OK = 0,
    EMU68_ERR_INVALID_ARGUMENT,
    EMU68_ERR_ABI_MISMATCH,
    EMU68_ERR_BUSY,
    EMU68_ERR_OVERLAP,
    EMU68_ERR_NOT_FOUND,
    EMU68_ERR_ACCESS,
    EMU68_ERR_INTERNAL
} emu68_status_t;

typedef enum emu68_region_kind {
    EMU68_REGION_DIRECT = 0,
    EMU68_REGION_EXTERNAL = 1,
    EMU68_REGION_UNMAPPED = 2
} emu68_region_kind_t;

enum {
    EMU68_REGION_READ = 1u << 0,
    EMU68_REGION_WRITE = 1u << 1,
    EMU68_REGION_EXECUTE = 1u << 2,
    EMU68_REGION_CACHEABLE = 1u << 3
};

typedef enum emu68_access_kind {
    EMU68_ACCESS_READ = 0,
    EMU68_ACCESS_WRITE = 1
} emu68_access_kind_t;

typedef enum emu68_address_space {
    EMU68_SPACE_DATA = 0,
    EMU68_SPACE_PROGRAM = 1,
    EMU68_SPACE_CPU = 2
} emu68_address_space_t;

typedef enum emu68_bus_result {
    EMU68_BUS_COMPLETE = 0,
    EMU68_BUS_ERROR = 1
} emu68_bus_result_t;

typedef enum emu68_stop_reason {
    EMU68_STOP_BUDGET = 0,
    EMU68_STOP_EXTERNAL_ACCESS,
    EMU68_STOP_STOPPED,
    EMU68_STOP_REQUESTED,
    EMU68_STOP_BUS_ERROR,
    EMU68_STOP_FATAL
} emu68_stop_reason_t;

typedef struct emu68_bus_access {
    uint32_t abi_version;
    size_t struct_size;
    uint64_t sequence;
    uint32_t address;
    uint32_t region_id;
    emu68_access_kind_t kind;
    emu68_address_space_t space;
    uint8_t function_code;
    uint8_t width;
    emu68_bus_result_t result;
    uint8_t reserved[2];
    uint64_t value_lo;
    uint64_t value_hi;
} emu68_bus_access_t;

typedef emu68_bus_result_t (*emu68_bus_access_fn)(
    void *opaque, emu68_bus_access_t *access);
typedef void (*emu68_progress_fn)(
    void *opaque, uint64_t cycle_delta, uint64_t instruction_delta,
    uint32_t pc);

typedef struct emu68_machine_ops {
    uint32_t abi_version;
    size_t struct_size;
    emu68_bus_access_fn bus_access;
    emu68_progress_fn progress;
} emu68_machine_ops_t;

typedef struct emu68_machine_config {
    uint32_t abi_version;
    size_t struct_size;
    emu68_execution_mode_t execution_mode;
    const emu68_machine_ops_t *ops;
    void *opaque;
} emu68_machine_config_t;

typedef struct emu68_direct_region {
    uint32_t abi_version;
    size_t struct_size;
    uint32_t guest_base;
    uint64_t size;
    void *host_base;
    uint32_t flags;
} emu68_direct_region_t;

typedef struct emu68_external_region {
    uint32_t abi_version;
    size_t struct_size;
    uint32_t guest_base;
    uint64_t size;
    uint32_t region_id;
    uint32_t flags;
} emu68_external_region_t;

typedef struct emu68_run_result {
    uint32_t abi_version;
    size_t struct_size;
    emu68_stop_reason_t reason;
    uint64_t cycles_executed;
    uint64_t instructions_executed;
    uint32_t pc;
    uint32_t detail;
} emu68_run_result_t;

typedef struct emu68_reset_state {
    uint32_t abi_version;
    size_t struct_size;
    uint32_t initial_ssp;
    uint32_t initial_pc;
} emu68_reset_state_t;

emu68_status_t emu68_machine_create(
    const emu68_machine_config_t *config, emu68_cpu_t **out_cpu);
void emu68_machine_destroy(emu68_cpu_t *cpu);

emu68_status_t emu68_machine_map_direct(
    emu68_cpu_t *cpu, const emu68_direct_region_t *region);
emu68_status_t emu68_machine_map_external(
    emu68_cpu_t *cpu, const emu68_external_region_t *region);
emu68_status_t emu68_machine_map_unmapped(
    emu68_cpu_t *cpu, uint32_t guest_base, uint64_t size);
emu68_status_t emu68_machine_unmap(
    emu68_cpu_t *cpu, uint32_t guest_base, uint64_t size);

emu68_status_t emu68_machine_run(
    emu68_cpu_t *cpu, uint64_t cycle_budget, emu68_run_result_t *result);
emu68_status_t emu68_machine_get_pending_access(
    emu68_cpu_t *cpu, emu68_bus_access_t *out_access);
emu68_status_t emu68_machine_complete_access(
    emu68_cpu_t *cpu, const emu68_bus_access_t *completion);

emu68_status_t emu68_machine_reset(
    emu68_cpu_t *cpu, const emu68_reset_state_t *state);
void emu68_machine_request_stop(emu68_cpu_t *cpu);
emu68_status_t emu68_machine_set_ipl(emu68_cpu_t *cpu, unsigned level);

emu68_status_t emu68_machine_invalidate_code(
    emu68_cpu_t *cpu, uint32_t guest_base, uint64_t size);
emu68_status_t emu68_machine_invalidate_all_code(emu68_cpu_t *cpu);

#ifdef __cplusplus
}
#endif

#endif
```

### `src/cpu/emu68/emu68_machine_internal.h`

```c
#ifndef EMU68_MACHINE_INTERNAL_H
#define EMU68_MACHINE_INTERNAL_H

#include "cpu/emu68/emu68_machine.h"

enum emu68_machine_page_class {
    EMU68_PAGE_DIRECT = 1,
    EMU68_PAGE_EXTERNAL = 2,
    EMU68_PAGE_FAULT = 3,
    EMU68_PAGE_MIXED = 4
};

#define EMU68_MACHINE_PAGE_SHIFT 16u
#define EMU68_MACHINE_PAGE_SIZE (1u << EMU68_MACHINE_PAGE_SHIFT)
#define EMU68_MACHINE_PAGE_COUNT (1u << (32u - EMU68_MACHINE_PAGE_SHIFT))

extern uint8_t emu68_machine_read_pages[EMU68_MACHINE_PAGE_COUNT];
extern uint8_t emu68_machine_write_pages[EMU68_MACHINE_PAGE_COUNT];

typedef struct emu68_machine_access_result {
    emu68_region_kind_t region_kind;
    uint32_t region_id;
    uint32_t flags;
} emu68_machine_access_result_t;

typedef enum emu68_machine_bridge_outcome {
    EMU68_BRIDGE_COMPLETE = 0,
    EMU68_BRIDGE_PENDING = 1,
    EMU68_BRIDGE_BUS_ERROR = 2
} emu68_machine_bridge_outcome_t;

typedef struct emu68_machine_bridge_result {
    uint64_t value;
    uint64_t outcome;
} emu68_machine_bridge_result_t;

enum {
    EMU68_BRIDGE_META_WIDTH_MASK = 0xffu,
    EMU68_BRIDGE_META_WRITE = 1u << 8,
    EMU68_BRIDGE_META_SPACE_SHIFT = 9,
    EMU68_BRIDGE_META_SPACE_MASK = 3u << EMU68_BRIDGE_META_SPACE_SHIFT,
    EMU68_BRIDGE_META_FC_SHIFT = 11,
    EMU68_BRIDGE_META_FC_MASK = 7u << EMU68_BRIDGE_META_FC_SHIFT,
    EMU68_BRIDGE_META_USE_SFC = 1u << 14,
    EMU68_BRIDGE_META_USE_DFC = 1u << 15
};

emu68_status_t emu68_machine_classify_access(
    uint32_t address, uint8_t width, emu68_access_kind_t kind,
    emu68_machine_access_result_t *result);

emu68_bus_result_t emu68_machine_dispatch_access(
    emu68_bus_access_t *access);

emu68_machine_bridge_result_t emu68_machine_bridge_dispatch(
    uint32_t address, uint64_t value, uint32_t metadata,
    uintptr_t native_resume, const void *native_frame);

emu68_machine_bridge_result_t emu68_machine_native_bridge(
    uint32_t address, uint64_t value, uint32_t metadata);

extern uint8_t *emu68_machine_resume_frame;
extern uintptr_t emu68_machine_resume_address;
extern uint64_t emu68_machine_resume_value;
extern uint64_t emu68_machine_resume_outcome;
extern uint32_t emu68_machine_bridge_address;
extern uint64_t emu68_machine_bridge_write_value;
extern uint32_t emu68_machine_bridge_metadata;
extern uintptr_t emu68_machine_bridge_tu_return;
extern uint64_t emu68_machine_bridge_read_value;
extern uint64_t emu68_machine_bridge_outcome;

int emu68_machine_prepare_native_resume(void);
int emu68_machine_native_access_pending(void);
int emu68_machine_native_bus_error_pending(void);
int emu68_machine_enter_bus_error(void);
int emu68_machine_instruction_fetch_allowed(uint32_t address, uint8_t width);
int emu68_machine_dispatch_quantum_progress(uint64_t retired_instructions,
                                            uint32_t pc);
void emu68_machine_resume_native(void);

int emu68_machine_runtime_active(void);

#endif
```

### `src/cpu/emu68/emu68_machine_platform.h`

```c
#ifndef EMU68_MACHINE_PLATFORM_H
#define EMU68_MACHINE_PLATFORM_H

#include "cpu/emu68/emu68_machine.h"

typedef struct emu68_machine_arch_state {
    uint32_t pc;
    uint32_t a7;
    uint32_t usp;
    uint32_t isp;
    uint32_t msp;
    uint32_t vbr;
    uint16_t sr;
} emu68_machine_arch_state_t;

emu68_status_t emu68_machine_platform_map_direct(
    uint32_t guest_base, uint64_t size, void *host_base, uint32_t flags);
void emu68_machine_platform_unmap_direct(uint32_t guest_base, uint64_t size);
void emu68_machine_platform_invalidate(uint32_t guest_base, uint64_t size);
void emu68_machine_platform_invalidate_all(void);
emu68_status_t emu68_machine_platform_reset(uint32_t initial_ssp,
                                            uint32_t initial_pc);
emu68_status_t emu68_machine_platform_set_ipl(unsigned level);
void emu68_machine_platform_wake(void);
void emu68_machine_platform_run(void);
void emu68_machine_platform_snapshot(uint64_t *instructions, uint64_t *cycles,
                                     uint32_t *pc, int *stopped);
void emu68_machine_platform_add_cycles(uint32_t cycles);
uint8_t emu68_machine_platform_source_function_code(int destination);
emu68_status_t emu68_machine_platform_get_arch_state(
    emu68_machine_arch_state_t *state);
emu68_status_t emu68_machine_platform_set_arch_state(
    const emu68_machine_arch_state_t *state);

#endif
```

### `src/cpu/emu68/emu68_machine_emit.h`

```c
#ifndef EMU68_MACHINE_EMIT_H
#define EMU68_MACHINE_EMIT_H

#include <stdint.h>

enum {
    EMU68_MACHINE_META_PROGRAM = 1u << 9,
    EMU68_MACHINE_META_CPU = 2u << 9,
    EMU68_MACHINE_META_USE_SFC = 1u << 14,
    EMU68_MACHINE_META_USE_DFC = 1u << 15
};

extern uint32_t emu68_machine_translation_metadata;

uint32_t *emu68_machine_emit_load(uint32_t *ptr, uint8_t address_reg,
                                  uint8_t value_reg, uint8_t width,
                                  int sign_extend, uint32_t metadata);
uint32_t *emu68_machine_emit_store(uint32_t *ptr, uint8_t address_reg,
                                   uint8_t value_reg, uint8_t width,
                                   uint32_t metadata);
uint32_t *emu68_machine_emit_load_offset(
    uint32_t *ptr, uint8_t base_reg, int32_t offset, uint8_t value_reg,
    uint8_t width, int sign_extend, uint32_t metadata);
uint32_t *emu68_machine_emit_store_offset(
    uint32_t *ptr, uint8_t base_reg, int32_t offset, uint8_t value_reg,
    uint8_t width, uint32_t metadata);

#endif
```

### `src/cpu/emu68/emu68_machine.c`

```c
#include "cpu/emu68/emu68_machine_internal.h"
#include "cpu/emu68/emu68_machine_platform.h"

#include <string.h>

#define EMU68_MACHINE_MAX_REGIONS 256u
#define EMU68_MACHINE_NATIVE_FRAME_SIZE 800u
#define EMU68_MACHINE_VALID_FLAGS                                             \
    (EMU68_REGION_READ | EMU68_REGION_WRITE | EMU68_REGION_EXECUTE |          \
     EMU68_REGION_CACHEABLE)

struct emu68_machine_region {
    uint32_t base;
    uint64_t size;
    uintptr_t host_base;
    uint32_t region_id;
    uint32_t flags;
    emu68_region_kind_t kind;
};

struct emu68_cpu {
    emu68_execution_mode_t mode;
    emu68_machine_ops_t ops;
    void *opaque;
    struct emu68_machine_region regions[EMU68_MACHINE_MAX_REGIONS];
    uint16_t region_count;
    uint8_t active;
    uint8_t running;
    uint8_t pending;
    uint8_t completion_ready;
    uint8_t bus_error_pending;
    uint8_t address_error_pending;
    uint8_t stopped;
    uint64_t next_sequence;
    emu68_bus_access_t pending_access;
    uintptr_t native_resume;
    uint64_t run_budget;
    uint64_t run_start_instructions;
    uint64_t run_start_cycles;
    uint64_t run_instructions;
    uint64_t run_cycles;
    uint64_t run_published_instructions;
    uint64_t run_published_cycles;
    uint32_t run_pc;
    emu68_bus_access_t fault_access;
    uint32_t fault_pc;
    uint8_t native_frame[EMU68_MACHINE_NATIVE_FRAME_SIZE]
        __attribute__((aligned(16)));
    volatile uint32_t stop_requested;
};

uint8_t *emu68_machine_resume_frame;
uintptr_t emu68_machine_resume_address;
uint64_t emu68_machine_resume_value;
uint64_t emu68_machine_resume_outcome;
uint32_t emu68_machine_bridge_address;
uint64_t emu68_machine_bridge_write_value;
uint32_t emu68_machine_bridge_metadata;
uintptr_t emu68_machine_bridge_tu_return;
uint64_t emu68_machine_bridge_read_value;
uint64_t emu68_machine_bridge_outcome;

uint8_t emu68_machine_read_pages[EMU68_MACHINE_PAGE_COUNT];
uint8_t emu68_machine_write_pages[EMU68_MACHINE_PAGE_COUNT];

static struct emu68_cpu machine_cpu;

static void publish_progress(uint64_t instructions, uint64_t cycles,
                             uint32_t pc)
{
    uint64_t run_instructions;
    uint64_t run_cycles;
    uint64_t instruction_delta;
    uint64_t cycle_delta;

    if (!machine_cpu.running)
        return;
    run_instructions = instructions >= machine_cpu.run_start_instructions ?
        instructions - machine_cpu.run_start_instructions : 0u;
    run_cycles = cycles >= machine_cpu.run_start_cycles ?
        cycles - machine_cpu.run_start_cycles : 0u;
    instruction_delta =
        run_instructions - machine_cpu.run_published_instructions;
    cycle_delta = run_cycles - machine_cpu.run_published_cycles;
    machine_cpu.run_instructions = run_instructions;
    machine_cpu.run_cycles = run_cycles;
    machine_cpu.run_pc = pc;
    machine_cpu.run_published_instructions = run_instructions;
    machine_cpu.run_published_cycles = run_cycles;
    if ((instruction_delta != 0u || cycle_delta != 0u) &&
        machine_cpu.ops.progress)
        machine_cpu.ops.progress(machine_cpu.opaque, cycle_delta,
                                 instruction_delta, pc);
}

static void put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int public_struct_valid(uint32_t version, size_t actual, size_t required)
{
    return version == EMU68_MACHINE_ABI_VERSION && actual >= required;
}

static int valid_cpu(const emu68_cpu_t *cpu)
{
    return cpu == &machine_cpu && machine_cpu.active;
}

static int valid_range(uint32_t base, uint64_t size)
{
    return size != 0u && size <= UINT64_C(0x100000000) &&
           (uint64_t)base + size <= UINT64_C(0x100000000);
}

static uint64_t region_end(const struct emu68_machine_region *region)
{
    return (uint64_t)region->base + region->size;
}

static int region_contains(const struct emu68_machine_region *region,
                           uint32_t address, uint8_t width)
{
    uint64_t end = (uint64_t)address + width;

    return width != 0u && (uint64_t)address >= region->base &&
           end <= region_end(region) && end <= UINT64_C(0x100000000);
}

static const struct emu68_machine_region *find_region(uint32_t address,
                                                       uint8_t width)
{
    unsigned int i;

    for (i = 0; i < machine_cpu.region_count; ++i) {
        if (region_contains(&machine_cpu.regions[i], address, width))
            return &machine_cpu.regions[i];
    }
    return NULL;
}

static uint8_t *direct_pointer(uint32_t address, uint8_t width,
                               uint32_t required_flag)
{
    const struct emu68_machine_region *region = find_region(address, width);

    if (!region || region->kind != EMU68_REGION_DIRECT ||
        (region->flags & required_flag) == 0u)
        return NULL;
    return (uint8_t *)(region->host_base + ((uint64_t)address - region->base));
}

static uint8_t classify_page(uint32_t page, uint32_t required_flag)
{
    uint32_t base = page << EMU68_MACHINE_PAGE_SHIFT;
    const struct emu68_machine_region *region =
        find_region(base, (uint8_t)1u);
    uint64_t end = (uint64_t)base + EMU68_MACHINE_PAGE_SIZE;

    if (!region)
        return EMU68_PAGE_FAULT;
    if (end > region_end(region))
        return EMU68_PAGE_MIXED;
    if ((region->flags & required_flag) == 0u)
        return EMU68_PAGE_FAULT;
    if (region->kind == EMU68_REGION_DIRECT)
        return EMU68_PAGE_DIRECT;
    if (region->kind == EMU68_REGION_EXTERNAL)
        return EMU68_PAGE_EXTERNAL;
    return EMU68_PAGE_FAULT;
}

static void rebuild_pages(void)
{
    uint32_t page;

    for (page = 0; page < EMU68_MACHINE_PAGE_COUNT; ++page) {
        emu68_machine_read_pages[page] =
            classify_page(page, EMU68_REGION_READ);
        emu68_machine_write_pages[page] =
            classify_page(page, EMU68_REGION_WRITE);
    }
}

static int overlaps_existing(uint32_t base, uint64_t size)
{
    uint64_t end = (uint64_t)base + size;
    unsigned int i;

    for (i = 0; i < machine_cpu.region_count; ++i) {
        const struct emu68_machine_region *other = &machine_cpu.regions[i];
        if ((uint64_t)base < region_end(other) && end > other->base)
            return 1;
    }
    return 0;
}

static emu68_status_t validate_new_region(
    const struct emu68_machine_region *region)
{
    if (!valid_range(region->base, region->size))
        return EMU68_ERR_INVALID_ARGUMENT;
    if ((region->flags & ~EMU68_MACHINE_VALID_FLAGS) != 0u)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (overlaps_existing(region->base, region->size))
        return EMU68_ERR_OVERLAP;
    if (machine_cpu.region_count == EMU68_MACHINE_MAX_REGIONS)
        return EMU68_ERR_INTERNAL;

    return EMU68_OK;
}

static void insert_region(const struct emu68_machine_region *region)
{
    unsigned int insert = machine_cpu.region_count;

    while (insert != 0u &&
           machine_cpu.regions[insert - 1u].base > region->base) {
        machine_cpu.regions[insert] = machine_cpu.regions[insert - 1u];
        --insert;
    }
    machine_cpu.regions[insert] = *region;
    ++machine_cpu.region_count;
    rebuild_pages();
    emu68_machine_platform_invalidate_all();
}

emu68_status_t emu68_machine_create(const emu68_machine_config_t *config,
                                    emu68_cpu_t **out_cpu)
{
    if (!config || !out_cpu)
        return EMU68_ERR_INVALID_ARGUMENT;
    *out_cpu = NULL;
    if (!public_struct_valid(config->abi_version, config->struct_size,
                             sizeof(*config)))
        return EMU68_ERR_ABI_MISMATCH;
    if (config->execution_mode != EMU68_EXEC_SYNCHRONOUS &&
        config->execution_mode != EMU68_EXEC_COOPERATIVE)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (config->execution_mode == EMU68_EXEC_SYNCHRONOUS) {
        if (!config->ops ||
            !public_struct_valid(config->ops->abi_version,
                                 config->ops->struct_size,
                                 sizeof(*config->ops)) ||
            !config->ops->bus_access)
            return EMU68_ERR_INVALID_ARGUMENT;
    }
    if (machine_cpu.active)
        return EMU68_ERR_BUSY;

    memset(&machine_cpu, 0, sizeof(machine_cpu));
    machine_cpu.mode = config->execution_mode;
    if (config->ops)
        machine_cpu.ops = *config->ops;
    machine_cpu.opaque = config->opaque;
    machine_cpu.next_sequence = 1u;
    machine_cpu.active = 1u;
    rebuild_pages();
    *out_cpu = &machine_cpu;
    return EMU68_OK;
}

void emu68_machine_destroy(emu68_cpu_t *cpu)
{
    unsigned int i;

    if (!valid_cpu(cpu) || machine_cpu.running)
        return;
    for (i = 0; i < machine_cpu.region_count; ++i) {
        const struct emu68_machine_region *region = &machine_cpu.regions[i];
        if (region->kind == EMU68_REGION_DIRECT)
            emu68_machine_platform_unmap_direct(region->base, region->size);
    }
    memset(&machine_cpu, 0, sizeof(machine_cpu));
    memset(emu68_machine_read_pages, EMU68_PAGE_FAULT,
           sizeof(emu68_machine_read_pages));
    memset(emu68_machine_write_pages, EMU68_PAGE_FAULT,
           sizeof(emu68_machine_write_pages));
}

emu68_status_t emu68_machine_map_direct(
    emu68_cpu_t *cpu, const emu68_direct_region_t *public_region)
{
    struct emu68_machine_region region;
    emu68_status_t status;

    if (!valid_cpu(cpu) || !public_region)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    if (!public_struct_valid(public_region->abi_version,
                             public_region->struct_size,
                             sizeof(*public_region)))
        return EMU68_ERR_ABI_MISMATCH;
    if (!public_region->host_base ||
        (public_region->guest_base & 0xfffu) != 0u ||
        ((uintptr_t)public_region->host_base & 0xfffu) != 0u ||
        (public_region->size & 0xfffu) != 0u)
        return EMU68_ERR_INVALID_ARGUMENT;

    memset(&region, 0, sizeof(region));
    region.base = public_region->guest_base;
    region.size = public_region->size;
    region.host_base = (uintptr_t)public_region->host_base;
    region.flags = public_region->flags;
    region.kind = EMU68_REGION_DIRECT;
    status = validate_new_region(&region);
    if (status != EMU68_OK)
        return status;
    status = emu68_machine_platform_map_direct(
        region.base, region.size, (void *)region.host_base, region.flags);
    if (status != EMU68_OK)
        return status;
    insert_region(&region);
    return EMU68_OK;
}

emu68_status_t emu68_machine_map_external(
    emu68_cpu_t *cpu, const emu68_external_region_t *public_region)
{
    struct emu68_machine_region region;
    emu68_status_t status;

    if (!valid_cpu(cpu) || !public_region)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    if (!public_struct_valid(public_region->abi_version,
                             public_region->struct_size,
                             sizeof(*public_region)))
        return EMU68_ERR_ABI_MISMATCH;
    if ((public_region->flags &
         (EMU68_REGION_EXECUTE | EMU68_REGION_CACHEABLE)) != 0u)
        return EMU68_ERR_INVALID_ARGUMENT;

    memset(&region, 0, sizeof(region));
    region.base = public_region->guest_base;
    region.size = public_region->size;
    region.region_id = public_region->region_id;
    region.flags = public_region->flags;
    region.kind = EMU68_REGION_EXTERNAL;
    status = validate_new_region(&region);
    if (status != EMU68_OK)
        return status;
    emu68_machine_platform_unmap_direct(region.base, region.size);
    insert_region(&region);
    return EMU68_OK;
}

emu68_status_t emu68_machine_map_unmapped(emu68_cpu_t *cpu,
                                          uint32_t guest_base, uint64_t size)
{
    struct emu68_machine_region region;
    emu68_status_t status;

    if (!valid_cpu(cpu))
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    memset(&region, 0, sizeof(region));
    region.base = guest_base;
    region.size = size;
    region.kind = EMU68_REGION_UNMAPPED;
    status = validate_new_region(&region);
    if (status != EMU68_OK)
        return status;
    emu68_machine_platform_unmap_direct(region.base, region.size);
    insert_region(&region);
    return EMU68_OK;
}

emu68_status_t emu68_machine_unmap(emu68_cpu_t *cpu, uint32_t guest_base,
                                   uint64_t size)
{
    unsigned int i;

    if (!valid_cpu(cpu) || !valid_range(guest_base, size))
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    for (i = 0; i < machine_cpu.region_count; ++i) {
        struct emu68_machine_region *region = &machine_cpu.regions[i];
        if (region->base == guest_base && region->size == size) {
            unsigned int remaining = machine_cpu.region_count - i - 1u;
            if (region->kind == EMU68_REGION_DIRECT)
                emu68_machine_platform_unmap_direct(region->base,
                                                    region->size);
            if (remaining != 0u)
                memmove(region, region + 1, remaining * sizeof(*region));
            --machine_cpu.region_count;
            rebuild_pages();
            emu68_machine_platform_invalidate_all();
            return EMU68_OK;
        }
    }
    return EMU68_ERR_NOT_FOUND;
}

emu68_status_t emu68_machine_classify_access(
    uint32_t address, uint8_t width, emu68_access_kind_t kind,
    emu68_machine_access_result_t *result)
{
    const struct emu68_machine_region *region;
    uint32_t required;

    if (!result || (width != 1u && width != 2u && width != 4u &&
                    width != 8u && width != 16u) ||
        (kind != EMU68_ACCESS_READ && kind != EMU68_ACCESS_WRITE))
        return EMU68_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));
    region = find_region(address, width);
    if (!region) {
        result->region_kind = EMU68_REGION_UNMAPPED;
        return EMU68_OK;
    }
    required = kind == EMU68_ACCESS_READ ? EMU68_REGION_READ :
                                           EMU68_REGION_WRITE;
    if ((region->flags & required) == 0u) {
        result->region_kind = EMU68_REGION_UNMAPPED;
        return EMU68_OK;
    }
    result->region_kind = region->kind;
    result->region_id = region->region_id;
    result->flags = region->flags;
    return EMU68_OK;
}

emu68_bus_result_t emu68_machine_dispatch_access(emu68_bus_access_t *access)
{
    emu68_machine_access_result_t classification;
    uint64_t instructions = 0u;
    uint64_t cycles = 0u;
    uint32_t pc = 0u;

    if (!machine_cpu.active || !access ||
        emu68_machine_classify_access(access->address, access->width,
                                      access->kind, &classification) != EMU68_OK)
        return EMU68_BUS_ERROR;
    if (classification.region_kind != EMU68_REGION_EXTERNAL)
        return EMU68_BUS_ERROR;

    access->abi_version = EMU68_MACHINE_ABI_VERSION;
    access->struct_size = sizeof(*access);
    access->region_id = classification.region_id;
    access->sequence = machine_cpu.next_sequence++;
    emu68_machine_platform_snapshot(&instructions, &cycles, &pc, NULL);
    publish_progress(instructions, cycles, pc);
    if (machine_cpu.mode == EMU68_EXEC_SYNCHRONOUS) {
        if (!machine_cpu.ops.bus_access)
            return EMU68_BUS_ERROR;
        access->result = machine_cpu.ops.bus_access(machine_cpu.opaque, access);
        return access->result;
    }

    if (machine_cpu.pending)
        return EMU68_BUS_ERROR;
    machine_cpu.pending_access = *access;
    machine_cpu.pending = 1u;
    machine_cpu.completion_ready = 0u;
    return EMU68_BUS_COMPLETE;
}

static uint8_t current_function_code(emu68_address_space_t space,
                                     uint32_t metadata)
{
    uint8_t explicit_fc =
        (uint8_t)((metadata & EMU68_BRIDGE_META_FC_MASK) >>
                  EMU68_BRIDGE_META_FC_SHIFT);
    uint32_t sr = 0u;

    if (metadata & EMU68_BRIDGE_META_USE_SFC)
        return emu68_machine_platform_source_function_code(0);
    if (metadata & EMU68_BRIDGE_META_USE_DFC)
        return emu68_machine_platform_source_function_code(1);
    if (explicit_fc != 0u)
        return explicit_fc;
#ifdef __aarch64__
    __asm__ volatile("mrs %0, TPIDR_EL0" : "=r"(sr));
#endif
    if (space == EMU68_SPACE_CPU)
        return 7u;
    return (uint8_t)(((sr & 0x2000u) ? 4u : 0u) |
                     (space == EMU68_SPACE_PROGRAM ? 2u : 1u));
}

emu68_machine_bridge_result_t emu68_machine_bridge_dispatch(
    uint32_t address, uint64_t value, uint32_t metadata,
    uintptr_t native_resume, const void *native_frame)
{
    emu68_machine_bridge_result_t bridge = {
        .value = 0u,
        .outcome = EMU68_BRIDGE_BUS_ERROR,
    };
    emu68_bus_access_t access;
    emu68_machine_access_result_t classification;
    emu68_access_kind_t kind =
        (metadata & EMU68_BRIDGE_META_WRITE) ? EMU68_ACCESS_WRITE :
                                                EMU68_ACCESS_READ;
    emu68_address_space_t space = (emu68_address_space_t)(
        (metadata & EMU68_BRIDGE_META_SPACE_MASK) >>
        EMU68_BRIDGE_META_SPACE_SHIFT);
    uint8_t width = (uint8_t)(metadata & EMU68_BRIDGE_META_WIDTH_MASK);

    memset(&machine_cpu.fault_access, 0, sizeof(machine_cpu.fault_access));
    machine_cpu.fault_access.address = address;
    machine_cpu.fault_access.kind = kind;
    machine_cpu.fault_access.space = space;
    machine_cpu.fault_access.function_code =
        current_function_code(space, metadata);
    if (metadata & (EMU68_BRIDGE_META_USE_SFC |
                    EMU68_BRIDGE_META_USE_DFC)) {
        uint8_t fc = machine_cpu.fault_access.function_code;
        space = fc == 7u ? EMU68_SPACE_CPU :
                ((fc & 3u) == 2u ? EMU68_SPACE_PROGRAM : EMU68_SPACE_DATA);
        machine_cpu.fault_access.space = space;
    }
    machine_cpu.fault_access.width = width;
    machine_cpu.fault_pc = native_frame ?
        (uint32_t)((const uint64_t *)native_frame)[16] : machine_cpu.run_pc;

    if (space > EMU68_SPACE_CPU ||
        emu68_machine_classify_access(address, width, kind,
                                      &classification) != EMU68_OK ||
        classification.region_kind != EMU68_REGION_EXTERNAL) {
        machine_cpu.bus_error_pending = 1u;
        return bridge;
    }

    if (machine_cpu.mode == EMU68_EXEC_COOPERATIVE &&
        (!native_resume || !native_frame))
        return bridge;

    memset(&access, 0, sizeof(access));
    access.address = address;
    access.kind = kind;
    access.space = space;
    access.function_code = machine_cpu.fault_access.function_code;
    access.width = width;
    access.value_lo = value;
    if (emu68_machine_dispatch_access(&access) != EMU68_BUS_COMPLETE) {
        machine_cpu.bus_error_pending = 1u;
        return bridge;
    }
    if (machine_cpu.mode == EMU68_EXEC_COOPERATIVE) {
        machine_cpu.native_resume = native_resume;
        memcpy(machine_cpu.native_frame, native_frame,
               sizeof(machine_cpu.native_frame));
        bridge.outcome = EMU68_BRIDGE_PENDING;
        return bridge;
    }
    bridge.value = access.value_lo;
    bridge.outcome = access.result == EMU68_BUS_COMPLETE ?
                         EMU68_BRIDGE_COMPLETE : EMU68_BRIDGE_BUS_ERROR;
    if (bridge.outcome == EMU68_BRIDGE_BUS_ERROR)
        machine_cpu.bus_error_pending = 1u;
    return bridge;
}

int emu68_machine_prepare_native_resume(void)
{
    if (!machine_cpu.active || !machine_cpu.pending ||
        !machine_cpu.completion_ready || !machine_cpu.native_resume)
        return 0;

    emu68_machine_resume_frame = machine_cpu.native_frame;
    emu68_machine_resume_address = machine_cpu.native_resume;
    emu68_machine_resume_value = machine_cpu.pending_access.value_lo;
    emu68_machine_resume_outcome =
        machine_cpu.pending_access.result == EMU68_BUS_COMPLETE ?
            EMU68_BRIDGE_COMPLETE : EMU68_BRIDGE_BUS_ERROR;
    if (emu68_machine_resume_outcome == EMU68_BRIDGE_BUS_ERROR)
        machine_cpu.bus_error_pending = 1u;
    machine_cpu.pending = 0u;
    machine_cpu.completion_ready = 0u;
    machine_cpu.native_resume = 0u;
    return 1;
}

int emu68_machine_native_access_pending(void)
{
    return machine_cpu.active && machine_cpu.pending &&
           !machine_cpu.completion_ready;
}

int emu68_machine_native_bus_error_pending(void)
{
    return machine_cpu.active && machine_cpu.bus_error_pending;
}

int emu68_machine_enter_bus_error(void)
{
    emu68_machine_arch_state_t state;
    uint8_t *frame;
    uint8_t *vector;
    uint32_t sp;
    uint16_t old_sr;
    uint16_t ssw;
    uint16_t size_code;

    if (!machine_cpu.bus_error_pending ||
        emu68_machine_platform_get_arch_state(&state) != EMU68_OK)
        return 0;
    old_sr = state.sr;
    if ((old_sr & 0x2000u) == 0u) {
        state.usp = state.a7;
        sp = (old_sr & 0x1000u) ? state.msp : state.isp;
    } else {
        sp = state.a7;
    }
    if (machine_cpu.address_error_pending) {
        if (sp < 12u)
            return 0;
        sp -= 12u;
        frame = direct_pointer(sp, 12u, EMU68_REGION_WRITE);
        vector = direct_pointer(state.vbr + 12u, 4u, EMU68_REGION_READ);
        if (!frame || !vector)
            return 0;
        put_be16(frame, old_sr);
        put_be32(frame + 2u, machine_cpu.fault_pc);
        put_be16(frame + 6u, 0x200cu);
        put_be32(frame + 8u, machine_cpu.fault_access.address & ~1u);
    } else {
        if (sp < 60u)
            return 0;
        sp -= 60u;
        frame = direct_pointer(sp, 60u, EMU68_REGION_WRITE);
        vector = direct_pointer(state.vbr + 8u, 4u, EMU68_REGION_READ);
        if (!frame || !vector)
            return 0;

        memset(frame, 0, 60u);
        put_be16(frame, old_sr);
        put_be32(frame + 2u, machine_cpu.fault_pc);
        put_be16(frame + 6u, 0x7008u);
        switch (machine_cpu.fault_access.width) {
        case 1u: size_code = 1u; break;
        case 2u: size_code = 2u; break;
        default: size_code = 0u; break;
        }
        ssw = (uint16_t)((machine_cpu.fault_access.kind == EMU68_ACCESS_READ ?
                              1u << 8 : 0u) |
                         (size_code << 5) |
                         (machine_cpu.fault_access.function_code & 7u));
        put_be16(frame + 12u, ssw);
        put_be32(frame + 20u, machine_cpu.fault_access.address);
    }

    state.sr = (uint16_t)((old_sr | 0x2000u) & ~(0xc000u));
    state.a7 = sp;
    if (state.sr & 0x1000u)
        state.msp = sp;
    else
        state.isp = sp;
    state.pc = get_be32(vector);
    if (emu68_machine_platform_set_arch_state(&state) != EMU68_OK)
        return 0;
    emu68_machine_platform_add_cycles(
        machine_cpu.address_error_pending ? 30u : 50u);
    machine_cpu.bus_error_pending = 0u;
    machine_cpu.address_error_pending = 0u;
    return 1;
}

int emu68_machine_instruction_fetch_allowed(uint32_t address, uint8_t width)
{
    const struct emu68_machine_region *region;

#if defined(BELLATRIX_EMU68_FAULT_DRIVEN) && BELLATRIX_EMU68_FAULT_DRIVEN
    (void)address;
    (void)width;
    return 1;
#endif
    if (!machine_cpu.active)
        return 1;
    if ((address & 1u) != 0u) {
        if (!machine_cpu.bus_error_pending) {
            memset(&machine_cpu.fault_access, 0,
                   sizeof(machine_cpu.fault_access));
            machine_cpu.fault_access.address = address;
            machine_cpu.fault_access.kind = EMU68_ACCESS_READ;
            machine_cpu.fault_access.space = EMU68_SPACE_PROGRAM;
            machine_cpu.fault_access.function_code =
                current_function_code(EMU68_SPACE_PROGRAM, 0u);
            machine_cpu.fault_access.width = width;
            machine_cpu.fault_pc = address;
            machine_cpu.address_error_pending = 1u;
            machine_cpu.bus_error_pending = 1u;
        }
        return 0;
    }
    region = find_region(address, width);
    if (region && region->kind == EMU68_REGION_DIRECT &&
        (region->flags & (EMU68_REGION_READ | EMU68_REGION_EXECUTE)) ==
            (EMU68_REGION_READ | EMU68_REGION_EXECUTE))
        return 1;

    if (!machine_cpu.bus_error_pending) {
        memset(&machine_cpu.fault_access, 0,
               sizeof(machine_cpu.fault_access));
        machine_cpu.fault_access.address = address;
        machine_cpu.fault_access.kind = EMU68_ACCESS_READ;
        machine_cpu.fault_access.space = EMU68_SPACE_PROGRAM;
        machine_cpu.fault_access.function_code =
            current_function_code(EMU68_SPACE_PROGRAM, 0u);
        machine_cpu.fault_access.width = width;
        machine_cpu.fault_pc = address;
        machine_cpu.bus_error_pending = 1u;
    }
    return 0;
}

int emu68_machine_dispatch_quantum_progress(uint64_t retired_instructions,
                                            uint32_t pc)
{
    uint64_t delta;
    uint64_t modeled_cycles = 0u;
    int stopped = 0;

    if (!machine_cpu.active || !machine_cpu.running)
        return 0;
    if (retired_instructions < machine_cpu.run_start_instructions)
        retired_instructions = machine_cpu.run_start_instructions;
    delta = retired_instructions - machine_cpu.run_start_instructions;
    machine_cpu.run_instructions = delta;
    emu68_machine_platform_snapshot(NULL, &modeled_cycles, NULL, &stopped);
    machine_cpu.run_cycles = modeled_cycles >= machine_cpu.run_start_cycles ?
        modeled_cycles - machine_cpu.run_start_cycles : 0u;
    machine_cpu.run_pc = pc;
    publish_progress(retired_instructions, modeled_cycles, pc);
    machine_cpu.stopped = stopped != 0;
    return machine_cpu.pending || machine_cpu.bus_error_pending ||
           machine_cpu.stopped ||
           __atomic_load_n(&machine_cpu.stop_requested, __ATOMIC_ACQUIRE) ||
           machine_cpu.run_cycles >= machine_cpu.run_budget;
}

int emu68_machine_runtime_active(void)
{
#if defined(BELLATRIX_EMU68_FAULT_DRIVEN) && BELLATRIX_EMU68_FAULT_DRIVEN
    return 0;
#else
    return machine_cpu.active;
#endif
}

emu68_status_t emu68_machine_get_pending_access(
    emu68_cpu_t *cpu, emu68_bus_access_t *out_access)
{
    if (!valid_cpu(cpu) || !out_access)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (!machine_cpu.pending)
        return EMU68_ERR_NOT_FOUND;
    *out_access = machine_cpu.pending_access;
    return EMU68_OK;
}

emu68_status_t emu68_machine_run(emu68_cpu_t *cpu, uint64_t cycle_budget,
                                 emu68_run_result_t *result)
{
    uint64_t instructions = 0u;
    uint64_t cycles = 0u;
    uint32_t pc = 0u;
    int stopped = 0;

    if (!valid_cpu(cpu) || !result)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (!public_struct_valid(result->abi_version, result->struct_size,
                             sizeof(*result)))
        return EMU68_ERR_ABI_MISMATCH;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;

    emu68_machine_platform_snapshot(&instructions, &cycles, &pc, &stopped);
    memset(result, 0, sizeof(*result));
    result->abi_version = EMU68_MACHINE_ABI_VERSION;
    result->struct_size = sizeof(*result);
    result->pc = pc;
    if (machine_cpu.pending && !machine_cpu.completion_ready) {
        result->reason = EMU68_STOP_EXTERNAL_ACCESS;
        return EMU68_OK;
    }
    if (machine_cpu.bus_error_pending) {
        result->reason = EMU68_STOP_BUS_ERROR;
        return EMU68_OK;
    }
    if (__atomic_load_n(&machine_cpu.stop_requested, __ATOMIC_ACQUIRE)) {
        result->reason = EMU68_STOP_REQUESTED;
        __atomic_store_n(&machine_cpu.stop_requested, 0u, __ATOMIC_RELEASE);
        return EMU68_OK;
    }
    if (stopped) {
        result->reason = EMU68_STOP_STOPPED;
        return EMU68_OK;
    }
    if (cycle_budget == 0u) {
        result->reason = EMU68_STOP_BUDGET;
        return EMU68_OK;
    }

    machine_cpu.run_budget = cycle_budget;
    machine_cpu.run_start_instructions = instructions;
    machine_cpu.run_start_cycles = cycles;
    machine_cpu.run_instructions = 0u;
    machine_cpu.run_cycles = 0u;
    machine_cpu.run_published_instructions = 0u;
    machine_cpu.run_published_cycles = 0u;
    machine_cpu.run_pc = pc;
    machine_cpu.stopped = 0u;
    machine_cpu.running = 1u;
    emu68_machine_platform_run();

    emu68_machine_platform_snapshot(&instructions, &cycles, &pc, &stopped);
    publish_progress(instructions, cycles, pc);
    machine_cpu.running = 0u;
    if (instructions >= machine_cpu.run_start_instructions)
        machine_cpu.run_instructions =
            instructions - machine_cpu.run_start_instructions;
    machine_cpu.run_cycles = cycles >= machine_cpu.run_start_cycles ?
        cycles - machine_cpu.run_start_cycles : 0u;
    result->cycles_executed = machine_cpu.run_cycles;
    result->instructions_executed = machine_cpu.run_instructions;
    result->pc = pc;
    if (machine_cpu.pending && !machine_cpu.completion_ready)
        result->reason = EMU68_STOP_EXTERNAL_ACCESS;
    else if (machine_cpu.bus_error_pending)
        result->reason = EMU68_STOP_BUS_ERROR;
    else if (__atomic_load_n(&machine_cpu.stop_requested, __ATOMIC_ACQUIRE)) {
        result->reason = EMU68_STOP_REQUESTED;
        __atomic_store_n(&machine_cpu.stop_requested, 0u, __ATOMIC_RELEASE);
    } else if (stopped)
        result->reason = EMU68_STOP_STOPPED;
    else
        result->reason = EMU68_STOP_BUDGET;
    return EMU68_OK;
}

static int completion_identity_matches(const emu68_bus_access_t *completion)
{
    const emu68_bus_access_t *pending = &machine_cpu.pending_access;

    return completion->sequence == pending->sequence &&
           completion->address == pending->address &&
           completion->region_id == pending->region_id &&
           completion->kind == pending->kind &&
           completion->space == pending->space &&
           completion->function_code == pending->function_code &&
           completion->width == pending->width;
}

emu68_status_t emu68_machine_complete_access(
    emu68_cpu_t *cpu, const emu68_bus_access_t *completion)
{
    if (!valid_cpu(cpu) || !completion)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (!public_struct_valid(completion->abi_version,
                             completion->struct_size,
                             sizeof(*completion)))
        return EMU68_ERR_ABI_MISMATCH;
    if (machine_cpu.mode != EMU68_EXEC_COOPERATIVE || !machine_cpu.pending)
        return EMU68_ERR_NOT_FOUND;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    if (!completion_identity_matches(completion))
        return EMU68_ERR_ACCESS;
    if (completion->result != EMU68_BUS_COMPLETE &&
        completion->result != EMU68_BUS_ERROR)
        return EMU68_ERR_INVALID_ARGUMENT;

    machine_cpu.pending_access.result = completion->result;
    machine_cpu.pending_access.value_lo = completion->value_lo;
    machine_cpu.pending_access.value_hi = completion->value_hi;
    machine_cpu.completion_ready = 1u;
    return EMU68_OK;
}

emu68_status_t emu68_machine_reset(emu68_cpu_t *cpu,
                                   const emu68_reset_state_t *state)
{
    if (!valid_cpu(cpu) || !state)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    if (!public_struct_valid(state->abi_version, state->struct_size,
                             sizeof(*state)))
        return EMU68_ERR_ABI_MISMATCH;

    machine_cpu.pending = 0u;
    machine_cpu.completion_ready = 0u;
    machine_cpu.native_resume = 0u;
    machine_cpu.bus_error_pending = 0u;
    machine_cpu.address_error_pending = 0u;
    __atomic_store_n(&machine_cpu.stop_requested, 0u, __ATOMIC_RELEASE);
    return emu68_machine_platform_reset(state->initial_ssp,
                                        state->initial_pc);
}

void emu68_machine_request_stop(emu68_cpu_t *cpu)
{
    if (valid_cpu(cpu)) {
        __atomic_store_n(&machine_cpu.stop_requested, 1u, __ATOMIC_RELEASE);
        emu68_machine_platform_wake();
    }
}

emu68_status_t emu68_machine_set_ipl(emu68_cpu_t *cpu, unsigned level)
{
    if (!valid_cpu(cpu))
        return EMU68_ERR_INVALID_ARGUMENT;
    if (level > 7u)
        return EMU68_ERR_INVALID_ARGUMENT;
    return emu68_machine_platform_set_ipl(level);
}

emu68_status_t emu68_machine_invalidate_code(emu68_cpu_t *cpu,
                                              uint32_t guest_base,
                                              uint64_t size)
{
    if (!valid_cpu(cpu) || !valid_range(guest_base, size))
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    emu68_machine_platform_invalidate(guest_base, size);
    return EMU68_OK;
}

emu68_status_t emu68_machine_invalidate_all_code(emu68_cpu_t *cpu)
{
    if (!valid_cpu(cpu))
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    emu68_machine_platform_invalidate_all();
    return EMU68_OK;
}
```

### `src/cpu/emu68/emu68_machine_platform.c`

```c
#include "cpu/emu68/emu68_machine_platform.h"

#include "A64.h"
#include "M68k.h"
#include "cache.h"
#include "mmu.h"

#include <string.h>

extern struct M68KState *__m68k_state;
extern void mmu_unmap(uintptr_t virt, uintptr_t length);
extern void MainLoopWindow(void);

emu68_status_t emu68_machine_platform_map_direct(
    uint32_t guest_base, uint64_t size, void *host_base, uint32_t flags)
{
    uint32_t attr = MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0;
    uintptr_t phys = mmu_virt2phys((uintptr_t)host_base);

    if (phys == (uintptr_t)-1)
        return EMU68_ERR_ACCESS;

    attr |= (flags & EMU68_REGION_CACHEABLE) ? MMU_ATTR_CACHED :
                                               MMU_ATTR_UNCACHED;
    if ((flags & EMU68_REGION_WRITE) == 0u)
        attr |= MMU_READ_ONLY;
    mmu_map(phys, guest_base, (uintptr_t)size, attr, 0u);
    return EMU68_OK;
}

void emu68_machine_platform_unmap_direct(uint32_t guest_base, uint64_t size)
{
    uintptr_t first = (uintptr_t)guest_base & ~(uintptr_t)0xfffu;
    uintptr_t end = (uintptr_t)((uint64_t)guest_base + size + 0xfffu) &
                    ~(uintptr_t)0xfffu;

    mmu_unmap(first, end - first);
}

void emu68_machine_platform_invalidate(uint32_t guest_base, uint64_t size)
{
    if (size > UINT32_MAX)
        cache_invalidate_all(ICACHE);
    else
        cache_invalidate_range(ICACHE, guest_base, (uint32_t)size);
}

void emu68_machine_platform_invalidate_all(void)
{
    cache_invalidate_all(ICACHE);
}

emu68_status_t emu68_machine_platform_reset(uint32_t initial_ssp,
                                            uint32_t initial_pc)
{
    uint32_t jit_control;
    uint32_t jit_control2;

    if (!__m68k_state)
        return EMU68_ERR_INTERNAL;

    jit_control = __m68k_state->JIT_CONTROL;
    jit_control2 = __m68k_state->JIT_CONTROL2;
    /* A machine run may overshoot its budget by at most one instruction.
     * One-instruction translation units make every dispatcher return point
     * an architectural instruction boundary. */
    jit_control &= ~(JCCB_INSN_DEPTH_MASK << JCCB_INSN_DEPTH);
    jit_control |= 1u << JCCB_INSN_DEPTH;
    jit_control &= ~(JCCB_INLINE_RANGE_MASK << JCCB_INLINE_RANGE);
    memset(__m68k_state, 0, sizeof(*__m68k_state));
    __m68k_state->JIT_CONTROL = jit_control;
    __m68k_state->JIT_CONTROL2 = jit_control2;
    __m68k_state->ISP.u32 = initial_ssp;
    __m68k_state->A[7].u32 = initial_ssp;
    __m68k_state->PC = initial_pc;
    __m68k_state->SR = SR_S | SR_IPL;
    __m68k_state->CACR = CACR_IE;
    cache_invalidate_all(ICACHE);
    return EMU68_OK;
}

emu68_status_t emu68_machine_platform_set_ipl(unsigned level)
{
    if (!__m68k_state)
        return EMU68_ERR_INTERNAL;
    __atomic_store_n(&__m68k_state->INT.IPL, (uint8_t)level, __ATOMIC_RELEASE);
    emu68_machine_platform_wake();
    return EMU68_OK;
}

void emu68_machine_platform_wake(void)
{
#ifdef __aarch64__
    __asm__ volatile("sev" ::: "memory");
#endif
}

void emu68_machine_platform_run(void)
{
    MainLoopWindow();
}

void emu68_machine_platform_snapshot(uint64_t *instructions, uint64_t *cycles,
                                     uint32_t *pc, int *stopped)
{
    if (instructions)
        *instructions = __m68k_state ? __m68k_state->INSN_COUNT : 0u;
    if (cycles)
        *cycles = __m68k_state ? __m68k_state->CYCLE_COUNT : 0u;
    if (pc)
        *pc = __m68k_state ? __m68k_state->PC : 0u;
    if (stopped)
    {
        int is_stopped = __m68k_state ? (__m68k_state->STOPPED != 0u) : 0;
        if (is_stopped) {
            unsigned mask = (__m68k_state->SR & SR_IPL) >> SRB_IPL;
            unsigned level = __m68k_state->INT.IPL;
            if (level == 7u || level > mask) {
                __m68k_state->STOPPED = 0u;
                is_stopped = 0;
            }
        }
        *stopped = is_stopped;
    }
}

void emu68_machine_platform_add_cycles(uint32_t cycles)
{
    if (__m68k_state)
        __m68k_state->CYCLE_COUNT += cycles;
}

uint8_t emu68_machine_platform_source_function_code(int destination)
{
    if (!__m68k_state)
        return 0u;
    return destination ? (__m68k_state->DFC & 7u) :
                         (__m68k_state->SFC & 7u);
}

emu68_status_t emu68_machine_platform_get_arch_state(
    emu68_machine_arch_state_t *state)
{
    if (!state || !__m68k_state)
        return EMU68_ERR_INTERNAL;
    state->pc = __m68k_state->PC;
    state->a7 = __m68k_state->A[7].u32;
    state->usp = __m68k_state->USP.u32;
    state->isp = __m68k_state->ISP.u32;
    state->msp = __m68k_state->MSP.u32;
    state->vbr = __m68k_state->VBR;
    state->sr = __m68k_state->SR;
    return EMU68_OK;
}

emu68_status_t emu68_machine_platform_set_arch_state(
    const emu68_machine_arch_state_t *state)
{
    if (!state || !__m68k_state)
        return EMU68_ERR_INTERNAL;
    __m68k_state->PC = state->pc;
    __m68k_state->A[7].u32 = state->a7;
    __m68k_state->USP.u32 = state->usp;
    __m68k_state->ISP.u32 = state->isp;
    __m68k_state->MSP.u32 = state->msp;
    __m68k_state->VBR = state->vbr;
    __m68k_state->SR = state->sr;
    return EMU68_OK;
}
```

### `src/cpu/emu68/emu68_machine_emit.c`

```c
#include "cpu/emu68/emu68_machine_emit.h"

#include "cpu/emu68/emu68_machine_internal.h"

#include "A64.h"
#include "RegisterAllocator.h"

uint32_t emu68_machine_translation_metadata;

static uint32_t *emit_pointer(uint32_t *ptr, uint8_t reg, uintptr_t value)
{
    *ptr++ = mov64_immed_u16(reg, (uint16_t)value, 0);
    *ptr++ = movk64_immed_u16(reg, (uint16_t)(value >> 16), 1);
    *ptr++ = movk64_immed_u16(reg, (uint16_t)(value >> 32), 2);
    *ptr++ = movk64_immed_u16(reg, (uint16_t)(value >> 48), 3);
    return ptr;
}

static uint32_t *emit_native_load(uint32_t *ptr, uint8_t address_reg,
                                  uint8_t value_reg, uint8_t width,
                                  int sign_extend)
{
    switch (width) {
    case 1u:
        *ptr++ = sign_extend ? ldrsb_offset(address_reg, value_reg, 0) :
                              ldrb_offset(address_reg, value_reg, 0);
        break;
    case 2u:
        *ptr++ = sign_extend ? ldrsh_offset(address_reg, value_reg, 0) :
                              ldrh_offset(address_reg, value_reg, 0);
        break;
    case 4u:
        *ptr++ = ldr_offset(address_reg, value_reg, 0);
        break;
    case 8u:
        *ptr++ = ldr64_offset(address_reg, value_reg, 0);
        break;
    default:
        *ptr++ = brk(0x68u);
        break;
    }
    return ptr;
}

static uint32_t *emit_native_store(uint32_t *ptr, uint8_t address_reg,
                                   uint8_t value_reg, uint8_t width)
{
    switch (width) {
    case 1u:
        *ptr++ = strb_offset(address_reg, value_reg, 0);
        break;
    case 2u:
        *ptr++ = strh_offset(address_reg, value_reg, 0);
        break;
    case 4u:
        *ptr++ = str_offset(address_reg, value_reg, 0);
        break;
    case 8u:
        *ptr++ = str64_offset(address_reg, value_reg, 0);
        break;
    default:
        *ptr++ = brk(0x68u);
        break;
    }
    return ptr;
}

static uint32_t *emit_access_prefix(uint32_t *ptr, uint8_t address_reg,
                                    uint8_t width, int write,
                                    uint8_t *page_reg, uint8_t *class_reg,
                                    uint32_t **slow_branch,
                                    uint32_t **boundary_branch,
                                    uint32_t **overflow_branch)
{
    uintptr_t table = write ? (uintptr_t)emu68_machine_write_pages :
                              (uintptr_t)emu68_machine_read_pages;
    uint8_t limit_reg;

    *page_reg = RA_AllocARMRegister(&ptr);
    *class_reg = RA_AllocARMRegister(&ptr);
    limit_reg = RA_AllocARMRegister(&ptr);
    *ptr++ = lsr(*page_reg, address_reg, EMU68_MACHINE_PAGE_SHIFT);
    ptr = emit_pointer(ptr, *class_reg, table);
    *ptr++ = ldrb_regoffset(*class_reg, *class_reg, *page_reg, UXTW);
    *ptr++ = cmp_immed(*class_reg, EMU68_PAGE_DIRECT);
    *slow_branch = ptr++;
    *boundary_branch = NULL;
    *overflow_branch = NULL;

    if (width > 1u) {
        uint32_t *within_page;

        *ptr++ = ubfx(*page_reg, address_reg, 0,
                      EMU68_MACHINE_PAGE_SHIFT);
        *ptr++ = mov_immed_u16(limit_reg,
                              (uint16_t)(EMU68_MACHINE_PAGE_SIZE - width), 0);
        *ptr++ = cmp_reg(*page_reg, limit_reg, LSL, 0);
        within_page = ptr++;
        *ptr++ = adds_immed(*page_reg, address_reg, width - 1u);
        *overflow_branch = ptr++;
        *ptr++ = lsr(*page_reg, *page_reg, EMU68_MACHINE_PAGE_SHIFT);
        ptr = emit_pointer(ptr, *class_reg, table);
        *ptr++ = ldrb_regoffset(*class_reg, *class_reg, *page_reg, UXTW);
        *ptr++ = cmp_immed(*class_reg, EMU68_PAGE_DIRECT);
        *boundary_branch = ptr++;
        *within_page = b_cc(A64_CC_LS, ptr - within_page);
    }
    RA_FreeARMRegister(&ptr, limit_reg);
    return ptr;
}

static uint32_t *emit_slow_call(uint32_t *ptr, uint8_t address_reg,
                                uint8_t value_reg, uint8_t width, int write,
                                int sign_extend, uint32_t metadata)
{
    uint8_t helper = RA_AllocARMRegister(&ptr);
    uint8_t outcome = RA_AllocARMRegister(&ptr);
    uint32_t *not_pending;
    uint32_t *complete;

    ptr = emit_pointer(ptr, helper, (uintptr_t)&emu68_machine_bridge_address);
    *ptr++ = str_offset(helper, address_reg, 0);
    ptr = emit_pointer(ptr, helper,
                       (uintptr_t)&emu68_machine_bridge_write_value);
    if (write)
        *ptr++ = str64_offset(helper, value_reg, 0);
    else {
        *ptr++ = mov64_immed_u16(outcome, 0u, 0);
        *ptr++ = str64_offset(helper, outcome, 0);
    }
    ptr = emit_pointer(ptr, helper, (uintptr_t)&emu68_machine_bridge_metadata);
    *ptr++ = mov_immed_u16(outcome, (uint16_t)metadata, 0);
    *ptr++ = str_offset(helper, outcome, 0);
    ptr = emit_pointer(ptr, helper, (uintptr_t)&emu68_machine_bridge_tu_return);
    *ptr++ = str64_offset(helper, 30, 0);
    ptr = emit_pointer(ptr, helper, (uintptr_t)emu68_machine_native_bridge);
    *ptr++ = blr(helper);
    ptr = emit_pointer(ptr, helper, (uintptr_t)&emu68_machine_bridge_tu_return);
    *ptr++ = ldr64_offset(helper, 30, 0);
    ptr = emit_pointer(ptr, helper, (uintptr_t)&emu68_machine_bridge_outcome);
    *ptr++ = ldr64_offset(helper, outcome, 0);
    *ptr++ = cmp_immed(outcome, EMU68_BRIDGE_PENDING);
    not_pending = ptr++;
    *ptr++ = ret();
    *not_pending = b_cc(A64_CC_NE, ptr - not_pending);
    *ptr++ = cmp_immed(outcome, EMU68_BRIDGE_COMPLETE);
    complete = ptr++;
    *ptr++ = ret();
    *complete = b_cc(A64_CC_EQ, ptr - complete);
    if (!write) {
        ptr = emit_pointer(ptr, helper,
                           (uintptr_t)&emu68_machine_bridge_read_value);
        *ptr++ = width == 8u ? ldr64_offset(helper, value_reg, 0) :
                              ldr_offset(helper, value_reg, 0);
        if (sign_extend && width == 1u)
            *ptr++ = sxtb(value_reg, value_reg);
        else if (sign_extend && width == 2u)
            *ptr++ = sxth(value_reg, value_reg);
    }
    RA_FreeARMRegister(&ptr, outcome);
    RA_FreeARMRegister(&ptr, helper);
    return ptr;
}

uint32_t *emu68_machine_emit_load(uint32_t *ptr, uint8_t address_reg,
                                  uint8_t value_reg, uint8_t width,
                                  int sign_extend, uint32_t metadata)
{
    uint8_t page_reg;
    uint8_t class_reg;
    uint32_t *slow_branch;
    uint32_t *boundary_branch;
    uint32_t *overflow_branch;
    uint32_t *done;
    uint32_t *slow;

    if (!emu68_machine_runtime_active())
        return emit_native_load(ptr, address_reg, value_reg, width,
                                sign_extend);
    ptr = emit_access_prefix(ptr, address_reg, width, 0, &page_reg,
                             &class_reg, &slow_branch, &boundary_branch,
                             &overflow_branch);
    ptr = emit_native_load(ptr, address_reg, value_reg, width, sign_extend);
    done = ptr++;
    slow = ptr;
    ptr = emit_slow_call(ptr, address_reg, value_reg, width, 0, sign_extend,
                         metadata | width);
    *slow_branch = b_cc(A64_CC_NE, slow - slow_branch);
    if (boundary_branch)
        *boundary_branch = b_cc(A64_CC_NE, slow - boundary_branch);
    if (overflow_branch)
        *overflow_branch = b_cc(A64_CC_CS, slow - overflow_branch);
    *done = b(ptr - done);
    RA_FreeARMRegister(&ptr, class_reg);
    RA_FreeARMRegister(&ptr, page_reg);
    return ptr;
}

uint32_t *emu68_machine_emit_store(uint32_t *ptr, uint8_t address_reg,
                                   uint8_t value_reg, uint8_t width,
                                   uint32_t metadata)
{
    uint8_t page_reg;
    uint8_t class_reg;
    uint32_t *slow_branch;
    uint32_t *boundary_branch;
    uint32_t *overflow_branch;
    uint32_t *done;
    uint32_t *slow;

    if (!emu68_machine_runtime_active())
        return emit_native_store(ptr, address_reg, value_reg, width);
    ptr = emit_access_prefix(ptr, address_reg, width, 1, &page_reg,
                             &class_reg, &slow_branch, &boundary_branch,
                             &overflow_branch);
    ptr = emit_native_store(ptr, address_reg, value_reg, width);
    done = ptr++;
    slow = ptr;
    ptr = emit_slow_call(ptr, address_reg, value_reg, width, 1, 0,
                         metadata | EMU68_BRIDGE_META_WRITE | width);
    *slow_branch = b_cc(A64_CC_NE, slow - slow_branch);
    if (boundary_branch)
        *boundary_branch = b_cc(A64_CC_NE, slow - boundary_branch);
    if (overflow_branch)
        *overflow_branch = b_cc(A64_CC_CS, slow - overflow_branch);
    *done = b(ptr - done);
    RA_FreeARMRegister(&ptr, class_reg);
    RA_FreeARMRegister(&ptr, page_reg);
    return ptr;
}

static uint32_t *emit_offset_address(uint32_t *ptr, uint8_t base_reg,
                                     int32_t offset, uint8_t address_reg,
                                     uint8_t offset_reg)
{
    if (offset == 0) {
        *ptr++ = mov_reg(address_reg, base_reg);
        return ptr;
    }
    *ptr++ = movw_immed_u16(offset_reg, (uint16_t)offset);
    *ptr++ = movt_immed_u16(offset_reg,
                            (uint16_t)((uint32_t)offset >> 16));
    *ptr++ = add_reg(address_reg, base_reg, offset_reg, LSL, 0);
    return ptr;
}

uint32_t *emu68_machine_emit_load_offset(
    uint32_t *ptr, uint8_t base_reg, int32_t offset, uint8_t value_reg,
    uint8_t width, int sign_extend, uint32_t metadata)
{
    uint8_t address_reg = RA_AllocARMRegister(&ptr);
    uint8_t offset_reg = RA_AllocARMRegister(&ptr);

    ptr = emit_offset_address(ptr, base_reg, offset, address_reg, offset_reg);
    ptr = emu68_machine_emit_load(ptr, address_reg, value_reg, width,
                                  sign_extend, metadata);
    RA_FreeARMRegister(&ptr, offset_reg);
    RA_FreeARMRegister(&ptr, address_reg);
    return ptr;
}

uint32_t *emu68_machine_emit_store_offset(
    uint32_t *ptr, uint8_t base_reg, int32_t offset, uint8_t value_reg,
    uint8_t width, uint32_t metadata)
{
    uint8_t address_reg = RA_AllocARMRegister(&ptr);
    uint8_t offset_reg = RA_AllocARMRegister(&ptr);

    ptr = emit_offset_address(ptr, base_reg, offset, address_reg, offset_reg);
    ptr = emu68_machine_emit_store(ptr, address_reg, value_reg, width,
                                   metadata);
    RA_FreeARMRegister(&ptr, offset_reg);
    RA_FreeARMRegister(&ptr, address_reg);
    return ptr;
}
```

### `src/cpu/emu68/emu68_machine_bridge.S`

```c
/* Explicit slow bridge used by generated guest-memory code.
 *
 * Arguments and results use private mailboxes so the generated code does not
 * have to clobber ABI argument registers before they are saved. The bridge
 * preserves every JIT-visible GPR, SIMD register and status register. Normal
 * external traffic therefore crosses an ordinary call boundary, never an ARM
 * exception boundary.
 */

    .text
    .align  4
    .global emu68_machine_native_bridge
    .type   emu68_machine_native_bridge, %function
emu68_machine_native_bridge:
    sub     sp, sp, #800
    str     x0,       [sp, #768]
    str     x1,       [sp, #776]
    str     x30,      [sp, #784]
    stp     x2,  x3,  [sp, #0]
    stp     x4,  x5,  [sp, #16]
    stp     x6,  x7,  [sp, #32]
    stp     x8,  x9,  [sp, #48]
    stp     x10, x11, [sp, #64]
    stp     x12, x13, [sp, #80]
    stp     x14, x15, [sp, #96]
    stp     x16, x17, [sp, #112]
    stp     x18, x19, [sp, #128]
    stp     x20, x21, [sp, #144]
    stp     x22, x23, [sp, #160]
    stp     x24, x25, [sp, #176]
    stp     x26, x27, [sp, #192]
    stp     x28, x29, [sp, #208]
    str     x30,      [sp, #224]
    mrs     x3, nzcv
    mrs     x4, fpcr
    mrs     x5, fpsr
    stp     x3, x4,   [sp, #232]
    str     x5,       [sp, #248]

    stp     q0,  q1,  [sp, #256]
    stp     q2,  q3,  [sp, #288]
    stp     q4,  q5,  [sp, #320]
    stp     q6,  q7,  [sp, #352]
    stp     q8,  q9,  [sp, #384]
    stp     q10, q11, [sp, #416]
    stp     q12, q13, [sp, #448]
    stp     q14, q15, [sp, #480]
    stp     q16, q17, [sp, #512]
    stp     q18, q19, [sp, #544]
    stp     q20, q21, [sp, #576]
    stp     q22, q23, [sp, #608]
    stp     q24, q25, [sp, #640]
    stp     q26, q27, [sp, #672]
    stp     q28, q29, [sp, #704]
    stp     q30, q31, [sp, #736]

    adrp    x6, emu68_machine_bridge_tu_return
    ldr     x6, [x6, #:lo12:emu68_machine_bridge_tu_return]
    str     x6, [sp, #224]
    adrp    x6, emu68_machine_bridge_address
    ldr     w0, [x6, #:lo12:emu68_machine_bridge_address]
    adrp    x6, emu68_machine_bridge_write_value
    ldr     x1, [x6, #:lo12:emu68_machine_bridge_write_value]
    adrp    x6, emu68_machine_bridge_metadata
    ldr     w2, [x6, #:lo12:emu68_machine_bridge_metadata]
    mov     x3, x30
    mov     x4, sp
    bl      emu68_machine_bridge_dispatch

    adrp    x6, emu68_machine_bridge_read_value
    str     x0, [x6, #:lo12:emu68_machine_bridge_read_value]
    adrp    x6, emu68_machine_bridge_outcome
    str     x1, [x6, #:lo12:emu68_machine_bridge_outcome]

    ldp     q0,  q1,  [sp, #256]
    ldp     q2,  q3,  [sp, #288]
    ldp     q4,  q5,  [sp, #320]
    ldp     q6,  q7,  [sp, #352]
    ldp     q8,  q9,  [sp, #384]
    ldp     q10, q11, [sp, #416]
    ldp     q12, q13, [sp, #448]
    ldp     q14, q15, [sp, #480]
    ldp     q16, q17, [sp, #512]
    ldp     q18, q19, [sp, #544]
    ldp     q20, q21, [sp, #576]
    ldp     q22, q23, [sp, #608]
    ldp     q24, q25, [sp, #640]
    ldp     q26, q27, [sp, #672]
    ldp     q28, q29, [sp, #704]
    ldp     q30, q31, [sp, #736]

    ldp     x3, x4, [sp, #232]
    ldr     x5,     [sp, #248]
    msr     nzcv, x3
    msr     fpcr, x4
    msr     fpsr, x5
    ldp     x2,  x3,  [sp, #0]
    ldp     x4,  x5,  [sp, #16]
    ldp     x6,  x7,  [sp, #32]
    ldp     x8,  x9,  [sp, #48]
    ldp     x10, x11, [sp, #64]
    ldp     x12, x13, [sp, #80]
    ldp     x14, x15, [sp, #96]
    ldp     x16, x17, [sp, #112]
    ldp     x18, x19, [sp, #128]
    ldp     x20, x21, [sp, #144]
    ldp     x22, x23, [sp, #160]
    ldp     x24, x25, [sp, #176]
    ldp     x26, x27, [sp, #192]
    ldp     x28, x29, [sp, #208]
    ldr     x0,       [sp, #768]
    ldr     x1,       [sp, #776]
    ldr     x30,      [sp, #784]
    add     sp, sp, #800
    ret

    .size emu68_machine_native_bridge, .-emu68_machine_native_bridge

    .align  4
    .global emu68_machine_resume_native
    .type   emu68_machine_resume_native, %function
emu68_machine_resume_native:
    sub     sp, sp, #16
    adrp    x9, emu68_machine_resume_frame
    ldr     x9, [x9, #:lo12:emu68_machine_resume_frame]

    adrp    x10, emu68_machine_bridge_read_value
    adrp    x11, emu68_machine_resume_value
    ldr     x12, [x11, #:lo12:emu68_machine_resume_value]
    str     x12, [x10, #:lo12:emu68_machine_bridge_read_value]
    adrp    x10, emu68_machine_bridge_outcome
    adrp    x11, emu68_machine_resume_outcome
    ldr     x12, [x11, #:lo12:emu68_machine_resume_outcome]
    str     x12, [x10, #:lo12:emu68_machine_bridge_outcome]
    adrp    x10, emu68_machine_resume_address
    ldr     x30, [x10, #:lo12:emu68_machine_resume_address]
    str     x30, [sp]

    ldp     x10, x11, [x9, #232]
    ldr     x12,      [x9, #248]
    msr     nzcv, x10
    msr     fpcr, x11
    msr     fpsr, x12

    ldp     q0,  q1,  [x9, #256]
    ldp     q2,  q3,  [x9, #288]
    ldp     q4,  q5,  [x9, #320]
    ldp     q6,  q7,  [x9, #352]
    ldp     q8,  q9,  [x9, #384]
    ldp     q10, q11, [x9, #416]
    ldp     q12, q13, [x9, #448]
    ldp     q14, q15, [x9, #480]
    ldp     q16, q17, [x9, #512]
    ldp     q18, q19, [x9, #544]
    ldp     q20, q21, [x9, #576]
    ldp     q22, q23, [x9, #608]
    ldp     q24, q25, [x9, #640]
    ldp     q26, q27, [x9, #672]
    ldp     q28, q29, [x9, #704]
    ldp     q30, q31, [x9, #736]

    ldp     x2,  x3,  [x9, #0]
    ldp     x4,  x5,  [x9, #16]
    ldp     x6,  x7,  [x9, #32]
    ldr     x8,       [x9, #48]
    ldr     x10,      [x9, #64]
    ldr     x11,      [x9, #72]
    ldr     x12,      [x9, #80]
    ldr     x13,      [x9, #88]
    ldp     x14, x15, [x9, #96]
    ldp     x16, x17, [x9, #112]
    ldp     x18, x19, [x9, #128]
    ldp     x20, x21, [x9, #144]
    ldp     x22, x23, [x9, #160]
    ldp     x24, x25, [x9, #176]
    ldp     x26, x27, [x9, #192]
    ldp     x28, x29, [x9, #208]
    ldr     x0,       [x9, #768]
    ldr     x1,       [x9, #776]
    ldr     x9,       [x9, #56]
    ldr     x30, [sp]
    add     sp, sp, #16
    br      x30

    .size emu68_machine_resume_native, .-emu68_machine_resume_native
```

