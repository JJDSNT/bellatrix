# bellatrix.card

P96 card driver (m68k) para o board `bellatrix.rtg` — ver
`docs/rtg_design.md` e `src/machine/expansions/rtg/rtg.h` (spec de
registradores, manter em sincronia).

## Build

```bash
cd cards/bellatrix.card
TTY_ENABLED="" ../../emu68/build-scripts/build-m68k-amigaos make
```

Gera `bellatrix.card` (hunk executável com romtag `RTF_COLDSTART`).

## Direção atual

Esta `.card` é o driver guest de uma placa P96 de framebuffer linear portátil.
O contrato não depende de SDL nem VideoCore: o harness e o Raspberry consomem o
mesmo estado de scanout por backends diferentes. A referência comportamental da
retomada é o `MiSTer.card.asm` do Minimig-AGA MiSTer; os endereços físicos do FPGA
não são portados.

Ver o plano e o histórico completo em `docs/rtg_design.md`.

## Estado / pendências

- O p96gfx (residentpri **-10**) escaneia a LibList ANTES do boot de
  disco — a card precisa estar residente cedo. Mecanismo escolhido:
  **DiagArea própria no board bellatrix.rtg** (AC_TYPE_DIAGVALID).
  RTG e lide são subsistemas independentes — não acoplar ao
  Chainloader do lide.
- `boardinfo.h`/`settings.h` copiados de external/VideoCore.card
  (MPL-2.0, projeto Emu68) — atualizar se o upstream mudar.
- Sem blitter (BIF_NOBLITTER) e sem sprite de hardware: p96gfx faz
  fallback por CPU direto na VRAM linear da board Z3.
- A cadeia AROS já foi comprovada até um scanout ativo: `FindCard`, `InitCard`,
  paleta, `SetGC`, `SetPanning` e `SetSwitch` programam 640x480 CLUT.
- O primeiro marco novo são testes host do contrato e do scanout; validação
  AmigaOS/P96 precede a retomada do fluxo AROS/ArosOne.
- A board Z3 fornece uma janela de 8 MB; `WaitVerticalSync` é intencionalmente
  no-op para não bloquear o tick do presenter host.
