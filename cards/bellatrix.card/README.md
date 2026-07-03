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
  disco — a card precisa estar residente cedo. Mecanismo a decidir:
  1. DiagArea própria no board bellatrix.rtg (AC_TYPE_DIAGVALID), ou
  2. estender o Chainloader do lide ROM (que já carrega lide.device do
     bank2) para InitResident também a card.
- `boardinfo.h`/`settings.h` copiados de external/VideoCore.card
  (MPL-2.0, projeto Emu68) — atualizar se o upstream mudar.
- Sem blitter (BIF_NOBLITTER) e sem sprite de hardware: p96gfx faz
  fallback por CPU direto na VRAM (janela linear Zorro II).
