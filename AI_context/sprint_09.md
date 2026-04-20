// AI_context/sprint_09.md

# Sprint 09 — Fix ROM carregada + restaurar bellatrix.c

## Status: concluído

---

## Problema 1: ROM não carregada (initramfs ignorado)

O `#elif defined(BELLATRIX)` em `start.c` pulava direto para `bellatrix_init()`
sem processar o initramfs. A ROM é passada via initramfs pelo launcher.

**Fix**: removido o `#elif BELLATRIX` separado; BELLATRIX cai no `#else` junto
com PISTORM. Bloco PISTORM-only (`rom_copy` / `ps_read_32`) guardado com
`#ifdef PISTORM`. `bellatrix_init()` adicionado antes de `M68K_StartEmu` no
final do bloco.

---

## Problema 2: bus router furado — bellatrix.c regrediu

O `bellatrix.c` tinha sido refatorado para uma arquitetura mais modular (machine.c)
e perdeu código crítico de inicialização do sprint_06. Sintomas no boot log:

```
[BUS] unmapped write: ffffeffc = 00000000   ← stack push sem chip RAM mapeada
[BUS] unmapped read: ffffe203               ← 0xffffe203 & 0xFFFFFF = 0xffe203 (ROM alias 32-bit)
[BUS] unmapped read: 84000058               ← 0x84000058 & 0xFFFFFF = 0x000058 (vetor exceção)
```

**Causa raiz**: chip RAM (0x0-0x1FFFFF) não estava mapeada na MMU. Sem isso:
- Writes da ROM para a tabela de vetores de exceção (0x0-0x400) eram silenciados
- Stack do M68K em 0xffffeffc ia para o bus handler e retornava 0
- Endereços 32-bit alias (0x84000058, 0xffffe203) não tratados

**Fix**: restaurada a versão funcional do `bellatrix.c` (estrutura da sprint_06),
adaptada para as APIs atuais do chipset (via machine.c):

### O que foi restaurado

1. **Chip RAM mapeada**: `mmu_map(0x0, 0x0, 0x200000, ...)` — R/W
2. **Overlay**: `mmu_map(0xf80000, 0x0, 4096, READ_ONLY)` → ROM visível em 0x0 no reset
3. **AF=0 trap mappings** para CIA/custom (0x200000-0xBFFFFF, 0xC00000-0xDFFFFF, 0xF00000-0xF7FFFF)
4. **chip_ram_write()**: writes através do overlay (OVL só afeta reads)
5. **set_overlay()**: troca o mapeamento 0x0 ao receber CIA-A PRA bit 0
6. **Normalize 24-bit**: `addr &= 0x00FFFFFFu` antes do roteamento
7. **ROM diagnostic**: imprime ISP e PC da ROM antes de iniciar
8. **VBL timer**: `PAL_ChipsetTimer_Init(50, NULL)`
9. **btrace_init()** e `btrace_log()` em cada acesso
10. **CIA address decoding**: CIA-A via `A0=1`, CIA-B via `A0=0`
11. **`rom_mapped`**: definido em `bellatrix.c` (removido de `vectors.c`)

---

## Arquivos alterados

- `src/cpu/bellatrix.c` — restaurado + adaptado para machine.c APIs
- `emu68/src/aarch64/start.c` — estrutura de boot reescrita (Problema 1)
- `emu68/src/aarch64/vectors.c` — `rom_mapped` removido do bloco BELLATRIX
- `patches/0002-add-bellatrix-bus-hook.patch` — patch regenerado com diff real

Build: verde (`[100%] Built target Emu68.elf`).

---

## Próxima sessão

- Flash e boot real com o novo bellatrix.c
- Verificar no serial:
  1. `[BELA] rom_mapped=1`
  2. `[BELA] ROM @ 0xf80000: ...` com bytes corretos
  3. `[BELA] Overlay check virt[0:3]: ...` igual aos bytes da ROM
  4. KS executando a partir do PC correto (0x00F8xxxx ou 0x00FCxxxx)
  5. Ausência de `[BUS] unmapped write` para chip RAM / stack
