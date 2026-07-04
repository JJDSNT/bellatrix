---
id: ISSUE-0036
title: "Regressão bare-metal: console serial (mini-UART) silencioso/lixo na Pi 3B física"
status: done
priority: high
type: bug
owner: agent
created_at: 2026-07-04
updated_at: 2026-07-04
tags:
  - baremetal
  - regression
  - serial
  - bisect
  - raspberry-pi
  - tlsf
  - heap-corruption
related_files:
  - src/io/bluetooth/bt_host.c
  - src/host/raspi3/console_log.c
  - src/host/raspi3/vc_mailbox.c
  - src/host/raspi3/vc_mailbox.h
  - emu68/src/tlsf.c
  - emu68/include/tlsf.h
  - emu68/src/aarch64/start.c
  - patches/0019-emu68-tlsf-hardening.patch
---

# RESOLVIDO (04/07): mini-UART estável no hardware

Causa raiz final: havia dois problemas sobrepostos no caminho do console:

1. O console fazia handoff PL011 → mini-UART durante o boot. Isso era frágil
   por construção e foi substituído por console mini-UART nativo desde
   `setup_serial()` em builds `BELLATRIX`.
2. Depois que o console inicial já estava correto, a bridge Paula reabria a
   mesma AUX mini-UART via `uart_host_open_miniuart()`, que chamava
   `miniuart_backend_open()` e usava o clock default legado de 250 MHz. No
   hardware testado o core clock real é 400 MHz; essa reabertura sobrescrevia
   o divisor correto (`433`) e corrompia a saída a partir da seleção do backend
   UART.

Fix final:

- `setup_serial()` em `BELLATRIX` inicializa o console mini-UART cedo.
- `config.txt` fixa `core_freq=400` junto com `enable_uart=1`.
- `uart_host_open_miniuart_clk(host, baud, clk_hz)` permite abrir a bridge
  Paula com o clock correto.
- Bellatrix usa `uart_host_open_miniuart_clk(&m->uart_host, 115200, 400000000u)`.

Validado pelo usuário em Pi 3B física: console limpo após o ponto onde antes
corrompia. Instrumentação diagnóstica foi removida do código final.

# ATUALIZAÇÃO (04/07): refatoração em andamento — mini-UART como console nativo desde `setup_serial()`

Decisão arquitetural: parar de tratar o problema como ajuste de timing no
handoff PL011 → mini-UART. O modelo anterior dependia de o Emu68 inicializar
PL011 nos GPIO14/15, imprimir parte do boot, e Bellatrix remuxar esses mesmos
pinos para ALT5 em algum ponto posterior. Isso é estruturalmente frágil:
qualquer byte ainda em voo no PL011, `kprintf` concorrente, baud/clock
divergente ou reabertura posterior da mini-UART pode corromper o console.

Novo plano implementado no worktree:

- Em build `BELLATRIX`, `setup_serial()` do Emu68 não configura mais PL011 nos
  pinos do header. Ele chama `bellatrix_console_log_init_early(get_clock_rate(4))`.
- `bellatrix_console_log_init_early()` configura GPIO14/15 para mini-UART,
  abre AUX mini-UART em 115200 e instala o `kprintf_set_putc_override()` antes
  do primeiro `kprintf`.
- Enquanto Paula ainda não compartilha a mini-UART, `kprintf` escreve direto
  no hardware (com polling limitado). Assim os logs iniciais aparecem sem
  depender de `console_log_drain()`.
- Quando `bellatrix_init()` abre a mini-UART como bridge de Paula,
  `console_log_set_deferred()` muda `kprintf` para o ring oportunístico; a
  drenagem continua depois da TX FIFO de Paula em `machine_step_host_serial_rigel()`.
- A chamada tardia `console_log_init()` em `emu68/src/aarch64/start.c` está
  sendo removida. Não há mais handoff PL011 → mini-UART durante o boot.

Verificação local:

- `./scripts/setup.sh --verify` passou com todos os patches, incluindo a
  alteração em `patches/0008-bellatrix-console-redirect.patch`.
- Build passou com:
  `BELLATRIX_CPU_BACKEND=musashi BELLATRIX_BTSTACK=0 BELLATRIX_USBSTACK=1 ./scripts/build.sh`.
- Imagem gerada:
  `emu68/install-bellatrix-rigel-musashi/Emu68.img`.

Teste em hardware do usuário:

- O console agora sai limpo desde o primeiro `[BOOT]` até
  `[USB] DWC2 low-level init...`, confirmando que a remoção do handoff
  PL011 → mini-UART resolveu a fase inicial.
- A partir do init USB o terminal passa a mostrar lixo, com algumas linhas
  parcialmente legíveis depois. Isso é consistente com mudança de clock
  core/VPU ou divisor da mini-UART ficando stale, não com concorrência de
  `kprintf` entre cores.

Mitigação adicionada em seguida:

- `config.txt` agora fixa explicitamente `core_freq=400` além de
  `enable_uart=1`; o log mostrou `CORE Clock at 400 MHz`, então este valor
  preserva o clock observado e evita DVFS no domínio usado pelo mini-UART.
- `bellatrix_console_log_reclock(core_hz)` reprograma só o divisor de baud
  da mini-UART depois de `core_io_init()`, caso USB/firmware tenha mudado o
  clock após `setup_serial()`.
- `s_direct_mode` passou a usar `__atomic_load/store`; o Emu68 já serializa
  `kprintf()`/`vkprintf()` com `print_lock`, então chamadas concorrentes de
  cores diferentes não intercalam bytes no override.

Resultado do usuário: o problema persiste mesmo com `core_freq=400` e reclock
pós-`core_io_init()`. Próximo teste de isolamento: build equivalente com
`BELLATRIX_USBSTACK=0` para separar "USB muda algum clock/estado MMIO que
afeta mini-UART" de "corrupção aparece apenas quando o volume de logs/runtime
avança".

Build de isolamento gerado:

```
BELLATRIX_CPU_BACKEND=musashi BELLATRIX_BTSTACK=0 BELLATRIX_USBSTACK=0 ./scripts/build.sh
```

Imagem atual em `emu68/install-bellatrix-rigel-musashi/Emu68.img` foi
sobrescrita por essa variante sem USB.

Resultado do usuário: também corrompeu com USB desligado, agora logo após
`[BELA] Overlay check...`. Isso descarta USB/DWC2 como causa primária e
mostra que a corrupção ocorre ainda em modo direto, antes de Paula/ring.

Nova ação: adicionar `bellatrix_console_log_reclock(get_clock_rate(4))`
diretamente em `platform_post_init()`, logo depois de o Emu68 mudar/logar os
clocks ARM/core. A chamada anterior pós-`core_io_init()` era tarde demais
para cobrir todos os logs Bellatrix se o divisor inicial foi calculado antes
de `set_clock_rate(3)` estabilizar o domínio core/VPU.

Resultado do usuário: ainda corrompe. Próximo diagnóstico aplicado: no modo
direto inicial, `console_log_putc()` agora espera ~100us após cada byte
escrito. Em 115200 8N1 um caractere leva ~87us no fio, então isso elimina
qualquer dependência do bit TX-ready/FIFO da AUX mini-UART durante o boot
direto. Se isso limpar a saída, o bug é pacing/FIFO/status; se não limpar,
o problema é baud/clock/pino ou corrupção de dados antes de chegar no putc.

Resultado do usuário: continuou corrompendo. Isso descarta também pacing
simples/FIFO no modo direto. Próximo diagnóstico: eliminar a mailbox como
variável e programar o divisor assumindo `core_freq=400` fixo em todos os
pontos (`setup_serial()`, `platform_post_init()` e pós-`core_io_init()`).
O log já mostrou `CORE Clock at 400 MHz`; se a corrupção sumir, a leitura
ou timing da mailbox era enganosa. Se persistir, a causa deve estar em
pino/clock efetivo da AUX, divisor/fórmula ou outra escrita no hardware.

Pedido do usuário: instrumentar para identificar exatamente onde a corrupção
começa. Instrumentação adicionada:

- `miniuart_backend_read_baud()` e `miniuart_backend_read_cntl()`.
- `bellatrix_serial_diag(tag)`, imprimindo `core`, `baud`, `lsr`, `cntl`.
- Marcadores em `bellatrix_init()`:
  `after-overlay`, `before PAL_Runtime_Init`, `after PAL_Runtime_Init`,
  `after-pal`, `before uart backend select`, `after-uart-open`.

Resultado da instrumentação:

```
[MU:after-overlay] core=400000000 baud=433 lsr=00000060 cntl=00000003
[MU:after-pal]     core=400000000 baud=433 lsr=00000060 cntl=00000003
[PHASE] before uart backend select
<corrupção>
```

Causa isolada: a corrupção começa quando a bridge Paula seleciona o backend
mini-UART. `uart_host_open_miniuart()` chamava `miniuart_backend_open()`, que
usa o default legado de 250 MHz (`SYS_CLK_HZ`) e sobrescreve o divisor correto
433 com divisor de 250 MHz. Como o core real está em 400 MHz, o baud efetivo
fica errado a partir dali.

Fix aplicado: adicionar `uart_host_open_miniuart_clk(host, baud, clk_hz)` e
usar `400000000u` no caminho Bellatrix/Paula, preservando o mesmo divisor do
console inicial.

Premissas técnicas:

- A mini-UART depende do VPU/core clock; `enable_uart=1`/clock fixo continua
  obrigatório para baud estável.
- PL011 fica reservado para Bluetooth desde o começo, inclusive quando
  `BELLATRIX_ENABLE_BTSTACK=0`, evitando comportamento diferente por build.

# ATUALIZAÇÃO (04/07, continuação #2): bisect de ontem invalidado — refeito só com commits "limpos"

**Descoberta crítica**: o build que eu disse ser "`0a0ee59`, o último que funciona" (reconstruído hoje via
`git worktree` + patches aplicados manualmente à mão) **não funciona** — mesmo sintoma do HEAD atual
(nada no serial até o launcher, lixo depois). O usuário testou seus **arquivos reais salvos** de builds
antigas (08/06 e 15/06) e confirmou que **esses sim funcionam de verdade**.

Conclusão: o bisect de ontem usou reconstrução manual de patches (`git apply` + edições à mão em
`start.c` + cópia de arquivos de um commit futuro) em vários pontos — introduzindo divergências em
relação ao histórico real. **Os resultados "✅ funciona" da tabela de bisect antiga (mais abaixo, seção
"CAUSA RAIZ ISOLADA POR BISECT") não são confiáveis** para os commits que precisaram de reconstrução
manual. Só é confiável um commit onde `./scripts/setup.sh` (sem NENHUMA edição manual, só inicializar
submódulos que a própria versão daquele `setup.sh` esquecia — isso é gap de tooling da época, não
reconstrução) aplica **100% limpo**.

## Tabela de commits "limpos" (setup.sh aplica sem edição manual), em ordem de data

| Commit | Data/hora | `setup.sh` limpo? | Testado em hardware |
|---|---|---|---|
| `0e6e4e9` | 2026-06-15 08:03:05 | ✅ limpo | ✅ **CONFIRMADO funciona** (rebuild de hoje, 100% limpo) |
| `a2653c7` | 2026-06-18 00:39:30 | ✅ limpo | 🔄 build em andamento |
| `768784f` | 2026-06-18 00:54:16 | ❌ **suja** (0007 não aplica) | não confiável, pular |
| `0a0ee59` | 2026-06-18 03:09:22 | ❌ **suja** (0007 não aplica) | não confiável — foi essa reconstrução manual que gerou a conclusão errada de ontem |
| `1415619` | 2026-06-18 03:13:54 | ✅ limpo | pendente (é o suspeito original) |
| `2c96245` | 2026-06-18 03:15:53 | ✅ limpo | pendente |
| `38afddd` | 2026-06-21 05:39:27 | ✅ limpo | pendente |
| `8d15e88` | 2026-06-28 23:47:29 | ✅ limpo | pendente |
| `57314f7` | 2026-07-03 22:51:32 | ✅ limpo | ❌ falha (HEAD antes desta sessão) |

Nota: `768784f` e `0a0ee59` são os únicos dois pontos "sujos" nessa janela — caem bem no meio da
transição (18/06, entre 00:54 e 03:09) onde `patches/0007-bellatrix-boot-sequence.patch` foi
regenerado fora de sincronia com o commit da superproject. Não são testáveis sem reconstrução manual
(risco já demonstrado), então ficam fora do bisect até serem regenerados corretamente (não é prioridade
agora).

## Plano: bisectar só entre os pontos limpos

**Descoberta adicional**: `a2653c7` e o próprio `1415619` têm uma janela real (~2-3 min) no histório
da superproject onde `cmake/bellatrix-variant.cmake` já referencia `src/cpu/emu68/bellatrix_profile.c`
antes desse arquivo existir de fato (só chega em `2c96245`) — **não é problema de patch, é um commit
genuinamente quebrado no histórico** (sequência rápida de commits sem build individual). Não dá pra
buildar `a2653c7` nem `1415619` isoladamente sem adicionar esse arquivo, o que seria reconstrução
manual de novo. Então: o primeiro ponto **limpo E buildável** depois do suspeito é `2c96245` (2 min
depois de `1415619`, funcionalmente equivalente pra fins de teste do serial).

**Build limpo de `2c96245` pronto**, 100% via `setup.sh` + inicialização de submódulos ausentes
(nenhuma edição de arquivo-fonte):
```
/tmp/claude-1000/-home-jaime-bellatrix/6deb1c25-0455-4973-b68b-427f5eee3a34/scratchpad/check-2c96245/emu68/install-bellatrix-rigel-musashi/
```
Flags: `BELLATRIX_CPU_BACKEND=musashi BELLATRIX_BTSTACK=0 BELLATRIX_USBSTACK=1`.

**Testado: `2c96245` FALHA.** Janela real agora: `0e6e4e9` (15/06, funciona) → `2c96245` (18/06 03:15,
falha). São só 8 commits nessa janela:

```
a2653c7 → 768784f → 8e84be7 → 0a0ee59 → 40adee1 → c84e196 → 1415619 → 206a641 → 2c96245(falha)
```

Todos exceto `768784f`/`0a0ee59` (que falham no `setup.sh` por causa do `0007`, mesmo problema de
sempre) têm a mesma janela quebrada do `bellatrix_profile.c` ausente (arquivo de profiling/MMIO trace,
**não relacionado ao serial**). Diferente da reconstrução de ontem: aqui só copio o arquivo inteiro e
inerte, verbatim de `2c96245`, sem editar nenhuma linha de lógica — risco bem menor, mas registrado
aqui por transparência.

**Build de `c84e196` (ponto médio da janela) pronto**, com `bellatrix_profile.c/.h` +
`src/runtime/cpu_progress.h` copiados verbatim de `2c96245` (md5 do arquivo:
`48780cf814f1dee8f537bf0b642c2474`):
```
/tmp/claude-1000/-home-jaime-bellatrix/6deb1c25-0455-4973-b68b-427f5eee3a34/scratchpad/check-c84e196/emu68/install-bellatrix-rigel-musashi/
```
**Testado: `c84e196` FALHA.** Janela agora: `0e6e4e9` (funciona) → `c84e196` (falha), 5 candidatos:
`a2653c7`, `768784f`(sujo, 0007), `8e84be7`(sujo, 0007), `0a0ee59`(sujo, 0007), `40adee1`.

**Build de `40adee1` pronto** (limpo, mesmo arquivo `bellatrix_profile.c` inerte copiado de `2c96245`,
md5 idêntico ao de antes):
```
/tmp/claude-1000/-home-jaime-bellatrix/6deb1c25-0455-4973-b68b-427f5eee3a34/scratchpad/check-40adee1/emu68/install-bellatrix-rigel-musashi/
```
**Testado: `40adee1` FALHA.** Janela agora: `0e6e4e9` (funciona) → `40adee1` (falha). Único candidato
limpo restante nessa janela: `a2653c7` (o cluster sujo `768784f`/`8e84be7`/`0a0ee59` fica sanduichado
entre `a2653c7` e `40adee1`, ainda sem forma limpa de testar).

**Build de `a2653c7` pronto** (mesmo tratamento do arquivo inerte):
```
/tmp/claude-1000/-home-jaime-bellatrix/6deb1c25-0455-4973-b68b-427f5eee3a34/scratchpad/check-a2653c7/emu68/install-bellatrix-rigel-musashi/
```
**Pendente**: testar no hardware. Se funcionar → a fronteira real está dentro do cluster sujo
(`768784f`→`8e84be7`→`0a0ee59`), e vamos precisar de uma estratégia pra testar esses três apesar do
`0007` não aplicar (ex.: regenerar o patch consultando o diff real do commit, não reconstrução à mão
de hunks). Se falhar → o problema já estava presente entre `0e6e4e9` (15/06) e `a2653c7` (18/06
00:39) — janela bem maior, precisa listar os commits desse intervalo.

---

# ATUALIZAÇÃO (sessão de 04/07, continuação): causa raiz provável encontrada — TLSF desatualizado

O paradoxo descrito abaixo ("código morto cuja remoção quebra o boot") tem uma
explicação plausível e **verificável estaticamente** (sem precisar do
hardware): nosso `emu68/` está pinado em `305f686` ("Merge pull request #310
from michalsc/1.0.7"), que está **atrás** de uma série de commits upstream
(`michalsc/Emu68`) que corrigem bugs reais no alocador TLSF
(`emu68/src/tlsf.c`) — o heap usado desde o início de `boot()`
(`tlsf_init_with_memory(&__bootstrap_end, pool_size)`,
`emu68/src/aarch64/start.c:589`), muito antes de qualquer código do
Bellatrix/BT rodar:

```
c1a08ef  tlsf fixed and reworked a tiny bit. should be much more stable now   (2025-11-06)
89e3c79  decrease alignment restriction of TLSF                               (2025-11-15)
1d36638  although fixed, keep some debug in TLSF in case it misbehaves        (2025-11-16)
7cc66f3  TLSF hardened against potential pitfalls                            (2025-12-25)
9627fd4  tlsf coding style compliant now                                     (2025-12-25)
4f0a44c  add tlsf flag field, add MULTITHREADING flag...                     (2026-02-01)
476129e  don't initialize TLSF if NULL is given as memory start              (2026-02-15)
```
(pulei `2cad646`, que só adiciona operadores C++ `new`/`delete` — irrelevante
pro nosso código C.)

**O bug concreto** (visível no diff de `c1a08ef`): em `tlsf_malloc_aligned()`,
a versão antiga escreve um header de bloco livre na folga de alinhamento
(`diff_begin`) sempre que `diff_begin > 0` — mas se `diff_begin` for maior que
zero e **menor que `ROUNDUP(sizeof(hdr_t))`** (o tamanho mínimo de um header
válido), esse header não cabe e corrompe memória adjacente. O fix upstream
troca a condição para `diff_begin >= ROUNDUP(sizeof(hdr_t))`.

Isso é **sensível ao endereço exato** de início do pool do heap
(`&__bootstrap_end`), que se desloca conforme o `.bss` do binário muda de
tamanho. O commit `1415619` removeu ~32KB de arrays estáticos
(`s_console_ring[32768]` + variáveis pequenas) de `bt_host.c` — encolhendo o
`.bss` e deslocando `__bootstrap_end` (mesmo que o código removido nunca
rodasse com `BELLATRIX_BTSTACK=0`). Esse deslocamento de poucos bytes pode
empurrar `diff_begin`, em alguma alocação alinhada feita em algum lugar do
boot, de "0 ou ≥header" pra "positivo e pequeno demais" — corrompendo algo
que antes não corrompia. **Isso explica o paradoxo**: a regressão não é sobre
o código do BT em si, é um efeito colateral no layout de memória que expôs um
bug de heap já existente e latente.

## Fix aplicado (commitado nesta sessão)

Em vez de re-pinar `emu68/` para o HEAD do upstream (619 commits à frente,
inclui a branch `1_1_wip` com trabalho experimental/PPC inacabado — risco
alto demais), portei só o `src/tlsf.c`/`include/tlsf.h` corrigidos (versão
final em `f5e5680`, upstream atual) via um patch novo e independente:
**`patches/0019-emu68-tlsf-hardening.patch`**. Também adicionei duas chamadas
`tlsf_set_flags(tlsf, TLSF_MULTITHREADING)` (uma pro heap principal, uma pro
`jit_tlsf`) em `patches/0007-bellatrix-boot-sequence.patch`, porque a versão
nova do TLSF só usa spinlock quando essa flag está setada — sem isso,
perderíamos a proteção de concorrência que o código antigo sempre tinha
(Bellatrix é multicore).

Verificação feita (sem hardware):
- Os 8 patches de `emu68/` (`0001,0002,0003,0007,0008,0009,0010,0019`)
  aplicam limpo em sequência a partir de um checkout limpo de `305f686`
  (testado via `git worktree` + `git apply --check`, depois comparado
  byte-a-byte com o working tree atual — idêntico).
- `./scripts/setup.sh --verify` passa para todos os patches.
- Build completo `BELLATRIX_CPU_BACKEND=musashi BELLATRIX_BTSTACK=1
  BELLATRIX_USBSTACK=1 ./scripts/build.sh` compila e linka sem erros.

**Testado na Pi física (04/07): o fix do TLSF sozinho NÃO resolveu.** Sintoma
seguiu presente (usuário não conseguiu confirmar se mudou exatamente, testou
com a config padrão da TUI: musashi, `BTSTACK=0`, `USBSTACK=1`, single-core).
O fix do TLSF continua válido/aplicado (é uma correção real e verificada de
bug upstream), mas não é suficiente sozinho — ou não é a causa principal.

## Segundo bug real encontrado: FIFO overrun na AUX mini-UART

Lendo `src/host/raspi3/miniuart_backend.c:93` com atenção:
`miniuart_backend_write_byte()` escrevia **incondicionalmente** no registrador
`AUX_MU_IO`, sem checar o bit de "FIFO tem espaço" (LSR bit 5, `0x20`). O
comentário original assumia "the TX FIFO is deep enough... at the rates we
call this function (one byte per machine_step_components)" — mas
`console_log_drain()` (`src/host/raspi3/console_log.c:41`) chama essa função
em loop, até `CONSOLE_LOG_DRAIN_MAX=256` vezes numa única chamada, sem
nenhum espaçamento. O FIFO real da AUX mini-UART tem só **8 bytes** de
profundidade — o banner de boot inteiro (múltiplas linhas de `kprintf`,
acumuladas no ring buffer antes do primeiro `console_log_drain()`) estoura
isso de longe, corrompendo/derrubando bytes no hardware real. Bate
exatamente com o sintoma relatado ("lixo, depois para").

Fix aplicado (`src/host/raspi3/miniuart_backend.c`): espera (com limite de
~4000 iterações, pra não travar o QEMU — cujo bit de LSR TX-ready não é
confiável, conforme o comentário original já observava) o bit de FIFO-livre
antes de cada escrita, em vez de escrever sempre.

Build verificado com as flags exatas que o usuário testou
(`BELLATRIX_CPU_BACKEND=musashi BELLATRIX_BTSTACK=0 BELLATRIX_USBSTACK=1`) —
compila limpo. **Ainda não testado no hardware.**

**Testado na Pi física (04/07): ainda não funcionou, e apareceu sintoma
novo** — depois do launcher tentar iniciar o emulador, o serial começou a
jogar lixo continuamente, e o launcher passou a se comportar como se não
houvesse pendrive USB conectado.

## Regressão auto-introduzida encontrada e corrigida: spin-wait bloqueante

O primeiro fix do FIFO overrun (acima) usava um spin-wait de até 4000
iterações por byte antes de escrever, esperando o bit de FIFO-livre (LSR bit
5). Como `console_log_drain()` chama isso até 256 vezes numa única passada,
o pior caso é ~1 milhão de iterações bloqueando **antes de retornar** — isso
atrasa qualquer outra coisa que divida o mesmo step loop, inclusive o
polling de USB/launcher. Isso explica o sintoma novo ("sem pendrive"): não é
um bug de detecção de USB, é o console log bloqueando o loop por tempo
demais.

**Fix corrigido** (`src/host/raspi3/miniuart_backend.c`): removido o
spin-wait. Agora `miniuart_backend_write_byte()` é não-bloqueante — checa o
bit uma vez, se o FIFO estiver cheio retorna `false` imediatamente (mesmo
contrato que `pl011_backend_write_byte()` já usa). `console_log_drain()`
(`src/host/raspi3/console_log.c`) agora para o loop no primeiro `false` em
vez de ignorar o retorno e avançar a `tail` mesmo assim (o que estava
descartando o byte ao invés de tentar de novo no próximo quantum). O
caminho de Paula (`machine_step_host_serial_rigel()` em
`machine_rigel_step.c`) já tratava `false` corretamente — não precisou
mudar.

Build verificado de novo com as flags exatas do usuário
(`BELLATRIX_CPU_BACKEND=musashi BELLATRIX_BTSTACK=0 BELLATRIX_USBSTACK=1`) —
compila limpo. **Ainda não testado no hardware.**

**Testado na Pi física (04/07): ainda sem funcionar, "sem pendrive" persiste
mesmo com o spin-wait removido** (confirmado que o build tinha
`BELLATRIX_USBSTACK=1`, então não é simplesmente USB desligado). Isso
descarta a hipótese do spin-wait como causa do "sem pendrive" e aponta de
volta pro TLSF.

## Terceira regressão auto-introduzida: SIZE_ALIGN do TLSF upstream é 16, não 64

O `tlsf.c` que portei do upstream (`patches/0019`) define
`SIZE_ALIGN=16` (valor do AROS/upstream), enquanto o nosso original usava
`SIZE_ALIGN=64`. **CherryUSB aloca os buffers de DMA do host DWC2 direto
desse heap** (`usb_osal_malloc()` em `src/io/usb/usb_libc_bellatrix.c:106`
→ `tlsf_malloc(tlsf, size)`). A Cortex-A53 (Pi 3B) tem linha de cache de 64
bytes — um buffer alinhado a só 16 bytes pode compartilhar linha de cache
com uma alocação vizinha não relacionada, e uma escrita DMA nesse buffer
corrompe o vizinho. Isso explica o "sem pendrive": a detecção/enumeração
USB (que aloca descritores/buffers via esse heap) quebra silenciosamente.

**Fix**: `SIZE_ALIGN` revertido para `64` em `emu68/src/tlsf.c` (mantendo
todas as correções de lógica do upstream, que não dependem desse valor —
`diff_begin >= ROUNDUP(sizeof(hdr_t))`, detecção de double-free, guards de
overflow). Patch `0019` regenerado. Build compila limpo com as mesmas
flags. **Ainda não testado no hardware.**

**Testado na Pi física (04/07): CONFIRMADO — o USB/pendrive voltou a
funcionar** depois do fix de `SIZE_ALIGN=64`. Isso valida a hipótese: eram
duas causas raiz distintas que só coincidiram no tempo (TLSF alignment →
USB; algo ainda não identificado → serial). **O serial continua com
problema.**

Os fixes de TLSF (hardening + alignment) e FIFO-pacing da mini-UART
continuam válidos e commitáveis — resolveram regressões reais (USB e o
overrun de FIFO), mesmo que não sejam a causa raiz do bug original do
serial.

## Instrumentação de diagnóstico adicionada (`src/host/raspi3/console_log.c`)

Duas sondas novas em `console_log_init()`, pra separar "problema de
clock/baud/pino da mini-UART" de "problema na lógica do ring
buffer/kprintf":

1. `kprintf("[CONSOLE-DIAG] core_hz=%u\n", ...)` — impresso **antes** de
   `pl011_backend_route_header_to_miniuart()` remuxar os pinos, então sai
   garantidamente pelo PL011 nativo do Emu68 (sempre confiável). Mostra o
   valor de clock que `vc_get_core_clock_hz()` leu (ou o fallback de 250MHz
   se a leitura via mailbox falhou).
2. `console_log_diag_blocking_write("\r\n[MINIUART-DIAG-0123456789-ABCDEFGHIJ]\r\n")`
   — escrita direta e bloqueante na mini-UART, **contornando o ring buffer**
   inteiro, logo depois de `miniuart_backend_open_clk()` ter sucesso.

Como interpretar o resultado no teste de hardware:
- Se **nem o `[CONSOLE-DIAG] core_hz=...` aparece no PL011**: o problema é
  anterior a tudo isso — ou o PL011 nativo do Emu68 já não está saindo
  limpo (pouco provável, é o caminho de boot mais antigo e testado), ou o
  adaptador/conexão física mudou.
- Se `core_hz` aparece limpo mas o `[MINIUART-DIAG-...]` sai **corrompido**:
  o bug é no nível de clock/baud/GPIO da mini-UART em si — o valor de
  `core_hz` lido está errado, ou o divisor de baud/timing do
  `miniuart_backend_open_clk()` está incorreto, ou o remux de pino
  (`pl011_backend_route_header_to_miniuart()`) não está estável.
- Se o `[MINIUART-DIAG-...]` sai **limpo** mas o texto do `kprintf` normal
  depois (banner de boot etc.) continua corrompido: o hardware/clock estão
  OK, e o bug está na lógica do ring buffer (`ring_push`/
  `console_log_putc`/`console_log_drain`) ou no mecanismo de
  `kprintf_set_putc_override`.

Build compila limpo.

## Teste na Pi física com a instrumentação: só `"[CONSOLE-DIAG] co"` apareceu

Nada mais depois disso até depois do launcher (onde vídeo/launcher
funcionam normalmente — a UI aparece na tela), quando o serial volta a
"jogar lixo". Ou seja: a própria linha de diagnóstico, que ainda roda no
PL011 nativo do Emu68 (antes do remux de pino pra mini-UART), trava no meio
da própria transmissão.

Contagem: `"[CONSOLE-DIAG] "` = 15 caracteres + `"co"` = 2 → parou exatamente
no **17º byte**. O FIFO de TX do PL011 tem **16 bytes** de profundidade.
Isso é a assinatura clássica de FIFO-cheio: o 17º byte precisa esperar 1
posição do FIFO drenar, e `waitSerOUT()` (`emu68/src/raspi/support_rpi.c:100`)
é um `while(1)` **sem timeout** — se o FIFO não drenar mais, trava para
sempre.

**Causa provável identificada**: nosso `console_log_init()` chamava
`vc_get_core_clock_hz()` — uma implementação **própria e separada** de
consulta à mailbox VC (`src/host/raspi3/vc_mailbox.c`), rodando bem no
início do boot (logo após `setup_serial()`, antes de MMU/cache estarem
plenamente assentados). Isso é uma **segunda** transação de mailbox, cedo
demais, usando um buffer estático nosso — enquanto o Emu68 upstream já tem
`get_clock_rate()` (`emu68/src/raspi/support_rpi.c:257`), com um buffer fixo
pré-mapeado (`0xffffff9000001000`), que `setup_serial()` **acabou de usar
com sucesso** um instante antes (é assim que o PL011 fica configurado). Uma
segunda chamada de mailbox própria, tão cedo, é um risco desnecessário —
suspeita de desestabilizar o clock/UART e travar a drenagem do FIFO do
PL011 logo em seguida.

**Fix aplicado**: `console_log_init()` agora chama `get_clock_rate(4)`
(mesma função, mesmo ID de clock CORE, já comprovada por `setup_serial()`)
em vez de `vc_get_core_clock_hz()`. Elimina a segunda transação de mailbox
inteiramente. `vc_get_core_clock_hz()`/`vc_mailbox.c` continuam existindo
(não têm mais nenhum chamador em `src/`, mas não removi o arquivo — pode
ser útil depois de MMU/cache estarem assentados, e não é o foco desta
issue).

**Testado na Pi física: só mudou de "co" pra "cor"** (1 byte a mais) — ou
seja, trocar o mecanismo de mailbox não resolveu, e o deslocamento mínimo
sugere que a causa não era a chamada de mailbox em si.

**Reavaliação importante**: o boot **continua** depois disso (launcher
aparece normalmente na tela) — se fosse um travamento de verdade em
`waitSerOUT()` (loop infinito sem timeout), a CPU ficaria presa lá para
sempre e o launcher nunca apareceria. Como o launcher funciona, a execução
NÃO está travada. O que parece "parar" no terminal é, na verdade, byte
corrompido/ilegível a partir dali — não silêncio real.

PuTTY confirmado em 115200 (baud nativo do PL011, `DEF_BAUD` no Emu68) — não
é descasamento de baud simples (isso corromperia desde o primeiro byte, não
um prefixo limpo seguido de lixo).

## Causa mais provável encontrada: remux de pino durante transmissão em voo

`putByte()`/`waitSerOUT()` (`emu68/src/raspi/support_rpi.c:100`) só espera o
**FIFO ter espaço** (`FR_TXFF`) antes de retornar — não espera a
transmissão **terminar de sair pelo fio** (`FR_BUSY`). `kprintf()` pode
retornar com os últimos bytes ainda fisicamente saindo do UART. Nosso
código chama `pl011_backend_route_header_to_miniuart()` **imediatamente**
depois do `kprintf()` de diagnóstico — isso reprograma a função ALT dos
pinos GPIO14/15, arrancando-os do PL011 **enquanto a cauda da transmissão
ainda está em andamento**. Isso corrompe exatamente os últimos bytes — bate
com "prefixo limpo, depois lixo" de forma bem mais precisa que qualquer
teoria de baud ou heap.

**Fix aplicado**: chamar `pl011_backend_wait_idle()` (já existia,
`pl011_backend.c:214`, checa `FR_BUSY`) logo após o `kprintf` de
diagnóstico e antes de `pl011_backend_route_header_to_miniuart()`.

**Testado na Pi física: funcionou parcialmente.** `[CONSOLE-DIAG]
core_hz=400000000` saiu **completo e limpo** pela primeira vez — confirma
que o fix do `wait_idle()` resolveu a corrupção de cauda no PL011. Mas nada
mais apareceu depois disso (nem o `[MINIUART-DIAG-...]`, o teste direto na
mini-UART) até o launcher — silêncio total, não corrompido.

## Observação crítica do usuário: o mini-UART já funcionava perfeitamente antes

O usuário apontou (corretamente) que no bisect desta sessão, **`0a0ee59`
funcionou com log legível completo pela mini-UART**, usando praticamente o
mesmo código (`console_log.c`/`miniuart_backend.c` não foram tocados entre
`0a0ee59` e `1415619`, e só foram alterados por mim nesta sessão). Isso
significa que a leitura de clock via mailbox (`vc_get_core_clock_hz()`
original), a escrita incondicional no FIFO (sem checar LSR), e a ausência
de `wait_idle()` — tudo isso **já funcionava** nesse hardware antes. Minhas
hipóteses de "hardware/clock/FIFO real quebrado" não se sustentam à luz
disso: se o código quase idêntico funcionava, o problema não é
fundamentalmente eletrônico/de configuração de baud.

**Suspeita mais provável agora**: o gate de LSR bit 5 que adicionei em
`miniuart_backend_write_byte()` (pra evitar overrun do FIFO) pode estar
resultando em **nunca escrever nada** — se esse bit não se comporta como
esperado neste hardware/timing específico, toda escrita retornaria `false`
silenciosamente, explicando por que nem o teste direto (que bypassa o ring
buffer) apareceu.

**Testado: mesmo resultado** (`[CONSOLE-DIAG] core_hz=...` aparece, silêncio
até o launcher, lixo depois). O usuário apontou, corretamente, que eu devia
parar de teorizar incrementalmente e simplesmente **reproduzir o estado
exato que sabemos que funciona** (`0a0ee59`), byte a byte, em vez de ajustar
hipóteses uma de cada vez.

## Reprodução exata de `0a0ee59` (sem nenhuma especulação em cima)

`src/host/raspi3/console_log.c` e `src/host/raspi3/miniuart_backend.c`
restaurados via `git show 0a0ee59:<path>` — **idênticos byte a byte** ao
commit que o bisect confirmou funcionando (verificado com `diff`, sem
nenhuma saída). `pl011_backend.c` já era idêntico a `0a0ee59` (nunca foi
tocado desde então, confirmado via diff também). Ou seja: **os três
arquivos do caminho de log/serial estão agora 100% iguais ao estado
conhecido-bom**, sem nenhum dos meus fixes desta sessão (`wait_idle`,
`get_clock_rate`, gate de LSR, diagnóstico) — tudo removido.

O que resta diferente entre este build e o `0a0ee59` real:
- `1415619` (bt_host.c) e tudo que veio depois no histórico principal —
  incluindo o paradoxo original, ainda não explicado.
- O fix de TLSF desta sessão (`patches/0019`, `SIZE_ALIGN=64` preservado) —
  mantido, porque é uma correção confirmada e independente (resolveu o
  USB), não uma teoria.
- `tlsf_set_flags(..., TLSF_MULTITHREADING)` em `patches/0007`.

Build compila limpo. **Ainda não testado no hardware.** Este é o teste mais
importante até agora: se falhar mesmo com o código de serial 100% idêntico
ao que funcionava, isso **prova definitivamente** que a causa nunca esteve
em `console_log.c`/`miniuart_backend.c`/`pl011_backend.c` — está de volta
no paradoxo original do `1415619`/TLSF/heap, exatamente onde o bisect tinha
apontado antes de eu começar a instrumentar o caminho serial.

---

# Sintoma

Console serial (USB-TTL + PuTTY, GPIO14/15) não funciona no firmware
bare-metal atual (HEAD) rodando numa Raspberry Pi 3B física:
- Inicialmente: silêncio total (nem a primeira mensagem `[BOOT] Booting
  Emu68...` aparece).
- Depois de um fix local nesta sessão (ver abaixo): passou a mostrar
  caracteres de lixo uma vez e parar — testado em 9600 e 115200 baud,
  ambos ruins (não é simplesmente baud nominal errado).

Vídeo/HDMI e o launcher (menu de seleção de ROM) funcionam normalmente
nesse mesmo firmware — a regressão é isolada ao console serial.

Builds antigos confirmados funcionando **de verdade** (serial legível, com
o emulador rodando) em hardware físico: 08/06/2026 e 15/06/2026.

O harness (Linux x86_64) nunca exercitou esse caminho — não faz bootstrap
ARM real (MMU, VC4, mailbox, wakeup de cores) nem usa esse console. "Passar
no harness" não valida esse código.

# Fix aplicado nesta sessão (sintoma mudou, causa raiz não)

`src/host/raspi3/console_log.c`: `console_log_init()` chamava
`kprintf_set_enabled(0)` quando `miniuart_backend_open_clk()` falhava,
desligando o `kprintf` inteiro pro resto do boot (silêncio total,
irrecuperável). Trocado pra: se a abertura falhar, mantém o `kprintf` no
caminho direto em vez de desligar tudo. Isso não resolveu o problema, só
tornou o sintoma observável (de "nada" pra "lixo uma vez e para"). Essa
mudança está só no worktree de trabalho, **não commitada ainda** — ver
`src/host/raspi3/console_log.c` no checkout principal
(`/home/jaime/bellatrix`).

# CAUSA RAIZ ISOLADA POR BISECT: commit `1415619`

**Commit exato que introduz a regressão**: `1415619` ("bluetooth: drop
local console handoff", 18/06/2026 03:13:54), imediatamente após `0a0ee59`
(03:09:22, que criou `console_log.c` e **funciona**).

Bisect completo (do mais recente ao mais antigo):

| Commit | Data | Resultado |
|---|---|---|
| HEAD (57314f7) | 04/07 | ❌ falha |
| `8d15e88` | 28/06 | ❌ falha |
| `38afddd` | 21/06 | ❌ falha |
| `2c96245` | 18/06 03:15 | ❌ falha |
| **`1415619`** | **18/06 03:13** | **❌ falha — commit da regressão** |
| `0a0ee59` | 18/06 03:09 | ✅ funciona |
| `768784f` | 18/06 00:54 | ✅ funciona |
| `a2653c7` | 18/06 00:39 | ✅ funciona |
| `0e6e4e9` | 15/06 08:03 | ✅ funciona (âncora) |

## O que o commit `1415619` faz

Único arquivo tocado: `src/io/bluetooth/bt_host.c` (203 linhas removidas,
12 adicionadas). Remove um mecanismo antigo de handoff temporário de
console (`bt_console_handoff()`/`bt_console_release()`/
`bt_console_drain()`/ring buffer próprio/`vc_get_core_clock_hz()` local
duplicado/`vc_mbox_send`/`vc_mbox_recv` locais duplicados) que existia pra
mover o `kprintf` do PL011 pro mini-UART **só durante** a operação do
Bluetooth (já que BT precisa do PL011 pra si). O comentário que substitui
o código removido diz:

> "kprintf already lives permanently on the mini-UART by the time BT
> exists (bellatrix_early_console_init(), called at the very start of
> boot) — PL011 is BT's alone, no runtime handoff needed here."

Ou seja: assume que o mecanismo novo de `console_log.c` (de `0a0ee59`, que
já roda ANTES de qualquer coisa de BT) torna esse handoff redundante.

## O paradoxo ainda não resolvido

`bt_host_init()` (onde vivia todo esse código, incluindo o que foi
removido) só é chamado se `BELLATRIX_ENABLE_BTSTACK=1`
(`src/cpu/emu68/bellatrix.c:623`, dentro de `#if BELLATRIX_ENABLE_BTSTACK`).
**Os testes de bisect usaram `BELLATRIX_BTSTACK=0`** (default, BT
desligado) — ou seja, `bellatrix_init_bluetooth()` nunca é chamado, e todo
o código removido por `1415619` deveria ser 100% morto/inerte no cenário
testado.

Ainda assim, o bisect é claro e reproduzível: com o código antigo presente
(mesmo nunca executado), o serial funciona; com ele removido, não
funciona. **A causa exata ainda não foi explicada estaticamente.**

Hipóteses a investigar na próxima sessão (nenhuma confirmada):
1. `vc_get_core_clock_hz()` — antes de `1415619`, existiam DUAS
   implementações: uma `static` em `bt_host.c` (inerte) e a global em
   `src/host/raspi3/vc_mailbox.c` (criada em `0a0ee59`, usada por
   `console_log.c`). Confirmado que a de `bt_host.c` era `static` (sem
   colisão de símbolo) — então a teoria de "duas definições conflitantes"
   não se sustenta à primeira vista, mas vale checar com mais cuidado
   (nomes de macros `VC_MBOX_*`, `ARM_PERI_VIRT_BASE` etc. — `1415619`
   remove definições locais duplicadas dessas macros também, que podem
   colidir/differir sutilmente com as de `vc_mailbox.h`).
2. Ordem de link/inicialização: talvez o simples fato de `bt_host.c` ter
   menos código (ou não incluir mais certos headers) mude o layout de
   memória/seção o suficiente pra expor um bug de timing já latente em
   `console_log_init()`/`miniuart_backend_open_clk()` — especulativo, não
   verificado.
3. Pode haver uma diferença sutil entre o binário que o bisect testou
   (reconstruído manualmente pelo agente, com patches aplicados à mão por
   causa de um problema de patches fora de sincronia — ver seção abaixo)
   e o binário real do commit. Vale re-testar `0a0ee59` e `1415619` a
   partir de uma checagem mais rigorosa antes de aprofundar teorias de
   código.

# Bloqueio técnico de tooling (encontrado durante o bisect, resolvido)

Vários commits no meio da janela de bisect (`a2653c7` até `2c96245`) têm
patches (`0007-bellatrix-boot-sequence.patch` principalmente) que não se
aplicam limpo contra o submódulo `emu68` — o conteúdo dos patches
"vazou"/mudou de arquivo entre `0002-add-bellatrix-bus-hook.patch` e
`0007` ao longo do histórico (regenerados em commits diferentes,
descobrindo overlaps). Também há dois arquivos (`src/cpu/emu68/
bellatrix_profile.c/.h`, `src/runtime/cpu_progress.h`) referenciados no
CMake antes de existirem por ~3 minutos (`a2653c7`/`40adee1` até
`2c96245`). Para testar os pontos intermediários, o agente copiou esses
arquivos do commit `2c96245` e reconstruiu manualmente os hunks que
falhavam, comentando cada decisão inline no shell. Isso funcionou (builds
compilaram e foram testados com sucesso em hardware), mas introduz risco
de pequena divergência do binário real — ver hipótese 3 acima.

# Próximos passos (para a próxima sessão)

1. Re-verificar `1415619` com mais rigor: idealmente, testar o commit
   seguinte real e completo do histórico oficial (sem reconstrução manual)
   pra eliminar de vez a hipótese de erro de tooling do bisect.
2. Se confirmado que `1415619` é mesmo a causa: ler o diff completo de
   novo com atenção às macros `VC_MBOX_*`/`ARM_PERI_VIRT_BASE` duplicadas
   removidas — checar se `vc_mailbox.h`/`vc_mailbox.c` define algo
   sutilmente diferente (endereço errado, ordem de bytes, timeout) do que
   a cópia antiga inline em `bt_host.c` tinha.
3. Alternativa: debug ao vivo na Pi física — adicionar `kprintf` extra em
   `console_log_init()` e `miniuart_backend_open_clk()` pra ver
   exatamente em que ponto/valor a abertura da mini-UART diverge entre os
   dois commits.
4. **Voltar o console pro PL011 direto NÃO é uma opção** — PL011 já
   pertence ao Bluetooth em todo build (decisão de arquitetura anterior,
   não deste issue). O fix tem que estabilizar/corrigir o caminho do
   mini-UART, não abandoná-lo.
5. Depois de confirmar a causa raiz: fix definitivo, commitar o fix do
   `console_log.c` (item já pronto, listado acima) junto ou separado, e
   fechar este issue.

# Worktrees de bisect (temporários, não commitados, ainda no disco)

`/tmp/claude-1000/-home-jaime-bellatrix/6deb1c25-0455-4973-b68b-427f5eee3a34/scratchpad/bisect/wt1`
até `wt9` — um por commit testado (`wt9` = `1415619`, o último testado).
Remover com `git worktree remove --force <path>` quando não forem mais
necessários (são todos filhos do repo principal, `git worktree list` na
raiz do repo mostra todos). Esses paths são de sessão temporária e podem
já não existir na próxima sessão — se for continuar o bisect, recriar via
`git worktree add --detach <path> <commit>` + `git submodule update --init
--recursive` + aplicar patches manualmente conforme documentado acima.

(Nota: a tabela de bisect acima e a lista de worktrees `wt1..wt9` são da
**primeira tentativa de bisect, hoje invalidada** — ver seção no topo do
arquivo, "ATUALIZAÇÃO (04/07, continuação #2)", para o estado atual e
confiável da investigação.)

---

# ATUALIZAÇÃO (04/07, continuação #3): teste decisivo proposto pelo usuário — BT ligado no build mais antigo limpo

Ideia do usuário: se o handoff **antigo** de console (lazy, só ativa quando o BT reivindica o PL011 —
existe desde `85e3b50`, 11/06, **antes** até do build de 15/06 confirmado bom) funcionar hoje com BT de
verdade, isso prova que o mecanismo de mini-UART em si funciona quando acionado mais tarde no boot —
apontando pra **timing cedo demais no boot** como causa, não pro mecanismo em si.

Contexto: `console_log_init()` (chamado incondicionalmente logo no início de `boot()`, antes de quase
tudo) só passou a existir em `768784f`/`0a0ee59` (18/06). Antes disso, com `BTSTACK=0` (config de todo
este bisect), o kprintf nunca saía do PL011 nativo — por isso todo commit anterior "funciona"
trivialmente, sem nunca exercitar a mini-UART de verdade. O handoff antigo do BT (`bt_console_handoff()`
em `bt_host.c`, removido por `1415619`) só troca pro mini-UART **depois** de boot bem mais avançado
(quando o BT começa a inicializar) — momento bem diferente de "logo no início do `boot()`".

Build de `0e6e4e9` (âncora limpa confirmada) com `BELLATRIX_BTSTACK=1 BELLATRIX_USBSTACK=1` desta vez
(antes só testado com `BTSTACK=0`). Precisou de mais um patch — `patches/0008-bellatrix-console-redirect.patch`
— pra suprir `kprintf_set_enabled`/`kprintf_set_putc_override` (que `bt_host.c` já usa, mas não existem
ainda no `support.h` nessa janela; mesmo padrão de gap histórico do `bellatrix_profile.c`). **Esse
patch nunca mudou desde que foi criado em `768784f`** (`git diff 768784f HEAD -- patches/0008...`
= vazio) — aplicado tal como está, sem nenhuma edição de lógica.

Build pronto:
```
/tmp/claude-1000/-home-jaime-bellatrix/6deb1c25-0455-4973-b68b-427f5eee3a34/scratchpad/bisect-clean/emu68/install-bellatrix-rigel-musashi/
```

**Pendente**: testar no hardware, parear via Bluetooth, ver se o console sobrevive à operação do BT
(handoff pro mini-UART) de forma legível.
- Se **sim**: o mecanismo de mini-UART funciona quando acionado tarde no boot — causa é
  especificamente sobre fazer isso cedo demais (antes de MMU/cache/mailbox VC assentarem). Próximo
  passo: atrasar `console_log_init()` pra depois de mais coisa do boot estar pronta, sem depender do
  BT pra isso.
- Se **não**: o mecanismo de mini-UART em si nunca funcionou, independente de quando é acionado —
  bug mais fundamental no `miniuart_backend.c`/`pl011_backend.c`/config do GPIO, não relacionado a
  timing de boot.

**Testado: CONFIRMADO — o mecanismo funciona (com glitches) quando acionado tarde no boot.** Isso
resolve o mistério: não é o mecanismo de mini-UART que está quebrado, é fazer a troca cedo demais
(logo após `setup_serial()`, antes de MMU/mapeamentos/resto do boot assentarem).

## Fix aplicado: mover `console_log_init()` pra bem mais tarde no boot

`patches/0007-bellatrix-boot-sequence.patch`: a chamada de `console_log_init()` (que antes rodava
logo após `setup_serial()`, uma das primeiras coisas em `boot()`) foi movida pra imediatamente antes
de `bellatrix_init()` — bem perto do fim de `boot()`, no mesmo ponto (em espírito) onde o handoff
antigo do BT disparava (bem depois de MMU, mapeamentos de ROM, chip RAM, etc. já estarem prontos).

Trade-off: perdemos o log dos primeiros ~600 linhas de boot do Emu68 (que ficam só no PL011 nativo,
nunca chegam no mini-UART) — mas isso é um preço aceitável comparado a não ter log nenhum depois do
launcher. Se precisarmos do log bem cedo no futuro, dá pra reconsiderar.

**Testado: progresso real.** Muito mais log apareceu (todo o boot do Emu68 — memória, MMU, cores,
clocks, VC4/framebuffer — dezenas de linhas limpas!). Travou de novo exatamente no momento da troca de
pino pra mini-UART (`"[BOOT] Disabling mail▒▒▒▒..."`) — **a mesma corrupção de cauda no PL011** que eu
já tinha diagnosticado e corrigido antes (`pl011_backend_wait_idle()`), removida quando reconstruí
`console_log.c` idêntico a `0a0ee59` a pedido do usuário. Reaplicado agora, em cima do fix de timing:

`src/host/raspi3/console_log.c`: adicionado `pl011_backend_wait_idle()` logo antes de
`pl011_backend_route_header_to_miniuart()` em `console_log_init()` (mesmo fix de antes, agora
combinado com o `console_log_init()` já estar bem mais tarde no boot).

**Testado de novo (com `wait_idle()` já reaplicado): mesmo resultado exato** — trava em
`"[BOOT] Disabling mailbox interrupts"`. Isso descartou a teoria de que só faltava o `wait_idle()`.

## Nova causa provável: race condition com os outros cores (multicore já ativo nesse ponto)

Diferença chave em relação ao teste anterior (quando `console_log_init()` rodava logo no início do
`boot()`): agora, sendo chamado bem mais tarde, **os cores 1/2/3 já foram acordados** ("Waking up CPU
1/2/3" aparece no log antes desse ponto). Se outro core estiver no meio de um `kprintf()` (protegido
pelo `print_lock` interno do Emu68, ao qual `console_log.c` não tem acesso) exatamente quando o core 0
faz `pl011_backend_wait_idle()` + remux dos pinos, isso pode corromper a transmissão do outro core — o
`wait_idle()` sozinho não fecha essa race, porque outro core pode começar uma escrita nova bem depois
do `wait_idle()` retornar mas antes do remux realmente acontecer.

**Fix aplicado**: `kprintf_set_enabled(0)` logo no início de `console_log_init()` (antes até do
`wait_idle()`) — impede que QUALQUER core comece uma nova chamada de `kprintf()` durante a transição
(a flag é checada uma vez, no início de cada chamada). Combinado com `wait_idle()` (que deixa uma
transmissão já em andamento terminar), fecha a race: nenhuma escrita nova começa, e a que já estava en
curso tem tempo de esvaziar antes do remux. Reabilita (`kprintf_set_enabled(1)`) só depois do
`kprintf_set_putc_override()` já estar instalado.

Build compila limpo com as flags do usuário (`musashi`, `BTSTACK=0`, `USBSTACK=1`). **Ainda não
testado no hardware.**

## Nota do usuário: no handoff antigo do BT, os logs do BT saíam bons na mini-UART

Isso é uma pista adicional importante: no mecanismo antigo (`bt_console_handoff()`, removido por
`1415619`), depois da troca de pino, o tráfego de log do próprio BT (`bt_diag_log`, etc.) chegava
legível na mini-UART. Isso é consistente com a raça de multicore ser a explicação certa: o handoff
antigo acontecia dentro de `bt_bootstrap_step()`, chamado repetidamente a partir do loop de step do
Core 0 (ou de onde quer que BT rode) — não necessariamente com outros cores ativamente imprimindo
naquele exato instante, e a`inda tinha timing diferente o suficiente pra não expor essa race. Reforça
que o problema é especificamente sobre coordenação entre cores na transição, não sobre o mecanismo de
mini-UART em si (que já sabíamos, do teste anterior, que funciona quando dá certo).

**Testado: fix de multicore não resolveu** (mesmo resultado exato, trava em "Disabling mailbox
interrupts"). Testei também pedir pro usuário reconfigurar a PuTTY pra 9600 baud (já que
`console_log_init()` abre a mini-UART a 9600) — **também ilegível**. Isso finalmente forçou reler o
código do handoff antigo com atenção total.

## CAUSA RAIZ REAL ENCONTRADA: baud da mini-UART nunca bateu com o PL011 nativo

Relendo `bt_console_handoff()` (o código removido por `1415619`) com cuidado:
```c
if (miniuart_backend_open_clk(&s_console_miniuart, 115200u, core_hz)) {
```
**115200, não 9600!** O handoff antigo abria a mini-UART na MESMA taxa do PL011 nativo do Emu68
(`DEF_BAUD 115200`) — por isso o usuário nunca precisava trocar a velocidade da PuTTY no meio da
sessão, e por isso os logs do BT saíam legíveis (nota do usuário acima). O "sink" que o usuário
lembrava não era uma estrutura de dados — era literalmente isso: a taxa final nunca mudava.

`console_log.c` sempre usou **9600** (desde `0a0ee59`, nunca foi 115200) — um valor errado desde a
criação da versão centralizada, justificado por um comentário incorreto ("matches Paula's existing
host-side baud"). Isso sozinho já explicaria todo o sintoma "prefixo limpo no PL011 (115200), lixo
depois da troca pra mini-UART (9600)" — sem precisar de nenhuma das teorias anteriores (TLSF, FIFO,
wait_idle, race de multicore). Todas essas continuam válidas como fixes de bugs reais e independentes,
mas **nenhuma delas era a causa do sintoma principal reportado nesta issue**.

**Segundo ponto crítico**: `src/cpu/emu68/bellatrix.c:604` (bridge serial da Paula,
`uart_host_open_miniuart(&m->uart_host, 9600)`) reabre esse MESMO hardware físico a 9600 logo depois,
dentro de `bellatrix_init()` — mesmo se só `console_log_init()` fosse corrigido, a Paula reconfiguraria
o registrador de baud de volta pra 9600 momentos depois.

**Fix aplicado**: ambos os locais mudados para **115200**:
- `src/host/raspi3/console_log.c`: `miniuart_backend_open_clk(&s_console_miniuart, 115200u, core_hz)`
- `src/cpu/emu68/bellatrix.c:604`: `uart_host_open_miniuart(&m->uart_host, 115200)`

Agora a linha física roda a 115200 do início ao fim do boot — igual ao mecanismo antigo que o usuário
confirmou funcionando.

**Testado: AINDA falha**, mas o padrão de corrupção mudou (agora `UUUU▒▒VV...` — muitos `U` = `0x55` =
`01010101` em binário, a assinatura clássica de descasamento de baud). Usuário perguntou corretamente:
(1) se os diagnósticos de frequência estavam poluindo o log (não, é corrupção real) e (2) se eu tinha
comparado com atenção o código de referência que sabemos funcionar com BT (`bt_console_handoff()`).

Comparei `pl011_backend_route_bluetooth_pi3()` (usado pelo handoff antigo) contra
`pl011_backend_route_header_to_miniuart()` (usado hoje) linha a linha — **sem diferença relevante**
pros pinos 14/15 (a única diferença é que a versão do BT também configura os pinos 30/31/32/33/43,
irrelevante pro nosso caso). Não é isso.

**Diagnóstico adicionado** (`console_log.c`): `kprintf("[CONSOLE-DIAG] core_hz=%u divisor=%u (target
115200 baud)\n", ...)` logo no início de `console_log_init()`, ainda no PL011 (antes de
`kprintf_set_enabled(0)`), pra ver o valor real de clock e o divisor calculado nesse ponto específico
do boot (bem mais tarde que o teste anterior, depois da troca de ARM clock pra 1200MHz) — substituindo
suposição por fato de registrador.

**Testado: `core_hz=400000000 divisor=433`** — matematicamente correto (400MHz/433 ≈ 115207Hz, erro de
0,006%, irrelevante). Não é erro de cálculo de baud. Comparei também as duas rotas de GPIO
(`pl011_backend_route_bluetooth_pi3()` do handoff antigo vs `pl011_backend_route_header_to_miniuart()`
de hoje) linha a linha — sem diferença relevante pros pinos 14/15.

## Log completo do usuário comparado (build `0e6e4e9`+BT): achada a causa real (backlog nunca drenado)

Usuário testou o build `0e6e4e9`+BT de novo e mandou o log **completo**. Achado crucial: depois do
trecho inicial corrompido logo após `"[BT] handing PL011 to BT..."`, o log **se recupera totalmente**
e mostra centenas de linhas limpas (`[BT-HAL]`, `[SCAN]`, `[USB-MSC]`, `[EMMC]`, `[FAT32]`, etc.) pelo
resto da sessão. Isso é diferente do nosso teste atual, onde a corrupção nunca se recupera.

Diferença estrutural real: no handoff antigo, `bt_console_drain()` é chamado a cada iteração de
`bt_host_step()` — bem frequente, drena quase em tempo real, nunca acumula backlog. No nosso caso,
`console_log_drain()` só roda a partir do loop de step do chipset/machine, que só começa **depois**
que `bellatrix_init()` inteiro (USB, storage, launcher, etc.) já gerou todo o seu próprio log via
`kprintf` — tudo isso fica acumulado no ring buffer, nunca drenado, até o momento em que o step loop
finalmente começa. Nesse momento, um backlog grande é despejado de uma vez.

`miniuart_backend_write_byte()` ainda escrevia **incondicionalmente** (revertido pra bater com
`0a0ee59` a pedido do usuário, dias atrás nesta sessão) — com um backlog grande, isso estoura o FIFO
de 8 bytes repetidamente, em TODAS as chamadas de drain subsequentes (não só uma vez), o que
corresponde exatamente a "corrupção sustentada" em vez de "glitch transitório que se recupera".

**Fix reaplicado** (desta vez pra ficar): `miniuart_backend_write_byte()` volta a checar o bit de
FIFO-livre (LSR bit 5) antes de escrever, retornando `false` se cheio. `console_log_drain()` para o
loop no primeiro `false` em vez de descartar o byte e continuar. O caminho de Paula
(`machine_step_host_serial_rigel()`) já tratava `false` corretamente, sem mudança.

Build compila limpo com as flags do usuário (`musashi`, `BTSTACK=0`, `USBSTACK=1`). **Ainda não
testado no hardware.**
