# Issue: Bluetooth — BCM43430A1 (Pi 3B On-Board)

## Contexto

Tentativa de bring-up do Bluetooth on-board do Raspberry Pi 3B (BCM43430A1)
para uso como canal de input alternativo. Investigado no Sprint 30.

## Estado da Investigação

### O que foi implementado
- PatchRAM BCM43430A1 embeddado no build (download e cache local)
- Bootstrap bloqueante no boot (antes do boot principal)
- Split do bootstrap em fases:
  1. Controller reset / settle
  2. BCM phase 1 (HCI reset sobre H4 raw)
  3. H5 main transport setup
- Migração de mini-UART para **PL011** (UART0) como transport Bluetooth
  (evidência de que mini-UART não é a seleção correta para Pi 3B BT)
- PL011 route switching: console header ↔ BT internal path
- UART trace dump em memória (para observar fase 1 mesmo com console handoff)
- `BT_REG_EN` reset + settle

### Observações em Hardware

- PatchRAM visível e correto no boot
- `BT_REG_EN` reset funciona
- BCM phase 1 transmite `HCI Reset` corretamente (TX path ativo)
- **Nenhuma resposta válida recebida durante a janela de phase 1**
- `0xFF` observado após timeout — não considerado resposta válida (chega só após restore do console)

### Hipóteses Não Validadas

- Pi 3B Bluetooth pode precisar de handling adicional de clock ou routing interno
  além do simples pin mux PL011 handoff
- Clocking assumptions da firmware RPi vs DT (`/chosen/...`) podem diferir da
  configuração Bellatrix bare-metal
- H5 transport setup (fase 3) não foi alcançado

## Status

**Bloqueado em BCM phase 1 bootstrap** — sem resposta válida do controller.

O issue do Bluetooth foi parcialmente suspenso quando USB host (CherryUSB/DWC2)
se tornou o caminho principal para input físico. USB HID é mais portátil e não
requer o protocolo propietário BCM.

## Próximos Passos (Se Retomado)

1. Verificar se Pi 3B precisa de `core_freq=250` ou similar em `config.txt` para
   PL011 funcionar como BT UART no Bellatrix bare-metal
2. Rever timing de settle após `BT_REG_EN` reset
3. Comparar com raspberrypi/firmware reference para sequência correta de init do BCM
4. Somente após phase 1 funcionar → avançar para H5 e pairing

## Arquivos Relevantes
- `src/io/bluetooth/` — stack BTstack port
- `src/host/raspi3/pl011_backend.c` — PL011 com route switching BT/console
- Script de build: `BELLATRIX_BT=1` (toggle, se existir)
