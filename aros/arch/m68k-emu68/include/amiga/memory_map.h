#ifndef AMIGA_MEMORY_MAP_H
#define AMIGA_MEMORY_MAP_H

/* The classic chipset owns this address window, not the host machine. */
#define AMIGA_CHIP_RAM_BASE       0x00000000UL
#define AMIGA_CHIP_RAM_SIZE       0x00200000UL
#define AMIGA_CHIP_RAM_ALLOC_BASE 0x00001000UL
#define AMIGA_CHIP_RAM_ALLOC_SIZE \
    (AMIGA_CHIP_RAM_SIZE - AMIGA_CHIP_RAM_ALLOC_BASE)

#endif
