#include "memory/memory.h"

#include <string.h>

/*
 * Backend atual de chip RAM no Bellatrix.
 *
 * Isso centraliza o que antes estava espalhado em módulos como Denise.
 * Mais adiante, se você quiser, esse backend pode vir da machine/config
 * em vez de ficar fixo aqui.
 */
#define BELLATRIX_CHIP_RAM_VIRT  ((uint8_t *)0xffffff9000000000ULL)
#define BELLATRIX_CHIP_RAM_SIZE  0x00200000u
#define BELLATRIX_CHIP_RAM_MASK  0x001FFFFFu

/* ------------------------------------------------------------------------- */
/* helpers internos                                                          */
/* ------------------------------------------------------------------------- */

static inline uint32_t chip_addr(const BellatrixMemory *m, uint32_t addr)
{
    return addr & m->chip_ram_mask;
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void bellatrix_memory_init(BellatrixMemory *m)
{
    memset(m, 0, sizeof(*m));

    m->chip_ram      = BELLATRIX_CHIP_RAM_VIRT;
    m->chip_ram_size = BELLATRIX_CHIP_RAM_SIZE;
    m->chip_ram_mask = BELLATRIX_CHIP_RAM_MASK;
}

void bellatrix_memory_reset(BellatrixMemory *m)
{
    /*
     * No estado atual, reset de memória não limpa chip RAM automaticamente.
     * Isso fica sob controle explícito do chamador, se desejado.
     *
     * Mantemos só a configuração estrutural.
     */
    bellatrix_memory_init(m);
}

/* ------------------------------------------------------------------------- */
/* chip RAM access                                                           */
/* ------------------------------------------------------------------------- */

uint8_t bellatrix_chip_read8(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a = chip_addr(m, addr);
    return m->chip_ram[a];
}

uint16_t bellatrix_chip_read16(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a = chip_addr(m, addr);
    uint8_t hi = m->chip_ram[a];
    uint8_t lo = m->chip_ram[(a + 1u) & m->chip_ram_mask];

    /*
     * Chip RAM é M68K big-endian.
     */
    return (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
}

uint32_t bellatrix_chip_read32(const BellatrixMemory *m, uint32_t addr)
{
    uint32_t a = chip_addr(m, addr);
    uint8_t b0 = m->chip_ram[a];
    uint8_t b1 = m->chip_ram[(a + 1u) & m->chip_ram_mask];
    uint8_t b2 = m->chip_ram[(a + 2u) & m->chip_ram_mask];
    uint8_t b3 = m->chip_ram[(a + 3u) & m->chip_ram_mask];

    return ((uint32_t)b0 << 24) |
           ((uint32_t)b1 << 16) |
           ((uint32_t)b2 <<  8) |
           ((uint32_t)b3 <<  0);
}

void bellatrix_chip_write8(BellatrixMemory *m, uint32_t addr, uint8_t value)
{
    uint32_t a = chip_addr(m, addr);
    m->chip_ram[a] = value;
}

void bellatrix_chip_write16(BellatrixMemory *m, uint32_t addr, uint16_t value)
{
    uint32_t a = chip_addr(m, addr);

    m->chip_ram[a] = (uint8_t)(value >> 8);
    m->chip_ram[(a + 1u) & m->chip_ram_mask] = (uint8_t)(value & 0xFFu);
}

void bellatrix_chip_write32(BellatrixMemory *m, uint32_t addr, uint32_t value)
{
    uint32_t a = chip_addr(m, addr);

    m->chip_ram[a] = (uint8_t)(value >> 24);
    m->chip_ram[(a + 1u) & m->chip_ram_mask] = (uint8_t)((value >> 16) & 0xFFu);
    m->chip_ram[(a + 2u) & m->chip_ram_mask] = (uint8_t)((value >>  8) & 0xFFu);
    m->chip_ram[(a + 3u) & m->chip_ram_mask] = (uint8_t)(value & 0xFFu);
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

uint32_t bellatrix_chip_wrap_addr(const BellatrixMemory *m, uint32_t addr)
{
    return chip_addr(m, addr);
}

int bellatrix_chip_is_configured(const BellatrixMemory *m)
{
    return (m->chip_ram != 0 && m->chip_ram_size != 0);
}