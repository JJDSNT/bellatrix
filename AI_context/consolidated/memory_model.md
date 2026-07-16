# Modelo de memória do Bellatrix (harness vs. Emu68)

Auditoria das faixas arquiteturais fixas:
[`fixed_memory_apertures.md`](fixed_memory_apertures.md).

Consolidado em 2026-07-03 após a sessão de fast RAM (ISSUE-0031).

> **Rebaseline implementado em 2026-07-15:** o backing ARM/host da Fast RAM é
> reservado antes do guest, mas não possui endereço 68k nessa etapa. A escrita
> Autoconfig chama `map()` na board Z2 e só então instala a janela direta no
> backend selecionado. Não há mais guest base fixa no memory map nem
> pré-mapeamento de `$00200000`; o fault handler original do Emu68 não foi
> alterado.

## Harness (Musashi)

| Região | Faixa | Tamanho | Como |
|---|---|---|---|
| Chip RAM | backing 0x000000–0x0FFFFF; mirror CPU até 0x1FFFFF | 1MB fitted | buffer direto e DMA do chipset; mirror não adiciona RAM |
| Zorro II Fast | base atribuída pelo guest | 8MB (ou 4MB com RTG) | board autoconfig `bellatrix.fastram` (default ON, `HARNESS_FASTRAM=0` desliga) |
| Slow/Bogo | 0xC00000–0xD7FFFF | 1.5MB | `BELLATRIX_SLOW_RAM=0` desliga |
| ROM | 0xE00000 (ext) / 0xF80000 | 512KB+512KB | — |

**Regra aprendida (ISSUE-0031):** a janela do fast RAM só pode responder
**depois** que o autoconfig atribuir base ao board. Se a RAM responder no
reset, o exec a encontra na sondagem inicial e o `AddMemList` do
expansion.library duplica a faixa → "Sanity check on memory list failed".
O gate agora é estrutural: antes da atribuição não existe região direta; depois
dela a board publica exatamente `[base, base + size)`. No reset o `unmap()`
remove a região. O Musashi consulta a tabela esparsa; o Emu68 usa a MMU e não
paga lookup por acesso.

**Z3 no harness:** não suportado hoje. Não basta retirar a máscara de 24 bits:
há bases conflitantes, ausência de lifecycle map/unmap e falta separar memória
direta de MMIO com efeitos colaterais. Ver ISSUE-0032.

## Emu68 (hardware)

- Chip RAM continua com seus aliases próprios; isso é semântica da máquina e
  não faz parte do lifecycle da expansão.
- Fast RAM Z2 é registrada por `z2_fast_ram.c`. A callback recebe a base do
  guest e associa o backing ARM a ela via `cpu_backend_map_direct()`.
- No Emu68, `direct_region.c` é apenas plano de controle que instala/remove
  páginas MMU; cargas e stores normais não o consultam. No Musashi, a mesma
  região é localizada pelos callbacks de memória.
- O endereço baixo `0x00200000` que ainda aparece em `bellatrix.c` identifica
  o **backing físico/virtual ARM reservado**, não uma base guest. Esses dois
  espaços não devem voltar a ser confundidos.

## Contrato fechado da Fast RAM Z2

1. Reservar backing não torna RAM visível ao 68k.
2. A board responde apenas em `$E80000` enquanto está pendente.
3. A escrita de base chama `map(base, size)` antes de publicar `configured`.
4. Falha no backend mantém a board pendente e a janela invisível.
5. Depois do sucesso, somente a faixa atribuída responde.
6. Reset, substituição ou remoção da board chama `unmap()`.

Esse contrato segue `emu68/src/boards/z2ram.c`: RAM direta elimina a passagem
pelo fault handler. Bellatrix acrescenta a separação explícita entre backing e
base guest para que Emu68 e Musashi reproduzam a mesma board.

Provas locais em 2026-07-15: testes unitários cobrem invisibilidade anterior à
atribuição, base não usual (`$00400000`), acesso big-endian, rollback de falha e
`unmap()` no reset. Builds bare-metal Emu68 fault-driven e Musashi/68040, além
do harness POSIX, completaram. Não houve validação em hardware real.

## O que permanece deliberadamente fixo

- Chip RAM e seu mirror dependem do modelo de Agnus.
- ROM, extended ROM, CIA, custom registers e a janela Autoconfig são aperturas
  arquiteturais do Amiga, não alocações de RAM expansion.
- O backing ARM pode ter endereço reservado fixo; somente seu endereço guest é
  escolhido pelo Autoconfig.

## Presets recomendados (discussão 2026-07-03)

- **harness / KS1.3 / SD-boot:** chip 2MB + Z2 8MB (+slow opcional). É o
  que existe e boota wb20.hdf até o desktop.
- **AROS "moderno" (Z3 + RTG):** backend Emu68 em hardware.
- Evitar UAE-ismos: 16MB chip não existe em Agnus real; 1GB Z3 e 512MB RTG
  (config WinUAE do ArosOne) mascaram bugs e não são reproduzíveis aqui.
- Consequência prática: ArosOne-68K espera RTG (WinUAE usa uaegfx); no
  harness ele boota (com fast RAM) mas o desktop não aparece via Denise —
  provável necessidade de forçar screenmode nativo PAL na distro.
