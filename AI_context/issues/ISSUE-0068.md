---
id: ISSUE-0068
title: "UX de boot: splash screen, progresso, early boot menu e warm reset"
status: backlog
priority: medium
type: feature
owner: agent
created_at: 2026-07-19
updated_at: 2026-07-19
tags: [ux, ui, boot, splash-screen, launcher, reset]
blockers: []
related_files:
  - src/launcher/launcher.c
  - src/launcher/launcher_ui.c
  - src/launcher/media_selection.c
  - docs/release_profiles.md
---

# Resumo

Planejar e implementar uma experiência visual integrada para o início e o
reinício do Bellatrix, com splash screen, barra de progresso, acesso a um menu
de early boot e warm reset previsível.

# Problema

O fluxo de boot ainda não oferece uma apresentação visual contínua nem um ponto
de entrada claro para opções anteriores ao guest. Também falta definir como um
warm reset deve retornar ao fluxo de boot sem exigir power cycle e sem deixar
subsystems ou estado visual inconsistentes.

Esses recursos compartilham estado, input e transições de tela. Tratá-los como
uma única frente de UX evita que splash, launcher, menu e reset adotem contratos
incompatíveis.

# Objetivo

Oferecer um boot legível e responsivo que:

- mostre identidade visual e progresso real enquanto o sistema inicializa;
- permita abrir um menu de early boot dentro de uma janela curta e claramente
  comunicada;
- mantenha o caminho normal automático quando não houver interação;
- permita warm reset e restaure o sistema a um estado de boot conhecido.

# Escopo proposto

## Splash screen e loading bar

- Definir layout, resolução/fallback e identidade visual da splash.
- Associar a barra a marcos reais de inicialização, evitando progresso apenas
  baseado em tempo.
- Definir comportamento quando um estágio demora, falha ou não fornece
  progresso granular.
- Preservar diagnóstico no serial e um fallback funcional quando vídeo não
  estiver disponível.

## Early boot menu

- Definir tecla, botão ou combinação para entrada e a janela de captura.
- Reutilizar os componentes visuais e de input do launcher quando apropriado.
- Prever inicialmente opções como continuar o boot, selecionar mídia/perfil e
  solicitar reset, sem tornar todas obrigatórias na primeira implementação.
- Garantir timeout e continuação automática sem input.

## Warm reset

- Definir a semântica: quais cores, devices, filas, interrupções, memória e
  estados do guest são reinicializados ou preservados.
- Encerrar ou reinicializar owners de recursos na ordem correta antes de voltar
  ao fluxo de boot.
- Evitar framebuffer congelado, input residual e inicialização duplicada de
  USB, Bluetooth, storage ou áudio.
- Manter hard reset/power cycle como fallback quando warm reset seguro não for
  possível.

# O que foi feito

- Requisitos iniciais de produto e UX registrados.
- Os três recursos relacionados foram agrupados em uma frente única para que
  compartilhem o mesmo contrato de boot, input e reset.

# O que falta fazer

- Mapear o fluxo atual de boot e os owners de cada subsystem.
- Definir os marcos observáveis usados pela barra de progresso.
- Especificar wireframe, assets, input e timeout do early boot menu.
- Especificar e revisar o contrato técnico de warm reset.
- Dividir a implementação em incrementos testáveis para harness/QEMU e hardware.
- Implementar, testar e documentar o comportamento final.

# Decisões tomadas

- A barra de progresso deve refletir trabalho real sempre que houver marcos
  disponíveis.
- A ausência de input mantém o boot automático como caminho principal.
- Warm reset faz parte do contrato da experiência de boot, não apenas de uma
  ação isolada da interface.

# Critérios de aceite

- [ ] Splash aparece sem atrasar materialmente o boot normal.
- [ ] Barra de progresso avança por estágios reais e não regride visualmente.
- [ ] Falhas ou estágios demorados permanecem diagnosticáveis pelo serial.
- [ ] Early boot menu pode ser aberto por input documentado e possui timeout.
- [ ] Sem input, o sistema segue automaticamente para o boot normal.
- [ ] Warm reset retorna ao fluxo definido sem exigir power cycle.
- [ ] Após warm reset não há input residual, owner duplicado, IRQ pendente ou
      estado visual inválido.
- [ ] O fluxo funciona nos perfis single-core e multicore aplicáveis.
- [ ] Há cobertura no harness/QEMU para a lógica testável e validação final em
      Raspberry Pi real.
- [ ] A documentação de usuário e de arquitetura descreve inputs, transições e
      limitações.

# Observações

A implementação pode ser desmembrada em sub-issues após o levantamento técnico,
mantendo esta issue como tracker da experiência completa.

# Log de execução

- 2026-07-19: issue criada para registrar a futura frente de UX de boot.
