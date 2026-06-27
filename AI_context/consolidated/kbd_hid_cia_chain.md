# Teclado USB HID → CIA-A: cadeia completa (single-core)

**Status:** funcional, validado em bare metal (RPi3 + DiagROM). Commit: `kbd: USB HID → CIA-A funcional em single-core`.

## Sintoma original
Launcher recebia teclas (lê HID direto); DiagROM não. Depois do primeiro fix,
teclas chegavam atrasadas: "selecionei uma coisa e apareceu outra" (fila com
backlog de 3–4 eventos).

## Quatro causas-raiz (todas reais, em cadeia)

1. **`io.running` nunca setado** — `bellatrix_init()` chamava `usb_host_init`
   direto em vez de `core_io_init(&g_runtime.io, machine)`. `core_io_step()`
   gateia em `core->running` e virava no-op silencioso após o launcher.
2. **`core_io.c` fora do build** — o cmake fazia `REMOVE_ITEM` de todo
   `runtime/*.c` e só readicionava `core_chipset.c`. O símbolo fraco
   `bellatrix_runtime_io_step` (no-op em `pal_core.c`) era linkado no lugar
   do forte. *Isso invalidou o "fix de símbolo forte" do sprint anterior —
   nunca compilou.* Fix: `core_io.c` em `cmake/bellatrix-variant.cmake`.
3. **Loop musashi sem ponto de serviço de IO** — `PAL_Runtime_Poll()` só era
   chamado de `bellatrix_bus_access` (hook de fault do JIT Emu68). O loop
   musashi (`bellatrix_run_selected_cpu_backend`) não passa por lá; Poll
   agora vive no próprio loop.
4. **SDR sem fila** — `cia_serial_receive_byte` descarta byte se `sdr_full=1`.
   Fila `BellatrixKeyboard` + `machine_keyboard_drain_rigel()`.

## Armadilhas de arquitetura descobertas

- **`bellatrix_machine_post_chipset_step()` SÓ roda no caminho multicore**
  (`core_chipset.c:129`, e `core_chipset_init` nunca é chamado em
  single-core → `s_core=NULL` → `host_step` retorna cedo). Qualquer coisa
  colocada lá é morta em single-core. O ponto certo para trabalho por
  avanço de chipset é **`machine_quantum_step()`** em
  `machine_rigel_step.c` (cobre `machine_step_components` E
  `machine_flush_for_bus`) — é onde o serial já era bombeado e onde o drain
  do teclado vive agora.
- **Handshake KDAT obrigatório**: reencher o SDR logo que o ROM o lê perde
  bytes — o ROM escreve no SDR durante o handshake (`cia_serial_write_sdr`
  grava incondicionalmente) e clobbera o byte pendente (key-up do Enter
  perdido, leitura SDR=0x00 observada em hardware). O drain só reenche após
  o pulso CRA SPMODE out→in (`machine_keyboard_on_cia_cra_write`, chamado
  do dispatch de write CIA-A reg 0x0E), com timeout de resend ~143ms
  (507000 CCK) como o controlador 6500/1 real.

## Logs de diagnóstico (permanentes, enxutos)
- `[HID->AMIGA] usage=.. rawkey=..` — tecla USB mapeada (usb_hid_bellatrix.c)
- `[KBD] rawkey=.. queue=N sdr=.. full=..` — enfileiramento (machine_rigel.c)
- `[KBD] CPU read CIA-A SDR=..` — só quando havia byte pendente (consumo
  real pelo ROM; DiagROM faz polling contínuo do SDR, logar sempre afoga).

## Encoding (verificado)
Wire byte = `~((rawkey<<1) | up)`. Ex.: seta-baixo 0x4d → down 0x65, up
0x64; Enter 0x44 → down 0x77, up 0x76. `bellatrix_keyboard_enqueue_raw`
já armazena o wire encoding (mesma transform de `rigel_keyboard_inject`),
então o drain alimenta `cia_receive_sdr` direto.

## Próximo (escolha de objetivo em aberto)
Bluetooth keyboard/mouse deve convergir no MESMO ponto:
`bellatrix_machine_keyboard_rawkey()` — fonte-agnóstico, sem conflito
USB/BT. Mouse não testável pelo usuário (não tem mouse USB).
