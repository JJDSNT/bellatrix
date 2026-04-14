// AI_context/sprint_02.md

# Sprint 02 — Fase 1: Chip RAM + ROM + Startup correto

## Status: concluído

---

## Issue 1 — Corrigir startup para BELLATRIX

### Problema encontrado
O patch 02 original tinha lacunas no `start.c`:
- `rom_copy` é variável local `#ifdef PISTORM` — referenciada no `#else` block causaria erro de compilação para BELLATRIX
- `M68K_StartEmu` lia o PC diretamente de `addr=0` (sem ler os reset vectors do ROM), que é o path para executáveis HUNK/ELF

### Solução implementada

**Patches atualizados** (`patches/0002-add-bellatrix-bus-hook.patch`):

1. `#ifndef PISTORM` → `#if !defined(PISTORM) && !defined(BELLATRIX)` (linha 1192)
   - BELLATRIX toma o path `#else` (ROM loading), não o path HUNK/ELF

2. Guarda `if (rom_copy != 0)` com `#ifdef PISTORM ... #endif`
   - `rom_copy` só existe quando `PISTORM` está definido
   - BELLATRIX não tem `ps_read_32`, então esse bloco não pode compilar

3. `M68K_StartEmu`: `#ifdef PISTORM` → `#if defined(PISTORM) || defined(BELLATRIX)` (linha 2091)
   - BELLATRIX lê reset vectors da ROM via addr=0 (exatamente como PiStorm)
   - ISP = [0x000000], PC = [0x000004] — ambos da ROM mapeada em overlay

4. `bellatrix_init()` chamado antes de `M68K_StartEmu(0, NULL)`

---

## Issue 2 — Chip RAM e overlay

### Implementação em `src/variants/bellatrix/bellatrix.c`

**`bellatrix_init()`:**
```c
// Chip RAM: 2MB R/W cached → RPi physical 0x000000
mmu_map(0x000000, 0x000000, 0x200000,
        MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);

// ROM overlay (initial state: overlay=1): primeiros 4K do Kickstart em virt 0
mmu_map(0xf80000, 0x000000, 4096,
        MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
```

**`bellatrix_bus_access()`:**
- Detecta escrita em `0xBFE001` (CIA-A PRA, bit 0 = OVL)
- overlay=1: re-mapeia virt 0 → ROM (read-only)
- overlay=0: re-mapeia virt 0 → chip RAM físico (R/W)
- `rom_mapped` definido aqui (uint32_t, valor inicial 0) para link correto

### Sequência de boot esperada
```
bellatrix_init()
  → chip RAM mapeada (0x000000–0x1FFFFF)
  → ROM shadow em 0x000000 (overlay=1)
M68K_StartEmu(0, NULL)
  → lê [0x000000] = ISP (ex: 0x00001000)
  → lê [0x000004] = PC (ex: 0xFC00D2)
  → JIT começa a executar o Kickstart
Kickstart escreve em CIA-A PRA (0xBFE001) com bit OVL=0
  → bellatrix_bus_access() re-mapeia 0 para chip RAM
Kickstart usa chip RAM normalmente
```

---

## Critério de conclusão da Fase 1

- [ ] `scripts/build.sh` compila sem erros
- [ ] Imagem roda e JIT executa as primeiras instruções do Kickstart
- [ ] UART mostra acesso a `0xBFE001` com OVL clear no btrace
- [ ] Após isso, chip RAM é acessada sem faults (sem entradas no btrace para 0x000000–0x1FFFFF)

---

## Questões em aberto para Fase 2

### config.txt para Kickstart
O Kickstart.rom deve ser passado como `initramfs` pelo bootloader RPi:
```
# config.txt
kernel=kernel8.img
initramfs kickstart.rom followkernel
```
A chave `followkernel` coloca o initramfs logo após o kernel em memória.
O device tree (`/chosen/linux,initrd-start` e `linux,initrd-end`) informa a localização.

### Slow RAM (opcional)
Arquitetura prevê Slow RAM em `0xC00000–0xD7FFFF` como buffer ARM.
Baixa prioridade para MVP — CIA e chipset têm prioridade.

---

## Próxima sessão: Fase 2 — CIAs

**Objetivo:** implementar `CIA_State` completa com:
- Timers A e B com countdown real
- ICR com mascaramento correto (read-clear)
- Port A/B com DDR
- TOD counter
- IPL gerado por CIA-A → Paula → `PAL_IPL_Set()`

**Referência:** FS-UAE `cia.cpp` — comportamento de referência obrigatório.
**Critério:** Kickstart passa detecção de hardware e PAL/NTSC sem travar.
