# Issue: USB Host — CherryUSB + DWC2 na BCM2837

> Decisão 2026-07-11: Core 0 é o owner permanente de DWC2/CherryUSB; Core 3
> permanece reservado. Launcher e runtime são fases do mesmo owner. O desenho
> Core 3 descrito abaixo é histórico e pode orientar uma extração futura.

## Estado canônico após ISSUE-0045 (2026-07-11)

O crash launcher/runtime revelou uma fronteira maior que o bug pontual: a mesma
instância CherryUSB/DWC2 era bombeada por Core 0 e Core 3. O gate transitório
`launcher_owns_usb` foi removido depois do cutover. A arquitetura implementada
é **um serviço permanente, um owner, múltiplos clientes**.

- Multicore: Core 0 possui DWC2, CherryUSB, HID, lifecycle e block I/O USB.
- Single-core: a mesma API progride cooperativamente no executor único.
- Launcher e supervisor atravessam o mesmo `bellatrix_runtime_io_step()`.
- Launcher e máquina mudam o contexto de despacho, não o owner do USB.
- IRQ DWC2 futura termina no Core 0; poll e IRQ
  ou gateway mínimo são mecanismos de ativação e não alteram ownership.
- `launcher_input` atual não é cross-core; a migração exige SPSC real.
- MSC permanece síncrono. Stalls de ~625 ms foram medidos no launcher; acesso
  runtime será o gatilho para request/completion ou worker no Core 3.

O trabalho completo está em `AI_context/issues/ISSUE-0045.md` e a arquitetura
pública em `docs/host_reactor.md`.

## Contexto

Bellatrix usa CherryUSB como USB host stack, com backend DWC2 nativo
(`src/io/usb/usb_hc_bellatrix.c`) para o controlador USB do Raspberry Pi 3B
(BCM2837). O objetivo é enumerar dispositivos USB HID (teclado/mouse) para input.

## Arquitetura

### Stack
```
Bellatrix Host Reactor (Core 0)
  → core_io_step → usb_hc_bellatrix (DWC2 polling)
     → CherryUSB host core (usbh_core.c)
        → Hub driver (usbh_hub.c) — detecta e enumera
        → HID class driver — recebe input
```

### Camadas
- `src/io/usb/usb_hc_bellatrix.c` — DWC2 host controller, **substitui** `external/cherryusb/port/dwc2/usb_hc_dwc2.c`
- `src/io/usb/usb_osal_bellatrix.c` — CherryUSB OSAL shim (semaphores, sleep)
- `external/cherryusb/` — submodule, patches via `0004-bellatrix-cherryusb-dwc2-host.patch`

## Big-Endian Considerations

Bellatrix roda AArch64 em **big-endian** (`EMU68_HOST_BIG_ENDIAN=1`).
CherryUSB e DWC2 assumem little-endian em todos os lugares.

### Soluções implementadas
- `rd32le(addr)` / `wr32le(addr, val)` — accessors MMIO que swappam bytes
- Setup packet bounce buffer: `setup_bounce_bufs[bus][ch][32]` serializa
  `struct usb_setup_packet` em LE explícito byte a byte
- `usbh_read_le16()` — helper para ler campos LE de descriptors
- Descriptors (device, config, interface, endpoint) parseados de raw bytes
- Hub port status: `wPortStatus/wPortChange` como LE explícito (não `memcpy` em struct)
- Hub descriptor: copia prefix para struct, parseia de raw bytes
- `.usbh_class_info` fallback registry — link order expõe sentinels em reverse

## Histórico de Bugs e Fixes

### Sprint 30: Scaffold
CherryUSB adicionado como submodule. OSAL shim, config header, build toggle.
Status: estruturalmente integrado, sem enumeração real.

### Sprint 31: Root hub não enumerava

**Problema**: Hub thread acordava mas não reset/enumera.
**Root cause**: Endianness no bitmap de port change e port status packing.

**Fixes**:
- Hub interrupt bitmap: assemblado explicitamente em LE
- `wPortStatus/wPortChange`: packing como LE fields
- `GET_STATUS` response: decode explícito de 4 bytes
- DWC2 register wrappers: LE read/write
- Root hub: seed `C_CONNECTION` inicial no boot

**Resultado QEMU**: root hub acorda, debounce, `device_connected`, `device_reset`

### Sprint 31: OSAL `sem_take` não-bloqueante
**Problema**: `usb_osal_sem_take()` era não-bloqueante → URBs síncronos falhavam
antes de `USBH_IRQHandler()` ter chance de servir.
**Fix**: `usb_osal_sem_take()` pumpa `USBH_IRQHandler(0)` enquanto espera.

### Sprint 32: Reset port real hardware
**Problema**: Pi 3B nunca completava reset (PENA nunca assertava).
**Fix**: `USBH_IRQHandler()` pumpeado durante wait de reset e PENA.
**Resultado**: hardware completa reset, `hprt=0x0002140d`.

### Sprint 33: HCINT não propagava
**Hipótese**: `GINTSTS.HCINT` não chegava para hardware BCM2837.
**Teste**: fallback scan de canais ativos quando `GINTSTS.HCINT=0`.
**Resultado**: hipótese invalidada. Canal nunca iniciava nem com fallback.

### Sprint 34: PHY mode mismatch
**Descoberta**: Bellatrix forçava `phy_type = DWC2_PHY_TYPE_PARAM_FS` mas BCM2837
reporta HS/UTMI (`GSNPSID 4f54280a`, GHWCFG2/3/4 HS).
**Fix**: mudado para `DWC2_PHY_TYPE_PARAM_UTMI`.

### Sprint 35: PHY override quebra QEMU
**Problema**: UTMI fixo faz QEMU assertar ("hs not supported").
**Fix**: PHY selecionado de `GHWCFG2` em runtime:
```c
// FS por default; upgrade para UTMI quando GHWCFG2 advertisa HS
if (ghwcfg2_hs_utmi || ghwcfg2_hs_utmi_ulpi)
    cfg.phy_type = DWC2_PHY_TYPE_PARAM_UTMI;
```

### Sprint 36: DMA bus aliasing BCM
**Hipótese**: BCM2837 DWC2 precisa de bus alias para DMA.
**Fix (UTMI path)**: `HCDMA = mmu_virt2phys(buf) | 0xC0000000`
QEMU: endereço físico normal.

### Sprint 37: HCFG.FSLSPCS não programado
**Problema**: `HCFG=0x00000000` em hardware após PENA (vs `HCFG=0x00000201` em QEMU).
**Fix**: programar `FSLSPCS` baseado na velocidade real do port após PENCHNG, independente de `phy_type`.

### Sprint 38: FSLSS bit faltando
**Problema**: `HCFG=0x00000001` (sem `FSLSS` bit).
**Fix**: setar `FSLSS` para FS/LS devices (bit 2 de HCFG).

### Sprint 39: Eliminação de hipóteses
Confirmados como não-causas:
- Setup endianness / request payload
- Root-hub wakeup/debounce
- Port reset/enable sequencing
- QEMU enumeration
- DMA address form (virt, phys, BCM alias)
- Post-reset `FSLSPCS/FSLSS` programming

**Diagnóstico remanescente**: BCM2837-specific DMA/buffer strategy.
Referência: `external/aros/.../usb2otg/usb2otg_schedule.c` (DMA bus alias + SOF kick).

### Sprint 40: SOF-driven kick
**Insight AROS**: scheduler usa `DMASTARTOFFRAME` para restart/continue channels.
**Fix**: SOF interrupts habilitados; na UTMI path, re-kick canal control parado no SOF se sem HCINT.

### Sprint 41: Bounce buffer + bug de endianness do setup packet

**Root cause do stall QEMU**: `usb_setup_packet` armazena `uint16_t` em BE order.
DWC2 enviava struct raw → QEMU recebia bytes BE como LE → `wLength=2048` em vez de 8.

**Fix crítico** (em `usb_hc_bellatrix.c`):
```c
static uint8_t setup_bounce_bufs[CONFIG_USBHOST_MAX_BUS][16][32]
    __attribute__((aligned(32)));

// Em dwc2_control_urb_init (SETUP stage):
bounce[0] = setup->bmRequestType;
bounce[1] = setup->bRequest;
bounce[2] = (uint8_t)(setup->wValue);
bounce[3] = (uint8_t)(setup->wValue >> 8);
bounce[4] = (uint8_t)(setup->wIndex);
bounce[5] = (uint8_t)(setup->wIndex >> 8);
bounce[6] = (uint8_t)(setup->wLength);
bounce[7] = (uint8_t)(setup->wLength >> 8);
usb_dcache_clean((uintptr_t)bounce, 32);
dwc2_chan_transfer(bus, chidx, 0x00, bounce, ...);
```

**Resultado QEMU após Sprint 41**:
```
[USB] event ... event=3 (device_connected)
[USB] event ... event=6 (device_configured)
```
QEMU com `-device usb-kbd` enumera completamente.

### Sprint 42: `usb_osal_msleep` era no-op

**Root cause no Pi 3B**: `usb_osal_msleep()` implementada como `(void)delay`.

**Consequências**:
- `drivebus(1)` sem delay de 200ms → device não pronto
- `usbh_reset_port` sem hold de 100ms → reset USB inválido
- Device nunca reseta para estado Default → ignora SETUP packets silenciosamente

**Fix**:
```c
void usb_osal_msleep(uint32_t delay)
{
    raspi3_delay_us((uint64_t)delay * 1000u);
}
```

**Bug secundário**: SOF kick re-habilitava `CHENA` sem restaurar `HCTSIZ/HCDMA`.
Após primeiro DMA (HCDMA += 8), kicks seguintes enviavam bytes 8-15 do bounce
buffer em vez dos 8 bytes originais.
**Fix**: salvar `saved_hcdma/saved_hctsiz` no arm; restaurar em cada SOF kick.

**Bug terciário (round 2)**: PENCHNG disparava APÓS primeiro arm do canal.
Resultado: canal armado com `HCFG=0x00000000` (HS mode, sem FSLSS). DMA carregava
TX FIFO em modo HS. PENCHNG setava HCFG para FS/FSLSS mas TX FIFO já tinha dados
HS → BCM2837 não conseguia enviar SETUP FS com dados HS no FIFO.

**Fix**: flush TX FIFO em `dwc2_port_irq_handler` após HCFG set:
```c
dwc2_flush_txfifo(bus, 0x10U);   // flush all TX FIFOs após PENCHNG
```
E em `dwc2_halt` antes de CHDIS|CHENA (BCM2837 mantém CHENA com TX FIFO não-vazio):
```c
dwc2_flush_txfifo(bus, 0x10U);
```

### Sprint 43: Patch 0004 divergente + Hub QEMU

**Problema**: `patches/0004` desatualizado; `external/cherryusb` tinha mudanças locais
mais novas. Build relaxado temporariamente. Sprint 43 regenerou patch e restaurou
invariante (`git apply --check` deve passar em subtree limpa).

**Resultado QEMU com patch correto**:
```
interface class=0x09 → hub driver loaded
hub descriptor accepted: 0a 29 08 0a 00 01 00 00 00 ff
Ep=81 Attr=03 Mps=2 Interval=255
Register HUB Class:/dev/hub2
```
QEMU com USB hub agora enumera hub completo.

**Workaround documentado**: `.usbh_class_info` fallback registry necessária por
link order reverso em Bellatrix. Known issue, não crash.

### Sprint 44: Sessão de eliminação sistemática no Pi 3B (HCINT=0)

Sintoma persistente em hardware: porta detecta/reseta/habilita FS, HFNUM avança,
canal arma, mas **nenhuma transação sai no fio** — `HCINT=0`, fila NP TX intocada
(`HNPTXSTS=0x01080080`), sem nenhum bit de erro.

Sequência de hipóteses testadas em hardware (cada uma confirmada via registrador
no log, **nenhuma resolveu sozinha**, todas mantidas por estarem corretas):

1. **Restore da HEAD (`da66367`)** — descoberto que o working tree estava cheio
   de experimentos não commitados (phy_type=FS/PHYSEL=1/slave mode, salvos em
   `/tmp/usb_fs_experiments.patch`). Teste lado-a-lado provou: **o launcher com
   HID só funcionou no QEMU; o driver nunca enumerou em hardware real.**
2. **HCFG clock select (Linux-style)** — `dwc2_apply_port_speed_config` aplicava
   o template STM32 (48MHz+FSLSS) no caminho UTMI. Errado: UTMI roda a 30/60MHz
   e Linux só toca FSLSPclkSel/FSLSS com PHYSEL=1. Fix mantido → `HCFG=0x0`.
3. **GAHBCFG semântica Broadcom/AXI** — bits[4:1] NÃO são HBSTLEN Synopsys neste
   SoC: bits[2:1]=max AXI burst, bit4=wait-for-AXI-writes (confirmado no header
   AROS `usb2otg.h`). Linux usa `ahbcfg=0x10`. Fix mantido → `GAHBCFG=0x31`.
4. **TRDT=9** — turnaround p/ UTMI 8-bit (Linux `dwc2_gusbcfg_init`; default 5 é
   p/ 16-bit). Fix mantido → `GUSBCFG=0x20002400`.
5. **No-core-reset (estilo AROS)** — AROS usb2otg tem o soft reset `#if (0)`'d;
   pulamos o reset no BCM2837 para preservar estado do firmware. Mantido.
6. **Dump ampliado no sem-timeout** — `GOTGCTL/HCFG/HFIR/HPRT/PCGCCTL`. Mostrou
   sessão A-valid OK, sem clock gating, porta enabled — tudo "de manual".

**Observação chave**: na 1ª tentativa o `HCDMA` readback avançava +8 → o fetch
DMA funcionava; o MAC entregava o pacote a um PHY morto. E porta habilitar em
**FS** num Pi 3B (cujo primeiro device é o hub LAN9514, HS) = chirp nunca
aconteceu = transmissor morto. Port enable NÃO prova TX: o MAC dirige reset às
cegas e amostra linestate (sensing passivo funciona sem energia).

### Sprint 44 (cont.): ROOT CAUSE — USB power domain OFF + tag mailbox errada

Reescrito o power-on usando o buffer mailbox **uncached** do Emu68
(`extern uint32_t *FBReq` @ `0xffffff9000001000`) — o buffer estático cacheado
anterior relia o próprio request (por isso o falso "state=0x3"). Com GET→SET→GET
e response codes:

```
get=0x00000000(rc=0x80000000)   ← firmware confirma: USB power OFF
set=0x00000003(rc=0x80000001)   ← SET rejeitado: "error parsing request buffer"
chk=0x00000000(rc=0x80000000)   ← continua OFF
```

**A tag SET_POWER_STATE usada era `0x00028002` (errada) desde o primeiro
experimento. A correta é `0x00028001`.** GET (`0x00020001`) sempre funcionou.
Com o domínio USB sem energia, todo o quadro fecha: sensing passivo OK, FS em
vez de HS, fetch DMA OK, zero pacotes, zero erros.

### Sprint 44 (cont. 2): CONFIRMADO EM HARDWARE — primeira enumeração real

Com a tag correta (`0x00028001`) o power-on funcionou
(`get=0x0 → set=0x1 → chk=0x1`, todos rc=0x80000000) e **pela primeira vez
transações USB reais completaram no Pi 3B**:

- **LAN9514 hub enumerado** (`0424:9514`), hub class em `/dev/hub2`
- Ethernet interna (`0424:ec00`, class 0xff) detectada → `interface_unsupported` (ok)
- **Device low-speed (teclado/mouse) detectado na porta 3 do hub**

Nota: o root port enumerou o LAN9514 em **full-speed** (chirp HS não aconteceu)
— funcional, mas significa LS-over-FS-hub (PRE packets) em vez de splits.

**Nova falha (camada seguinte)**: na 1ª transação para o device LS,
`hcint=0x82` (XACTERR+CHH) e em seguida a **porta raiz foi desabilitada**
(`HPRT ena=0`, `HFNUM=0x3fff` congelado) — todas as transferências seguintes
morrem em timeout e não há recuperação.

**Causa suspeita (fix compilado, aguardando teste)**: sem o core reset, o
GUSBCFG herdou do firmware **SRPCAP+HNPCAP (bits 8,9) e TSDPS (bit 22)** —
`GUSBCFG=0x20402700` — e `GINTSTS.OTGINT` aparece pendente nos dumps de
timeout. Protocolo de sessão OTG ativo em modo host: a sinalização LS dispara
a state machine de sessão e derruba a porta. USPI/Circle limpam esses bits no
init. Fix: clear de SRPCAP|HNPCAP|TSDPS em `dwc2_core_init()`.

### Sprint 44 (final): ✅ USB HID FUNCIONANDO EM HARDWARE

Mais dois fixes nesta sequência:

1. **Supressão de PCDET-com-PENA no HPRT irq handler** — o PCDET gerado pelo
   nosso próprio port reset era servido tarde (pump do sem_take) e re-marcava
   port_csc, fazendo o hub thread derrubar o hub recém-enumerado. Critério
   estrutural: PCDET com PENA=1 só pode ser rastro de self-reset (connect
   genuíno chega com porta desabilitada).
2. **FSLSS=1 no host-init** (não no PENCHNG — FSLSSupp é amostrado quando a
   porta habilita; setar depois chega tarde).

**Resultado em Pi 3B real** (teclado FS `25a7:fa70` combo + pendrive SanDisk
`0781:5567`):
```
[USB-HID] keyboard attached: minor=0 intf=0 ep_in=0x81 mps=8  → /dev/input0
[USB-HID] mouse attached:    minor=1 intf=1 ep_in=0x82 mps=8  → /dev/input1
New device found,idVendor:0781,idProduct:5567 (MSC, class 08/06/50)
```
Cadeia completa: power-on → LAN9514 hub → ethernet ignorada → HID nas duas
interfaces → pendrive enumerado (MSC desabilitado neste build de teste).

**Limitação conhecida**: devices **low-speed** atrás do hub ainda derrubam a
porta raiz (XACTERR + port disable) mesmo com FSLSS=1 desde o init — teclados
LS antigos não funcionam. PRE+LS no DWC2/BCM2837 em porta FS segue sem
solução; alternativas: investigar chirp HS (splits) ou aceitar FS-only.

### Sprint 45: MSC big-endian + input HID de verdade + undervoltage

Commits: `72fabff` (CBW LE), `ddf2bf8` (ODDFRM/single-core/hub-ack),
`f0b2935` (telemetria).

1. **MSC CBW/CSW little-endian** (`usbh_msc.c`, patch 0004) — host BE
   enviava os uint32 do CBW (dSignature/dTag/dDataLength) byte-swapped;
   o device STALLava ambos bulk EPs por CBW inválido (BOT 6.6.1, erro -8).
   Serialização LE explícita resolve; `testunitready`/`readcapacity10`
   passaram em hardware.
2. **ODDFRM invertido** (`usb_hc_bellatrix.c` dwc2_chan_transfer + sof
   kick) — ao armar canal periódico, ODDFRM era setado com a paridade
   *atual* do HFNUM; transfers periódicos executam no frame *seguinte*,
   então tem que ser a paridade oposta (HAL ST: `(HFNUM&1)?0:1`). Com o
   bug, FRMOR (hcint=0x202) em 100% dos interrupt IN — HID enumerava mas
   nunca entregava input. QEMU não modela paridade de frame, mascarando.
   **Após o fix: input de teclado funcionando no launcher.**
3. **Single-core não pisava USB pós-launcher** — `PAL_Runtime_Poll` agora
   chama `bellatrix_runtime_io_step` com throttle ~1ms; e havia *duas*
   definições weak de `bellatrix_runtime_io_step` (stub vazio em
   pal_core.c vs real em core_io.c, linker escolhia por ordem) — a de
   core_io.c agora é strong.
4. **Hub-level change (bitmap bit 0) nunca ACKado** — o LAN9514 reporta
   local-power change no arranque; sem GetHubStatus+ClearHubFeature
   (C_HUB_LOCAL_POWER) o intr EP re-reporta para sempre (era o loop
   bitmap=0x8101). Fix: `_usbh_hub_handle_hub_change` em usbh_hub.c.
5. **Loop attach/detach do MSC era UNDERVOLTAGE** — padrão: drive ready →
   primeira read10 XACTERR → device cai (status=0x0100) → re-enumera em
   loop; Cruzer Blade chegou a subir em modo fallback (PID 5530, 64MB
   fictícios = firmware do stick não carregou da NAND por brownout).
   `GET_THROTTLED` (mailbox 0x30046, `usb_glue_vc_get_throttled`)
   confirmou `0x00050005` = undervoltage ativo. Causa física: Pi
   alimentado através de hub USB com interruptor; ligando direto na
   fonte resolve. Telemetria logada no attach/detach do MSC.

**Workflow do patch 0004**: regenerar com `rtk proxy git diff` dentro de
`external/cherryusb` (o hook rtk filtra `git diff` puro e corrompe o
patch); setup.sh valida com `apply --reverse --check`, então qualquer
edit novo no cherryusb exige regenerar.

## Estado Atual

### Funcionando (Pi 3B hardware)
- Hub LAN9514 + HID keyboard/mouse FS com **input real** no launcher
- MSC: enumeração + scsi_init completos; leituras dependem de
  alimentação adequada (verificar `throttled=0x0` no drive ready)

Build: `BELLATRIX_CPU_BACKEND=musashi BELLATRIX_USBSTACK=1 BELLATRIX_USB_MSC=1
./scripts/build.sh`

### Pendente
- Validar launcher listando/carregando ADF/ISO do pendrive com
  alimentação direta (sem hub no caminho do 5V)
- HID → input do lado Amiga (pós-launcher): ponte relatórios HID →
  keycodes CIA-A ainda não existe
- LS devices atrás do hub (teclados antigos) — sem suporte por ora
- `usb_class_info` sentinels em ordem correta (link order issue)

## Registros Observados em Hardware

```
GSNPSID: 4f54280a        (BCM2837 DWC2)
GHWCFG2: 228ddd50        (HS UTMI+ULPI capable)
GHWCFG3: 0ff000e8
GHWCFG4: 1ff00020
HCFG target: 0x00000005  (FSLSS=1, FSLSPCS=48MHz)
HCDMA: 0xf82b....        (BCM bus alias 0xC0000000 | phys)
```

## Arquivos Relevantes
- `src/io/usb/usb_hc_bellatrix.c` — DWC2 host controller (implementação principal)
- `src/io/usb/usb_glue_dwc2_bellatrix.c` — params PHY + VC mailbox USB power-on
- `src/io/usb/usb_osal_bellatrix.c` — OSAL shim (msleep, sem + register dump no timeout)
- `src/io/usb/usb_hid_bellatrix.h` — HID class integration (stub)
- `external/cherryusb/` — submodule
- `patches/0004-bellatrix-cherryusb-dwc2-host.patch` — patches CherryUSB
- `src/host/raspi3/time.h` — `raspi3_delay_us` para msleep
- `external/aros/arch/arm-native/soc/broadcom/2708/usb/usb2otg/` — referência AROS
