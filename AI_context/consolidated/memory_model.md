# Modelo de memória do Bellatrix (harness vs. Emu68)

Consolidado em 2026-07-03 após a sessão de fast RAM (ISSUE-0031).

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

**Z3 no harness:** não suportado hoje porque o backend mascara todo acesso
com `addr & 0x00FFFFFF` (barramento 24-bit). Musashi 020+ endereça 32-bit;
para Z3 bastaria não mascarar em 020+, mapear a faixa Z3 (>= 0x10000000) e
implementar autoconfig Z3. Viável, não prioritário.

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
