---
id: ISSUE-0019
title: "Frame counters — common debug frame definition"
status: backlog
priority: medium
type: infra
owner: unassigned
created_at: 2026-06-27
updated_at: 2026-06-27
tags:
  - harness
  - rigel
  - denise
  - trace
  - diagnostics
related_files:
  - tools/harness/main.c
  - src/machine/machine_rigel_step.c
  - src/machine/machine_rigel_trace.c
  - external/rigel/src/chipset/denise/output/framebuffer.c
  - external/rigel/src/core/rigel_denise_api.c
---

# Resumo

Definir uma nomenclatura comum para contadores de frame usados em debug,
dumps, screenshots e logs. A investigacao do EON mostrou que `frame=...` no
titulo SDL, `BELLATRIX_RIGEL_DUMP_FRAME`, `denise.output.frame_counter` e
`[RIGEL-FRAME-VIDEO] N=...` nao representam necessariamente o mesmo relogio.

# Problema

Hoje existem varios contadores validos, mas com origens e pontos de publicacao
diferentes:

- `harness_frame`: contador local do harness em `tools/harness/main.c`, usado
  pelo titulo SDL, `FRAMES=...`, parada automatica e input scripted.
- `machine_frame`: contador da maquina/presenter, exposto por
  `bellatrix_machine_get()->frame_counter`.
- `video_frame`: contador do frame de video produzido pelo backend Rigel/Denise,
  exposto por `rigel_get_frame().frame_count` e derivado de
  `denise.output.frame_counter`.
- `trace_frame`: contador local de trace em `src/machine/machine_rigel_trace.c`,
  usado em logs como `[RIGEL-FRAME-VIDEO] N=...`.

Como esses contadores resetam, incrementam e sao publicados em pontos
diferentes, um screenshot com `frame=2619` no SDL pode corresponder a outro
numero nos traces internos da Denise/Rigel. Isso dificulta correlacionar
capturas visuais, dumps headless e traces de DMA/sprites.

# Proposta

Usar `video_frame` como frame canonico para investigacao grafica, porque ele
representa a imagem efetivamente produzida pelo backend de video.

Nomenclatura recomendada:

- `harness_frame`: frame do harness/SDL/input/FRAMES.
- `machine_frame`: frame publicado pela maquina/presenter.
- `video_frame`: frame de video Rigel/Denise; frame canonico para dumps e
  comparacao visual.
- `trace_frame`: contador auxiliar local do subsistema de trace.

# Escopo Sugerido

- Documentar `BELLATRIX_RIGEL_DUMP_FRAME` como `video_frame`, ou introduzir
  alias explicito `BELLATRIX_RIGEL_DUMP_VIDEO_FRAME` mantendo compatibilidade.
- Atualizar logs relevantes para imprimir nomes qualificados em vez de
  `frame` generico quando houver ambiguidade.
- Adicionar uma linha de correlacao opcional em pontos de publicacao de frame:

```text
harness_frame=2622 machine_frame=2622 video_frame=2619 trace_frame=1800
```

- Considerar titulo SDL com dois contadores durante investigacao:

```text
Bellatrix 46.1 fps hframe=2622 vframe=2619
```

# Fora de Escopo

- Unificar fisicamente todos os contadores em uma unica variavel.
- Alterar semantica de `FRAMES=...` sem compatibilidade, porque o harness usa
  esse contador para input scripted e parada automatica.
- Usar `trace_frame` como fonte de verdade para dump visual.

# Criterios de Aceite

- [ ] Existe documentacao clara dizendo qual contador cada log/env var usa.
- [ ] Dumps graficos usam nome ou documentacao explicita de `video_frame`.
- [ ] Pelo menos um modo de trace imprime `harness_frame` e `video_frame` na
  mesma linha para correlacao.
- [ ] Issues graficas passam a referenciar frames com origem qualificada quando
  houver risco de ambiguidade.

# Referencias

- `ISSUE-0017`: EON frame 2619 expôs a discrepancia entre SDL/harness e traces
  internos.
- `ISSUE-0008`: EON frame 2619 tambem serve como referencia headless para
  sprite overlay, entao precisa de frame origin bem definido.

# Log

- 2026-06-27: issue criada para padronizar a definicao comum de frame de debug.
