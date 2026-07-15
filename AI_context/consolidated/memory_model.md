# Modelo de memória do Bellatrix (harness vs. Emu68)

Consolidado em 2026-07-03 após a sessão de fast RAM (ISSUE-0031).

> **Rebaseline em andamento (2026-07-15):** a parte Z3 abaixo descreve apenas
> intenção histórica. Bellatrix ainda não possui suporte Z3 funcional. A
> arquitetura vigente está em ISSUE-0058 e o trabalho de implementação em
> ISSUE-0032: Autoconfig é externo, mas RAM/ROM/VRAM configurada deve ser
> mapeada diretamente por backend; somente páginas MMIO passam pelo serviço do
> Rigel. Este arquivo só será reconsolidado após implementação e prova.

## Harness (Musashi)

| Região | Faixa | Tamanho | Como |
|---|---|---|---|
| Chip RAM | 0x000000–0x1FFFFF | 2MB | buffer direto, DMA do chipset |
| Zorro II Fast | 0x200000–0x9FFFFF | 8MB | board autoconfig `bellatrix.fastram` (default ON, `HARNESS_FASTRAM=0` desliga) |
| Slow/Bogo | 0xC00000–0xD7FFFF | 1.5MB | `BELLATRIX_SLOW_RAM=0` desliga |
| ROM | 0xE00000 (ext) / 0xF80000 | 512KB+512KB | — |

**Regra aprendida (ISSUE-0031):** a janela do fast RAM só pode responder
**depois** que o autoconfig atribuir base ao board. Se a RAM responder no
reset, o exec a encontra na sondagem inicial e o `AddMemList` do
expansion.library duplica a faixa → "Sanity check on memory list failed".
Gate: `bellatrix_zorro2_fast_ram_configured()` (zorro2_bus.c), consultado
pelos dois backends Musashi (tools/harness e src/cpu/musashi).

**Z3 no harness:** não suportado hoje. Não basta retirar a máscara de 24 bits:
há bases conflitantes, ausência de lifecycle map/unmap e falta separar memória
direta de MMIO com efeitos colaterais. Ver ISSUE-0032.

## Emu68 (hardware)

- Chip RAM 2MB via mmu_map direto; fast RAM Z2 via
  `bellatrix_zorro2_enable_fast_ram()` (bellatrix.c, legacy path).
- RAM "grande" e RTG são território do Emu68 (Z3/32-bit + driver RTG
  próprio do Emu68) — não distorcer o harness para replicar isso.

## Presets recomendados (discussão 2026-07-03)

- **harness / KS1.3 / SD-boot:** chip 2MB + Z2 8MB (+slow opcional). É o
  que existe e boota wb20.hdf até o desktop.
- **AROS "moderno" (Z3 + RTG):** backend Emu68 em hardware.
- Evitar UAE-ismos: 16MB chip não existe em Agnus real; 1GB Z3 e 512MB RTG
  (config WinUAE do ArosOne) mascaram bugs e não são reproduzíveis aqui.
- Consequência prática: ArosOne-68K espera RTG (WinUAE usa uaegfx); no
  harness ele boota (com fast RAM) mas o desktop não aparece via Denise —
  provável necessidade de forçar screenmode nativo PAL na distro.
