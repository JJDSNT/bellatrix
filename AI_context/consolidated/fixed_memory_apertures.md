# Auditoria das aperturas fixas de memória

Auditado em 2026-07-15 contra o caminho fault-driven do Emu68, os dois
backends Musashi e o mapa comum do Bellatrix. Estas faixas são semântica da
máquina Amiga; não devem ser convertidas em alocações dinâmicas.

## Matriz vigente

| Categoria | Abertura guest | Backing/owner | Emu68 | Musashi |
|---|---|---|---|---|
| Chip RAM | `$000000-$0fffff` | backing compartilhado de 1 MiB, DMA-visible | MMU direta | buffer direto |
| mirror Chip | `$100000-$1fffff` | alias do mesmo backing; não adiciona RAM | MMU phys `0` na segunda janela | endereço mascarado pelo backing |
| overlay | leituras `$000000-$07ffff` | ROM enquanto OVL=1; Chip quando OVL=0 | remapeamento MMU | seleção no callback |
| ROM estendida | `$e00000-$e7ffff` quando presente | primeira metade de ROM de 1 MiB | MMU direta RO | buffer direto RO |
| Autoconfig | `$e80000-$e8ffff` | sequenciador Z2/Z3 | Data Abort + `vectors.c` | chamada explícita |
| ROM padrão | `$f80000-$ffffff` | Kickstart/segunda metade de ROM de 1 MiB | MMU direta RO | buffer direto RO |
| CIA-B | endereços pares em `$bfd000-$bfdf00` e `$bfe000-$bfef00` | Rigel CIA-B | páginas BFD/BFE fault-driven | chamada explícita |
| CIA-A | endereços ímpares em `$bfe001-$bfef01` | Rigel CIA-A | página BFE fault-driven | chamada explícita |
| custom | `$dff000-$dfffff` | Rigel custom registers | Data Abort + `vectors.c` | chamada explícita |

As páginas CIA são protegidas inteiras no Emu68 porque a MMU trabalha em
páginas de 4 KiB. Isso não torna todos os bytes dessas páginas registradores:
o decoder esparso acima continua definindo owner; holes retornam open bus.

## Regras consolidadas

- O backing físico de Chip RAM tem 1 MiB. A janela CPU tem 2 MiB porque A20 é
  ignorado; a metade superior é mirror coerente com o DMA.
- Overlay modifica somente a fonte das leituras. Escritas baixas continuam no
  backing de Chip RAM, inclusive com OVL ativo.
- Para ROM de 1 MiB, o overlay baixo usa a metade estendida de `$e00000`, como
  o remapeamento Emu68. `$f80000` continua sendo a abertura ROM padrão.
- ROM é direta e read-only; CIA, custom e Autoconfig têm efeitos colaterais e
  permanecem no ponto de integração fault/vectors.
- `$e90000-$efffff` não pertence ao Autoconfig. Uma board só ganha outra
  janela depois que seu `map()` aceita a base atribuída.
- O endereço fixo de uma abertura não implica backing fixo nem board fechada.
  Autoconfig permanece fixo; as janelas das expansões permanecem dinâmicas.

## Divergências corrigidas nesta auditoria

1. Musashi e o mapa comum agora reproduzem o mirror de Chip RAM já instalado
   pelo Emu68 em `$100000-$1fffff`.
2. Escritas Musashi sob overlay deixaram de ser descartadas e atingem o backing
   de Chip RAM, como o contrato comum e o fault path Emu68.
3. O overlay de ROM de 1 MiB passou a usar a metade `$e00000` também nos
   caminhos Musashi.
4. A constante Autoconfig foi corrigida de `$e80000-$efffff` para os 64 KiB
   reais `$e80000-$e8ffff`.
5. CIA-A/CIA-B deixaram de ser classificadas como dois blocos densos; os
   predicados comuns preservam paridade, aliases e holes.

## Limites desta conclusão

- `memory_map_decode()` descreve somente o espaço Amiga baixo normalizado para
  24 bits. Regiões Z3 continuam no lifecycle próprio e não devem ser inferidas
  desse decoder.
- `$f00000-$f7ffff` permanece uma janela neutra de probe de expansion ROM no
  perfil atual; não foi promovida a ROM padrão.
- Slow/Bogo RAM e RTC não foram redefinidos por esta auditoria.
- Nenhuma validação em hardware real foi executada.
