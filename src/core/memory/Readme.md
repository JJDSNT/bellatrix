src/core/memory/
├── memory.c              # API geral de leitura/escrita
├── memory.h
├── memory_map.c          # decode de regiões
├── memory_map.h
├── chip_ram.c            # Chip RAM
├── chip_ram.h
├── fast_ram.c            # Fast RAM / Zorro RAM futura
├── fast_ram.h
├── overlay.c             # overlay ROM/RAM no endereço 0
├── overlay.h
├── autoconfig.c          # futuro Zorro II/III Autoconfig
├── autoconfig.h

CPU / Chipset / DMA
        ↓
    memory_read()
    memory_write()
        ↓
    memory_map_decode()
        ↓
Chip RAM / ROM / Fast RAM / Zorro / Custom Chips

uint8_t  mem_read8(addr);
uint16_t mem_read16(addr);
uint32_t mem_read32(addr);

void mem_write8(addr, value);
void mem_write16(addr, value);
void mem_write32(addr, value);

0x000000-0x1FFFFF  Chip RAM / ROM overlay
0x00DFF000-0x00DFFFFF  Custom chips
0x00BFD000-0x00BFEFFF  CIA
0x00E80000-0x00EFFFFF  Zorro II config
0x40000000+            Zorro III / Fast RAM futura

reads em 0x000000 podem ver ROM ou RAM dependendo do OVL
writes em 0x000000 sempre vão para Chip RAM