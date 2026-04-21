// src/core/machine.c

#include "core/machine.h"
#include "support.h"

#include <string.h>

static BellatrixMachine g_machine;

/* ---------------------------------------------------------------------------
 * Address decode
 * ------------------------------------------------------------------------- */

static inline bool is_custom_addr(uint32_t addr)
{
    return (addr >= 0x00dff000u && addr <= 0x00dfffffu);
}

static inline bool is_cia_a_addr(uint32_t addr)
{
    return (addr >= 0x00bfe001u && addr <= 0x00bfef01u);
}

static inline bool is_cia_b_addr(uint32_t addr)
{
    return (addr >= 0x00bfd000u && addr <= 0x00bfdf00u);
}

/* ---------------------------------------------------------------------------
 * Internal sync
 * ------------------------------------------------------------------------- */

static inline void machine_publish_ipl(BellatrixMachine *m, uint8_t ipl)
{
    if (ipl > 7) ipl = 7;
    if (ipl != m->current_ipl)
        kprintf("[IPL] %u -> %u\n", (unsigned)m->current_ipl, (unsigned)ipl);
    m->current_ipl = ipl;
    if (m->cpu)
        m->cpu->INT.IPL = ipl;
}

static inline uint8_t machine_compute_ipl(BellatrixMachine *m)
{
    return paula_compute_ipl(&m->paula);
}

static inline void machine_step_components(BellatrixMachine *m, uint32_t ticks)
{
    if (ticks == 0) return;

    agnus_step(&m->agnus, ticks);
    cia_step(&m->cia_a, ticks);
    cia_step(&m->cia_b, ticks);
    paula_step(&m->paula, ticks);
    denise_step(&m->denise, ticks);

    m->tick_count += ticks;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

BellatrixMachine *bellatrix_machine_get(void)
{
    return &g_machine;
}

void bellatrix_machine_init(struct M68KState *cpu)
{
    BellatrixMachine *m = &g_machine;

    memset(m, 0, sizeof(*m));
    m->cpu = cpu;

    bellatrix_memory_init(&m->memory);

    agnus_init(&m->agnus);
    denise_init(&m->denise);
    paula_init(&m->paula);
    cia_init(&m->cia_a, CIA_PORT_A);
    cia_init(&m->cia_b, CIA_PORT_B);

    agnus_attach_denise(&m->agnus, &m->denise);
    agnus_attach_paula(&m->agnus, &m->paula);
    agnus_attach_memory(&m->agnus, &m->memory);

    denise_attach_agnus(&m->denise, &m->agnus);

    paula_attach_agnus(&m->paula, &m->agnus);
    paula_attach_cia_a(&m->paula, &m->cia_a);
    paula_attach_cia_b(&m->paula, &m->cia_b);

    cia_attach_paula(&m->cia_a, &m->paula);
    cia_attach_paula(&m->cia_b, &m->paula);

    m->tick_count = 0;
    machine_publish_ipl(m, 0);
}

void bellatrix_machine_reset(void)
{
    BellatrixMachine *m = &g_machine;

    agnus_reset(&m->agnus);
    denise_reset(&m->denise);
    paula_reset(&m->paula);
    cia_reset(&m->cia_a);
    cia_reset(&m->cia_b);

    m->tick_count = 0;
    machine_publish_ipl(m, 0);
}

/* ---------------------------------------------------------------------------
 * Synchronization
 * ------------------------------------------------------------------------- */

void bellatrix_machine_advance(uint32_t ticks)
{
    machine_step_components(&g_machine, ticks);
    bellatrix_machine_sync_ipl();
}

void bellatrix_machine_sync_ipl(void)
{
    BellatrixMachine *m = &g_machine;
    machine_publish_ipl(m, machine_compute_ipl(m));
}

/* ---------------------------------------------------------------------------
 * Bus protocol
 * ------------------------------------------------------------------------- */

uint32_t bellatrix_machine_read(uint32_t addr, unsigned int size)
{
    BellatrixMachine *m = &g_machine;
    uint32_t value = 0;

    machine_step_components(m, 1);

    if (is_custom_addr(addr)) {
        if (paula_handles_read(&m->paula, addr))
            value = paula_read(&m->paula, addr, size);
        else if (agnus_handles_read(&m->agnus, addr))
            value = agnus_read(&m->agnus, addr, size);
        else if (denise_handles_read(&m->denise, addr))
            value = denise_read(&m->denise, addr, size);
    } else if (is_cia_a_addr(addr)) {
        value = cia_read_reg(&m->cia_a, (uint8_t)((addr >> 8) & 0xF));
    } else if (is_cia_b_addr(addr)) {
        value = cia_read_reg(&m->cia_b, (uint8_t)((addr >> 8) & 0xF));
    }

    bellatrix_machine_sync_ipl();
    return value;
}

void bellatrix_machine_write(uint32_t addr, uint32_t value, unsigned int size)
{
    BellatrixMachine *m = &g_machine;

    machine_step_components(m, 1);

    if (is_custom_addr(addr)) {
        if (paula_handles_write(&m->paula, addr))
            paula_write(&m->paula, addr, value, size);
        else if (agnus_handles_write(&m->agnus, addr))
            agnus_write(&m->agnus, addr, value, size);
        else if (denise_handles_write(&m->denise, addr))
            denise_write(&m->denise, addr, value, size);
    } else if (is_cia_a_addr(addr)) {
        cia_write_reg(&m->cia_a, (uint8_t)((addr >> 8) & 0xF), (uint8_t)value);
    } else if (is_cia_b_addr(addr)) {
        cia_write_reg(&m->cia_b, (uint8_t)((addr >> 8) & 0xF), (uint8_t)value);
    }

    bellatrix_machine_sync_ipl();
}

/* ---------------------------------------------------------------------------
 * Raw access to owned components
 * ------------------------------------------------------------------------- */

Agnus  *bellatrix_machine_agnus(void)  { return &g_machine.agnus; }
Denise *bellatrix_machine_denise(void) { return &g_machine.denise; }
Paula  *bellatrix_machine_paula(void)  { return &g_machine.paula; }
CIA    *bellatrix_machine_cia_a(void)  { return &g_machine.cia_a; }
CIA    *bellatrix_machine_cia_b(void)  { return &g_machine.cia_b; }
