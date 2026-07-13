Sim — isso deveria virar **princípio arquitetural explícito**.

Eu colocaria assim nos documentos:

````md
## Emu68 Preservation Principle

Bellatrix deve alterar o mínimo possível o código do Emu68.

O Emu68 já provou funcionar no contexto PiStorm e deve ser tratado como um backend de execução confiável, não como uma base a ser reescrita.

### Regras

- Preferir integração por adaptadores, hooks e camadas externas.
- Evitar modificar o núcleo do JIT.
- Evitar alterar a MMU interna do Emu68 salvo quando estritamente necessário.
- Não deslocar responsabilidades do Bellatrix para dentro do Emu68.
- Manter patches pequenos, isolados e documentados.
- Sempre que possível, preservar compatibilidade com upstream.

### Modelo desejado

```text
Emu68 upstream/core
        ↓
Bellatrix integration layer
        ↓
Bellatrix runtime / chipset / memory semantics
````

### Regra final

> Emu68 deve continuar sendo o motor de execução.
> Bellatrix deve ser a arquitetura ao redor dele.

````

Isso é muito importante porque reduz risco. O Emu68 já validou uma parte difícil: execução 68k rápida em ARM, MMU, cache, JIT e integração PiStorm. Bellatrix deve aproveitar isso, não competir com isso.

Eu também adicionaria uma política prática:

```text
Toda alteração no Emu68 precisa responder:
1. Isso pode ser feito fora do Emu68?
2. Isso pode ser feito por hook/adaptador?
3. Isso quebra compatibilidade com PiStorm/upstream?
4. Isso mistura semântica Bellatrix dentro do backend?
````

A direção correta é: **mínimo patch no Emu68, máximo encapsulamento no Bellatrix**.

## Emu68 public machine API

The authoritative API contract is [Emu68 Public Machine Integration API](emu68_public_api.md).

The Bellatrix-owned files under `src/cpu/emu68/` and the dispatcher added by
`patches/0021-emu68-public-bus-dispatch.patch` describe the current integration,
not the target public API. In particular, dispatching a callback from
`vectors.c` still depends on Data Abort and must not be used as the basis of the
machine API.

The public API is implemented at the JIT memory-emission boundary: direct
regions keep native loads/stores, external regions use an explicit bridge, and
unmapped regions use defined 68k semantics. The detailed document defines the
motivation, current Emu68 operation, and the complete API behavior.
