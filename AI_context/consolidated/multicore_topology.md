# Topologia multicore canônica

> **Congelado em 2026-07-15:** este é o estado provisório deixado pela sessão.
> Não ampliar nem refatorar a topologia enquanto o trabalho de RAM/memory map
> estiver em curso. As lacunas e a validação multicore voltarão em uma sessão
> dedicada.

Este documento é a referência vigente de placement e ownership. Documentos
anteriores que fixam `Core0=host/Core1=Emu68` são históricos e não definem o
runtime atual.

O código comum trabalha por **papel**, não por número de core. O mapa único
está em `src/runtime/topology.h`.

| Papel | Core | Contrato |
|---|---:|---|
| boot, CPU selecionada e ingresso de IRQ física | Core 0 | Emu68 **ou** Musashi, vectors e top-halves ARM |
| auxiliar | Core 1 | estacionado até existir trabalho medido |
| chipset | Core 2 | owner único do Rigel e do tempo emulado |
| host reactor | Core 3 | timeline, bottom-halves USB/BT, miniUART runtime, HDMI e apresentação |

## Invariantes

- A topologia é idêntica com Emu68 e Musashi; selecionar backend troca somente
  a implementação da CPU no Core 0.
- Emu68 no Core 0 ainda é um contrato **provisório de estabilização** da sua
  integração interna. Ele
  preserva juntos JIT, `VBAR_EL1`, `TPIDRRO_EL0`, fault handler e roteamento
  físico original até existir prova de equivalência para outro placement.
- Core 2 é o único stepper do Rigel. CPU e host reactor comunicam-se com ele
  por atomics, filas e fronteiras explícitas; não acessam seu estado interno.
- O host reactor é sempre o Core 3. `bellatrix_runtime_io_step()` reúne seus
  serviços comuns, sem bifurcação por backend de CPU.
- O ingresso de IRQ física continua no Core 0. O top-half PL011/BT roda ali,
  publica trabalho e o bottom-half é consumido no Core 3. Isso não transforma
  IRQ ARM em IPL do Amiga.
- AUX miniUART pertence ao log desde o boot; PL011 pertence ao Bluetooth desde
  o boot. O placement do reactor não altera esse ownership.
- Mudanças de IPL produzidas pelo Rigel são estado emulado. Na topologia Emu68
  são publicadas no `M68KState` nativo por `PAL_IPL_Set()`; no Musashi são
  consumidas na fronteira do backend.
- STOP estaciona Core 0 com `WFE` nos dois backends. A timeline realtime do
  host reactor mantém Core 2 avançando; quando Rigel publica um IPL elegível,
  `SEV` acorda a CPU. `timeline=cpu` permanece override determinístico para
  testes que não dependem de progresso durante STOP.
- Single-core é outro modo de execução, não uma quinta topologia multicore:
  CPU, Rigel e host services cooperam no boot core via `PAL_Runtime_Poll()`.

## Fronteiras de implementação

- `runtime/topology.h`: único mapa papel→core.
- `host/raspi3/pal_core.c`: bootstrap dos workers e loops por papel.
- `runtime/core_chipset.c`: propriedade do Rigel, horizonte e publicação IPL.
- `runtime/core_io.c`: contrato do host reactor.
- `cpu/emu68/bellatrix.c`: seleção do placement provisório e handoff da CPU.
- `emu68/src/aarch64/start.c`: setup original por PE e entrada dos secundários.

Logs novos devem identificar primeiro o papel (`[HOST]`, `[CPU]`,
`[CHIPSET]`). O banner de boot imprime também o mapa numérico efetivo para
diagnóstico.
