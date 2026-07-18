---
id: ISSUE-0067
title: "Launcher de release: boot visualmente silencioso quando não há pendrive"
status: ready
priority: high
type: feature
owner: agent
created_at: 2026-07-18
updated_at: 2026-07-18
tags: [launcher, release, usb, msc, boot, ui, bare-metal, multicore]
blockers:
  - "Validação final do fluxo e dos tempos de enumeração exige Raspberry Pi real, com e sem pendrive"
related_files:
  - docs/release_profiles.md
  - .github/workflows/release-images.yml
  - scripts/build.sh
  - src/launcher/launcher.c
  - src/launcher/media_selection.c
  - src/launcher/launcher_ui.c
  - src/io/usb/usb_msc.c
---

# Objetivo

Manter o launcher habilitado nas imagens oficiais, sem interromper nem alterar
visualmente o boot normal quando não há pendrive USB MSC inserido.

# Estado atual

`scripts/build.sh` usa `BELLATRIX_LAUNCHER=1` como default. O workflow de release
não sobrescreve a variável, então as seis imagens atuais compilam o launcher.

O caminho atual não é silencioso. `launcher_run()` inicializa e desenha a UI
antes de saber se há mídia. `media_selection_run()` mostra `Scanning USB
drive...`, espera a enumeração MSC por até 5 segundos e, sem mídia, mostra `No
media on USB drive...` antes de `wait_ack()`. Portanto, habilitar explicitamente
o launcher no workflow hoje apenas congelaria esse comportamento incompleto
como política de release.

# Decisão de produto a implementar

- Launcher permanece compilado em todas as releases, single-core e multicore.
- A ausência de pendrive é o caminho normal, não um erro de UI.
- Sem MSC pronto, o framebuffer não deve ser tocado pelo launcher e o boot deve
  prosseguir automaticamente.
- Timeout/ausência continuam observáveis no serial, sem tela ou espera por tecla.
- A UI de seleção só aparece quando a enumeração encontrou mídia suportada.
- Pendrive presente, mas vazio ou sem `ADF`/`HDF`/`ISO`, deve seguir a mesma
  política silenciosa; o resultado fica apenas no serial.
- As telas modais de runtime e a funcionalidade Bluetooth não devem regredir.

# Direção técnica

Separar descoberta de mídia de apresentação:

1. inicializar input e serviços sem chamar `draw_message()`;
2. fazer uma sondagem MSC limitada, bombeando o reactor sem bloquear
   indefinidamente;
3. enumerar FAT/mídia antes de inicializar ou desenhar a UI;
4. retornar imediatamente quando não houver mídia suportada;
5. inicializar métricas e desenhar o seletor somente após `count > 0`;
6. preservar o fallback de loader do QEMU sem acessar MMIO de áudio/HDMI não
   modelado.

A sondagem não deve introduzir dois owners do USB. Launcher e runtime continuam
usando o reactor/ownership estabelecido em `issue_usb_host_dwc2.md`.

# Critérios de aceite

- [ ] Workflow define explicitamente `BELLATRIX_LAUNCHER=1` para as seis imagens.
- [ ] Pi sem pendrive não exibe nenhum conteúdo do launcher e inicia o guest.
- [ ] Pi com pendrive vazio não exibe launcher e inicia o guest.
- [ ] Pi com `ADF`, `HDF` ou `ISO` exibe o seletor e carrega a seleção.
- [ ] Ausência, timeout e mídia não suportada permanecem registrados no serial.
- [ ] Single-core e multicore passam pelos mesmos cenários.
- [ ] BTStack, HID, USB MSC e modais F11/F12 não apresentam regressão.
- [ ] QEMU/harness continua podendo usar seus caminhos de mídia de desenvolvimento.

# Fora de escopo desta atualização documental

Esta issue não muda ainda o fluxo do launcher nem adiciona
`BELLATRIX_LAUNCHER=1` ao workflow. Isso será feito junto da implementação e dos
testes, para que a configuração explícita represente a política já satisfeita.
