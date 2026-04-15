// AI_context/sprint_06.md

# Sprint 06 — ROM loading + QEMU chipset trap fix

## Status: concluído

---

## Issue 1 — ROM loading diagnóstico

`src/cpu/bellatrix.c` → `bellatrix_init()`

Adicionado diagnóstico completo antes do overlay:
- `[BELA] rom_mapped=N` — confirma se initrd chegou
- `[BELA] ROM @ 0xf80000: xx xx ...` — bytes físicos do ROM
- `[BELA] Reset vectors: ISP=... PC=...` — valores que JIT vai usar
- `[BELA] Overlay check virt[0:3]: ...` — confirma overlay ativo

Resultado confirmado: KS 1.3 ISP=0x11114EF9, PC=0x00FC00D2. Ambos corretos —
ISP é o ROM header OCS, KS substitui SP na primeira instrução (LEA $40000, A7).

Overlay check usou inline asm (`mov x9, #0; ldr w0, [x9]`) para evitar o
GCC 11 UB trap que gerava `BRK #0x3e8` em null pointer literal.

---

## Issue 2 — enable_cache obrigatório para KS/bare-metal

`run.sh`: `-append "${BOOTARGS:-enable_cache}"`

Sem `enable_cache`, CACR=0 e o execution loop usa label `2f` (slow path com
SaveContext + VerifyUnit(CRC32) por iteração). Com `enable_cache`, CACR=0x80008000
e o fast path (label 99, hash lookup) é usado. Essencial para KS bare-metal.

Também adicionado `-accel tcg,tb-size=64` conforme docs oficiais do Emu68.

---

## Issue 3 — QEMU TCG não injeta Translation Faults para guest RAM

**Root cause**: QEMU TCG com AArch64 BE não injeta exception para translation
faults quando o endereço físico existe dentro da RAM do guest (0-862MB). Endereços
como $BFE201 (CIA-A), $DFF09A (Agnus), $F00000 (ROM check) eram acessados
diretamente na RAM física do QEMU sem gerar data abort.

QEMU SI injeta Access Flag Faults e Permission Faults para páginas que existem
em TTBR0 com AF=0 ou sem permissão de escrita.

**Fix**: mapear regiões chipset/CIA/expansão como read-only SEM MMU_ACCESS (AF=0).
Qualquer acesso (leitura ou escrita) gera Access Flag Fault → SYSPageFaultHandler
→ bellatrix_bus_access → btrace.

```c
// Chipset trap mappings em bellatrix_init():
mmu_map(0x200000, 0x200000, 0xA00000,   // 0x200000-0xBFFFFF (CIA, slow RAM)
        MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
mmu_map(0xC00000, 0xC00000, 0x200000,   // 0xC00000-0xDFFFFF (custom chips)
        MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
mmu_map(0xF00000, 0xF00000, 0x80000,    // 0xF00000-0xF7FFFF (expansion ROM check)
        MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
```

NOTA: Esta workaround é específica para QEMU. Em hardware real, translation faults
funcionam normalmente. O mapeamento adicional não prejudica o hardware real.

---

## Resultado final — KS 1.3 rodando

btrace confirma execução completa até pedido de VBL:

```
R $F00000    → ROM check (0 = sem ROM ext) ✓
W $BFE201=3  → CIA-A DDRA outputs ✓
W $BFE001=2  → OVL=0 (overlay off) ✓
W $DFF09A=7FFF → INTENA disable all ✓
W $DFF09C=7FFF → INTREQ clear ✓
W $DFF096=7FFF → DMACON disable ✓
W $DFF180=$444 → COLOR00 = gray (KS coloring screen!) ✓
... memory tests at $C3F09A (slow RAM detect) ...
W $DFF09A=$8004 → INTENA: master + PORTS ✓
W $DFF096=$8200 → DMACON master enable ✓
W $DFF09A=$C000 → INTENA: master + VERTB ← KS pedindo VBL!
```

KS agora aguarda VBL a 50Hz para avançar. FIQ/IRQ handler já implementado
(Phase 3). Próxima sessão: verificar se VBL está chegando ao KS e o que acontece
após os primeiros ticks.

---

## Próxima sessão

1. Verificar log completo — o KS deve entrar em idle loop esperando VBL
2. Com VBL chegando (FIQ a 50Hz), INTREQ[VERTB] é setado → IPL 3
3. KS deve executar o VBL interrupt handler → progress
4. Endereço 0xE1031FD0 suspeito no btrace — investigar (pode ser acesso legítimo
   com bits altos setados por aritmética interna do KS)
