# Perfis oficiais de release bare-metal

Este documento define quais recursos entram nas imagens oficiais do Bellatrix.
Os mesmos perfis são usados nas variantes single-core e multicore; multicore
altera apenas `BELLATRIX_MULTICORE_BUILD`.

## Matriz

| Perfil | CPU | SD card board | Devicetree board | 68040 board | RTG | USB + MSC | BTStack | HDMI audio |
|---|---|---:|---:|---:|---|---:|---:|---:|
| `emu68` | Emu68 | sim | sim | sim | `VideoCore.card` | sim | sim | sim |
| `musashi_68040` | Musashi 68040 | sim | sim | não | `VideoCore.card` | sim | sim | sim |
| `musashi_68000` | Musashi 68000 | sim | não | não | não | sim | sim | sim |

Regras:

- o board de SD card é obrigatório em todas as imagens;
- o board 68040 pertence somente ao perfil Emu68;
- `VideoCore.card` exige o board de devicetree e uma CPU 68040;
- o perfil Musashi 68000 não inclui devicetree, board 68040 ou RTG;
- USB host, USB MSC, BTStack e áudio HDMI são comuns às seis imagens;
- os artefatos RTG são publicados separadamente da imagem, junto de seu
  checksum, apenas nos perfis compatíveis.

## Launcher: política pretendida para releases

O launcher está habilitado por padrão no build (`BELLATRIX_LAUNCHER=1`) e,
portanto, também está presente nas releases enquanto o workflow não sobrescrever
essa opção. A direção pretendida é mantê-lo habilitado, mas tornar o boot sem
pendrive completamente silencioso na saída de vídeo:

- sem dispositivo USB MSC inserido, não desenhar logo, tela de inicialização,
  mensagem de varredura ou aviso de ausência de mídia;
- após uma detecção limitada e não bloqueante, continuar diretamente para o
  boot normal;
- manter diagnóstico da ausência/timeout apenas no log serial;
- mostrar a interface de seleção somente quando houver pendrive pronto com
  mídia suportada (`ADF`, `HDF` ou `ISO`);
- preservar o acesso às telas modais em runtime e o fluxo Bluetooth definido
  pelo launcher;
- aplicar o mesmo comportamento em single-core e multicore.

Esse refinamento ainda não foi implementado. Até ele ser validado em Raspberry
Pi real, o workflow não deve tratar “launcher habilitado” como garantia de boot
visualmente silencioso sem pendrive. O trabalho está rastreado em
`AI_context/issues/ISSUE-0067.md`.

## Pontos de configuração

- `scripts/build.sh`: traduz `BELLATRIX_RELEASE_PROFILE` para opções CMake;
- `cmake/bellatrix-variant.cmake`: seleciona os boards de forma independente;
- `.github/workflows/release-images.yml`: instancia os seis artefatos e valida
  sua composição.
