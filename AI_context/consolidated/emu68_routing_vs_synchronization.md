# Roteamento vs. sincronização — o mal-entendido que gerou a "public machine API"

> Este documento existe para que o erro conceitual por trás do
> ISSUE-0061/arquivo `AI_context/archive/emu68-public-machine-api-2026-07.md`
> não se repita. Não é sobre o bug em si (esse já está corrigido e
> documentado) — é sobre a confusão de modelo mental que o causou.

## O mal-entendido

Ao propor a "public machine API" (patches 0025-0035, commits `c8599c8` a
`f28b21a`, 2026-07-13), a intenção era **substituir o fault handler** por um
mecanismo mais explícito. A crença por trás disso: *"é o fault handler quem
sincroniza o progresso do chipset — se eu quiser um jeito melhor de
sincronizar, preciso de algo que substitua o fault handler."*

Essa crença estava errada. São duas coisas **completamente diferentes**:

| | O que faz | Pergunta que responde |
|---|---|---|
| **Fault handler** (`vectors.c` → `bellatrix_bus_access`) | **Roteamento.** Quando o M68K toca um endereço específico (registrador de custom chip, CIA, etc.), decide para onde essa leitura/escrita vai. | "Esse endereço é *o quê*, e o que fazer com ele?" |
| **Progress driver** (`bellatrix_emu68_report_jit_progress`, chamado do `MainLoop`) | **Sincronização.** Em todo passo do loop de execução — tocando memória mapeada ou não — informa ao chipset (Rigel) quanto tempo/instruções se passaram, para que ele avance seu próprio relógio (CCK, VBL, IPL). | "Quanto tempo passou, e o chipset já sabe disso?" |

O fault handler só é acionado quando o endereço tocado **precisa de
roteamento** (é um registrador de hardware, não RAM comum). Ele nunca foi
acionado — e nunca poderia ser — por um loop que só lê/escreve RAM comum
(chip RAM é mapeado direto via MMU, propositalmente, para não pagar o custo
de fault em todo acesso). Ou seja: **o fault handler nunca sincronizou
progresso**. Ele não tem esse papel, nunca teve.

## A consequência concreta do mal-entendido

Como as duas coisas foram tratadas como se fossem a mesma decisão, a "public
machine API" acoplou o mecanismo de sincronização (`emu68_machine_dispatch_
quantum_progress`, a substituta pretendida) à MESMA chave de seleção que
decidia o modo de roteamento (`BELLATRIX_EMU68_FAULT_DRIVEN`). Quando esse
modo fault-driven virou o padrão do build (commit `7b4f7c9`, 2026-07-15), o
bloco inteiro de sincronização — incluindo a chamada original e comprovada
`bellatrix_emu68_report_jit_progress()` — foi **excluído da compilação**,
sem nada para substituí-la.

Resultado prático: qualquer trecho do guest que só toca RAM (o loop de idle
do Exec, por exemplo) parava de fazer o chipset avançar. Sem avanço de
chipset, sem VBL. Sem VBL, sem IPL. Sem IPL, o `STOP` nunca acordava. Boot
trava silenciosamente depois de `"[JIT] Let it go..."` — o sintoma que
motivou a sessão inteira de investigação de 2026-07-16.

## A correção conceitual

Roteamento e sincronização são **eixos ortogonais**. Uma escolha em um não
deveria nunca implicar automaticamente uma escolha no outro:

- Pode-se rotear acesso via fault (implícito) OU via chamada explícita a
  cada instrução — isso é só sobre *como* o M68K toca hardware mapeado.
- A sincronização de progresso precisa acontecer **sempre**, em todo passo
  do loop, **independente** de qual mecanismo de roteamento está ativo —
  porque o chipset precisa saber quanto tempo passou mesmo quando nenhum
  hardware foi tocado.

O fix aplicado (`patches/0003`) restaura exatamente essa separação: a
chamada de sincronização voltou a ser incondicional (`#ifdef BELLATRIX`, sem
excluir por modo de roteamento).

## Regra prática para não repetir

Antes de gatear qualquer bloco de código atrás de uma flag existente,
perguntar: **essa flag decide a mesma pergunta que este bloco responde, ou
é uma pergunta diferente que só aconteceu de nascer no mesmo commit?** Se for
uma pergunta diferente, a flag errada é a pista de que duas decisões
distintas foram fundidas em uma só.
