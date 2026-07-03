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

## Estado / pendências (fase 2)

- O p96gfx (residentpri **-10**) escaneia a LibList ANTES do boot de
  disco — a card precisa estar residente cedo. Mecanismo escolhido:
  **DiagArea própria no board bellatrix.rtg** (AC_TYPE_DIAGVALID).
  RTG e lide são subsistemas independentes — não acoplar ao
  Chainloader do lide.
- `boardinfo.h`/`settings.h` copiados de external/VideoCore.card
  (MPL-2.0, projeto Emu68) — atualizar se o upstream mudar.
- Sem blitter (BIF_NOBLITTER) e sem sprite de hardware: p96gfx faz
  fallback por CPU direto na VRAM (janela linear Zorro II).
