// AI_context/bellatrix_architecture_and_wiring.md
//
// Bellatrix – Arquitetura Consolidada e Guia de Wiring
//
// Propósito:
// Definir de forma única e coerente:
//
// 1. A arquitetura canônica do Bellatrix
// 2. O modelo de memória correto para boot do AROS
// 3. O wiring necessário para convergir o codebase
//
// Este documento substitui e unifica:
// - memory_model.md
// - decisões arquiteturais do chipset
// - instruções de migração dispersas
//
// -----------------------------------------------------------------------------
// 1. Decisão Arquitetural Fundamental
// -----------------------------------------------------------------------------
//
// O modelo correto do Bellatrix é:
//
//   → o chipset é o dono do tempo observável
//
// Consequências:
//
// - o tempo nasce do Agnus (beam, DMA, raster, etc)
// - a CPU não governa o tempo
// - o Emu68 é apenas executor de instruções
//
// Hierarquia correta:
//
//   Chipset  → define tempo observável
//   Machine  → integra componentes
//   Bus      → protocolo de transação/sincronização
//   Memory   → fonte de verdade do mapa de endereços
//
// -----------------------------------------------------------------------------
// 2. Estrutura da Máquina
// -----------------------------------------------------------------------------
//
// BellatrixMachine
//  ├── CPU (Emu68 / M68KState)
//  ├── CIA A
//  ├── CIA B
//  ├── Paula
//  ├── Agnus
//  │     ├── Copper
//  │     ├── Blitter
//  │     └── Bitplanes
//  └── Denise
//
// -----------------------------------------------------------------------------
// 3. Ownership (regra crítica)
// -----------------------------------------------------------------------------
//
// Agnus:
//   - tempo (beam)
//   - DMA
//   - Copper
//   - Blitter
//
// Paula:
//   - INTREQ / INTENA
//   - consolidação de IRQ
//
// CIA:
//   - timers / TOD / alarm
//
// Denise:
//   - estado visual
//
// Machine:
//   - integração estrutural
//   - publicação de IPL
//
// CPU:
//   - execução de instruções
//
// -----------------------------------------------------------------------------
// 4. Modelo de Memória (NOVO)
// -----------------------------------------------------------------------------
//
// A memória NÃO é mais bus-centric.
//
// O modelo correto é:
//
//   bellatrix_mem_*()
//        ↓
//   memory_map_decode()
//        ↓
//   região (chip / fast / rom / etc)
//
// Arquivos:
//
//   memory.c        → API pública
//   memory_map.c    → decode (FONTE DE VERDADE)
//   chip_ram.c      → Chip RAM
//   fast_ram.c      → Fast RAM
//   overlay.c       → overlay ROM/RAM
//   autoconfig.c    → janela vazia até existir Zorro dinâmico
//
// -----------------------------------------------------------------------------
// 5. Mapa de Memória – Phase 1 (AROS)
// -----------------------------------------------------------------------------
//
// ESSENCIAL:
//
//   0x000000–0x1FFFFF  → Chip RAM
//   0x200000–0x9FFFFF  → Fast RAM (OBRIGATÓRIO)
//   0xDFF000–0xDFFFFF  → Custom chips
//   0xF80000–0xFFFFFF  → ROM
//
// Importante:
//
// - Fast RAM é estática (não depende de AutoConfig)
// - A janela 0xE80000–0xEFFFFF fica vazia para evitar conflito
// - Isso é suficiente para AROS bootar
//
// -----------------------------------------------------------------------------
// 6. Regras de Acesso à Memória
// -----------------------------------------------------------------------------
//
// CPU:
//
//   bellatrix_mem_read*
//   bellatrix_mem_write*
//
// DMA / chipset:
//
//   bellatrix_chip_read*
//   bellatrix_chip_write*
//
// Regra crítica:
//
//   writes em 0x000000 → SEMPRE Chip RAM
//
// Overlay:
//
//   read:
//     overlay ON  → ROM
//     overlay OFF → Chip RAM
//
// -----------------------------------------------------------------------------
// 7. Copper – Modelo Final
// -----------------------------------------------------------------------------
//
// Separação:
//
//   copper.c          → execução
//   copper_service.c  → timing
//
// Regra crítica:
//
//   WAIT resolvido → executar MOVEs imediatamente
//
// Ordem obrigatória:
//
//   beam_step()
//   copper_service_poll()
//   copper_service_step()
//   bitplanes_step()
//
// Isso resolve o bug clássico:
//
//   snapshot antes do MOVE do Copper
//
// -----------------------------------------------------------------------------
// 8. Wiring Necessário (CRÍTICO)
// -----------------------------------------------------------------------------
//
// Arquivos a modificar:
//
//   src/chipset/agnus/agnus.h
//   src/chipset/agnus/agnus.c
//   src/core/machine.c
//   src/core/machine.h
//   src/cpu/bellatrix.c
//   CMakeLists.txt
//
// -----------------------------------------------------------------------------
// 8.1 agnus.h
// -----------------------------------------------------------------------------
//
// Adicionar:
//
//   #include "copper/copper.h"
//   #include "copper/copper_service.h"
//
//   CopperState copper;
//   CopperService copper_service;
//
// -----------------------------------------------------------------------------
// 8.2 agnus.c
// -----------------------------------------------------------------------------
//
// Substituir:
//
//   copper_step(...)
//
// por:
//
//   copper_service_poll(...)
//   copper_service_step(...)
//
// Garantir ordem:
//
//   beam_step(...);
//   copper_service_poll(...);
//   copper_service_step(...);
//   bitplanes_step(...);
//
// -----------------------------------------------------------------------------
// 8.3 machine.c / machine.h
// -----------------------------------------------------------------------------
//
// Deve conter:
//
//   BellatrixMemory memory;
//
// E passar:
//
//   agnus.memory = &machine.memory;
//
// -----------------------------------------------------------------------------
// 8.4 bellatrix.c (CRÍTICO)
// -----------------------------------------------------------------------------
//
// Substituir:
//
//   acessos diretos
//   decode manual
//
// por:
//
//   bellatrix_mem_read*
//   bellatrix_mem_write*
//
// IMPORTANTE:
//
//   NÃO duplicar decode de memória aqui
//
// -----------------------------------------------------------------------------
// 8.5 memory_map.c
// -----------------------------------------------------------------------------
//
// Deve conter:
//
//   if (addr >= 0x200000 && addr <= 0x9FFFFF)
//       return MEM_REGION_FAST;
//
// -----------------------------------------------------------------------------
// 8.6 MMU Mapping
// -----------------------------------------------------------------------------
//
// Em bellatrix_init:
//
//   mapear Fast RAM:
//
//   0x200000–0x9FFFFF → READ_WRITE
//
// -----------------------------------------------------------------------------
// 8.7 DMA
// -----------------------------------------------------------------------------
//
// Sempre usar:
//
//   bellatrix_chip_read*
//   bellatrix_chip_write*
//
// -----------------------------------------------------------------------------
// 8.8 CMakeLists.txt
// -----------------------------------------------------------------------------
//
// Adicionar:
//
//   copper/
//   memory/
//   autoconfig/
//
// Remover:
//
//   copper antigo
//
// -----------------------------------------------------------------------------
// 9. Fases Futuras
// -----------------------------------------------------------------------------
//
// Phase 2:
//
//   AutoConfig em 0xE80000–0xEFFFFF
//
// Phase 3:
//
//   Zorro III (>0x10000000)
//
// -----------------------------------------------------------------------------
// 10. Checklist de Conclusão
// -----------------------------------------------------------------------------
//
// ✔ Fast RAM em 0x200000–0x9FFFFF
// ✔ memory_map é fonte de verdade
// ✔ CPU usa bellatrix_mem_*
// ✔ DMA usa bellatrix_chip_*
// ✔ Copper executa antes do snapshot
// ✔ Machine contém BellatrixMemory
// ✔ Agnus contém CopperService
// ✔ bellatrix.c não faz decode manual
// ✔ MMU mapeia Fast RAM corretamente
//
// -----------------------------------------------------------------------------
// Resultado esperado:
//
//   ✔ AROS detecta Fast RAM
//   ✔ memory probing passa
//   ✔ boot avança além do stall atual
//
// -----------------------------------------------------------------------------
// END
