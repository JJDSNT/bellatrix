# Issue: USB Host — CherryUSB + DWC2 na BCM2837

## Contexto

Bellatrix usa CherryUSB como USB host stack, com backend DWC2 nativo
(`src/io/usb/usb_hc_bellatrix.c`) para o controlador USB do Raspberry Pi 3B
(BCM2837). O objetivo é enumerar dispositivos USB HID (teclado/mouse) para input.

## Arquitetura

### Stack
```
BellatrixMachine (Core 3)
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

## Estado Atual

### QEMU — Funcionando
- Enumera dispositivos USB (teclado, hub)
- Setup packets corretos (LE bounce buffer)
- Descriptor parsing sem endianness bugs
- `device_connected + device_configured` confirmados

### Pi 3B Hardware — Status Incerto
**Sprint 43** regenerou patch com msleep fix + TX FIFO flush mas não confirmou
em hardware real. Esperado que:
- msleep fix resolve reset USB inválido
- TX FIFO flush resolve timing PENCHNG vs canal arm

**Próximo experimento**:
1. Flash com Sprint 42+43 changes
2. Verificar no log: `port enable speed=1 hcfg=0x00000005` (FSLSS set)
3. Verificar: TX FIFO flush log após PENCHNG
4. Verificar: SOF kicks restauram HCDMA ao alias base
5. Alvo: `out irq ch=0 hcint=0x00000021` (XFRC+CHH) → primeira transação completa

### Pendente
- Confirmação em hardware do msleep fix
- HID class driver para input (após enumeração funcionar)
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
- `src/io/usb/usb_osal_bellatrix.c` — OSAL shim (msleep, sem)
- `src/io/usb/usb_hid_bellatrix.h` — HID class integration (stub)
- `external/cherryusb/` — submodule
- `patches/0004-bellatrix-cherryusb-dwc2-host.patch` — patches CherryUSB
- `src/host/raspi3/time.h` — `raspi3_delay_us` para msleep
- `external/aros/arch/arm-native/soc/broadcom/2708/usb/usb2otg/` — referência AROS
