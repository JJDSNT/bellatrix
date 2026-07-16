# BT Scan + Pairing + HID Host — BCM43430A1

> **Status atual (2026-07-16, corrige a seção abaixo):** pairing e conexão
> HID estão implementados e comprovados (scan, pareamento, reconexão
> automática, input chegando ao Amiga) — ver
> `AI_context/issues/ISSUE-0059.md` e `AI_context/consolidated/issue_bluetooth.md`.
> A descrição de `src/launcher/launcher.c` como dono da UI de scan também é
> histórica: o launcher foi reduzido a coordenador e as telas de scan/mídia
> foram separadas em `btscan.c`/`media_selection.c`/`launcher_ui.c`
> (commit `450c183`). Único item pendente: validação em hardware real,
> adiada por decisão do usuário.

## Status: EM PROGRESSO (2026-06-26) — histórico, ver nota acima

Scanning funcionando em hardware. HID host implementado e compilando. Pairing
e conexão HID ainda não validados em hardware — esse é o próximo passo do
subsistema BT.

## Estado: HID host implementado e compilando; pendente teste em hardware

---

## Sprint atual: HID host completo (auto-connect + dispatcher)

### Arquitetura multi-source (intencional)

`bellatrix_machine_keyboard_rawkey()`, `bellatrix_machine_mouse_motion/button()`,
`bellatrix_machine_joystick_*()` acumulam de qualquer fonte (USB ou BT) no mesmo
ring/acumulador — não há arbitragem necessária. BT e USB simplesmente escrevem
no mesmo destino.

---

### Roteamento HID

- **Teclado** → `bt_hid_handle_keyboard_report()` → `hid_usage_to_amiga_raw()` → CIA-A SDR
- **Mouse/gamepad** → `bt_hid_handle_mouse/joystick_report()` → BellatrixControllerPortState (porta 0)

Mapeamento HID→rawkey Amiga extraído para `src/io/hid/hid_amiga_map.h`,
compartilhado por USB (`usb_hid_bellatrix.c`) e BT (`bt_hid.c`).

---

### Fluxo de conexão

1. `bt_setup_hci_main()` chama `l2cap_init()`, `sdp_init()`, `hid_host_init()`,
   `hci_set_link_key_db(btstack_link_key_db_memory_instance())`, `bt_hid_init()`
   e registra `hid_packet_handler`.
2. Ao atingir `HCI_STATE_WORKING`, itera `bt_pairs_get()` e chama
   `hid_host_connect(addr, HID_PROTOCOL_MODE_BOOT, &cid)` para cada pair.
3. `hid_packet_handler()` trata:
   - `HID_SUBEVENT_CONNECTION_OPENED` → registra cid→device_type na tabela
     `s_hid_cid_table` (lookup via `bt_pairs_get()` por endereço MAC)
   - `HID_SUBEVENT_REPORT` → despacha para `bt_hid_handle_keyboard/mouse/joystick_report()`
     com base no device_type; fallback por tamanho do report se tipo desconhecido
   - `HID_SUBEVENT_CONNECTION_CLOSED` → `bt_hid_release_all()` + unregister

---

### BTPAIRS.TXT

Formato: `CL K AA:BB:CC:DD:EE:FF nome\r\n`
- Campo 1: transporte (`CL`=Classic, `LE`=LE, `DM`=dual)
- Campo 2: tipo (`K`=keyboard, `M`=mouse, `J`=joystick, `?`=unknown)
- Criado pelo cmake como placeholder de 512 bytes (igual ao BTSCAN.TXT)
- `bt_pairs_load()` / `bt_pairs_serialise()` sem dependência de libc
- `fat32_overwrite_in_place()` requer que o arquivo pré-exista no SD

---

### UI da tela de scan

- `bt_draw_scan_rows()`: `>` cursor, `[P]` pareado, `[*]` HID marker, linha
  selecionada destacada com COL_CURSOR_BG
- Teclas: UP/DOWN=mover, ENTER=toggle pair (add/remove), DEL=remove+salvar, ESC=boot
- Rodapé esquerdo: `build:Jun 13 2026 HH:MM:SS BT USB` (tag de identificação de binário)

---

## Problemas de scan resolvidos (sprints anteriores)

### rxq=330/1220 — LE scan causa overflow do FIFO PL011

BCM43430A1 empacota todos os LE adverts em um único evento de até 1220 bytes.
**Fix:** LE scan desativado. `bt_scan_enter_le()` = pausa de 2 s entre inquiries.
**TODO(LE):** Reabilitar quando FIFO PL011 puder ser drenado por interrupção.

### rxq=251/252 — HCI_Read_Local_Name trava init após recovery

Firmware BCM43430A1 envia 251 de 252 bytes do Command Complete.
**Fix:** `#define ENABLE_AIROC_DOWNLOAD_MODE` em `btstack_config.h`.

### Timers HCI disparando durante re-init

**Fix:** `bt_scan_notify_recovery()` cancela timers e seta `s_phase = SCAN_WAIT_STACK`.

---

## Arquivos modificados/criados

| Arquivo | Papel |
|---|---|
| `src/io/bluetooth/bt_host.c` | HID host init, connect on WORKING, hid_packet_handler |
| `src/io/bluetooth/bt_hid.c/.h` | Dispatcher boot-protocol keyboard/mouse/joystick |
| `src/io/bluetooth/bt_pairs.c/.h` | Load/save BTPAIRS.TXT, type_from_cod, serialise |
| `src/io/bluetooth/bt_scan.c/.h` | LE desativado, stall counter, recovery |
| `src/io/bluetooth/btstack_config.h` | ENABLE_AIROC_DOWNLOAD_MODE, L2CAP/HID limits |
| `src/io/hid/hid_amiga_map.h` | HID→rawkey Amiga, compartilhado USB+BT |
| `src/io/usb/usb_hid_bellatrix.c` | Usa hid_amiga_map.h (thin wrapper) |
| `src/launcher/launcher.c` | Tela scan: cursor, pairs UI, load/save BTPAIRS.TXT |
| `cmake/bellatrix-variant.cmake` | l2cap_signaling, hid_host, sdp_*, link_key_db, bt_hid |

---

## Próximo passo

Teste em hardware:
1. Colocar binário no SD (`emu68/install-bellatrix-rigel-musashi/Emu68.img`)
2. Na tela de scan: selecionar teclado/mouse/gamepad, pressionar ENTER para parear
3. Rebootar — o sistema deve conectar automaticamente via `hid_host_connect()`
4. Verificar no log BT: `HID connected: ... type=K/M/J cid=0x...`
5. Testar input do teclado (CIA-A) e mouse/gamepad (controller port 0)
