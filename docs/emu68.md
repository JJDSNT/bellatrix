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

## API pública Bellatrix/Emu68

A API pública inicial deve seguir a mesma regra:

- contrato e adapter vivem em `src/cpu/emu68/`;
- inclusão no firmware acontece por `cmake/bellatrix-variant.cmake`;
- o submódulo `emu68/` só é alterado via `patches/`;
- patches no Emu68 devem ser limitados aos pontos onde o core precisa chamar a
  camada externa.

Estado atual:

- `src/cpu/emu68/emu68_api.h` define o contrato público inicial;
- `src/cpu/emu68/emu68_api_adapter.c` implementa um singleton sobre o runtime
  global atual do Emu68;
- `patches/0021-emu68-public-bus-dispatch.patch` faz `vectors.c` chamar o
  dispatcher de bus público com fallback para `bellatrix_bus_access()`.

Ainda não existe execução por janela real no Emu68. Enquanto o JIT continuar
controlado por `MainLoop()`, `emu68_run_cycles()` e `emu68_step()` devem ser
tratados como API reservada/unsupported.

Documento detalhado: `docs/emu68_public_api.md`.
