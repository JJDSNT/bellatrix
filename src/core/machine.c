// src/core/machine.c

#include "core/machine.h"

#include "chipset/floppy/floppy_drive.h"

#include "debug/btrace.h"
#include "debug/cpu_pc.h"
#include "debug/probe.h"
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
 * Debug helpers
 * ------------------------------------------------------------------------- */

static void machine_debug_init(BellatrixMachine *m)
{
    BellatrixDebug *d = &m->debug;

    probe_init(&d->probe);
    btrace_init(&d->btrace);

    d->enable_probe = true;
    d->enable_btrace = true;

    d->dump_on_watchdog = true;
    d->dump_on_cpu_stop = true;
    d->dump_on_cpu_except = true;
    d->dump_on_ipl_change = false;

    d->probe_last_n = 128;
    d->btrace_last_n = 128;
    d->copper_max_insn = 32;

    d->btrace_reads = true;
    d->btrace_writes = true;
    d->btrace_only_chipset = true;
    d->btrace_addr_lo = 0x00dff000u;
    d->btrace_addr_hi = 0x00dfffffu;
}

static void machine_debug_reset(BellatrixMachine *m)
{
    probe_reset(&m->debug.probe);
    btrace_reset(&m->debug.btrace);
}

static inline uint32_t machine_cpu_pc(const BellatrixMachine *m)
{
    (void)m;
    return bellatrix_debug_cpu_pc();
}

uint32_t bellatrix_debug_cpu_pc(void)
{
    const BellatrixMachine *m = &g_machine;
    if (!m->cpu_backend || !m->cpu_backend->get_pc)
        return 0u;
    return m->cpu_backend->get_pc(m->cpu_backend->ctx);
}

static inline uint16_t machine_vpos(const BellatrixMachine *m)
{
    /*
     * Ajuste aqui se o nome real do campo em Agnus for outro.
     */
    return (uint16_t)m->agnus.beam.vpos;
}

static inline bool machine_btrace_addr_match(const BellatrixMachine *m, uint32_t addr)
{
    const BellatrixDebug *d = &m->debug;

    if (d->btrace_only_chipset)
    {
        return is_custom_addr(addr) || is_cia_a_addr(addr) || is_cia_b_addr(addr);
    }

    return (addr >= d->btrace_addr_lo && addr <= d->btrace_addr_hi);
}

static inline void machine_btrace_read(BellatrixMachine *m,
                                       uint32_t addr,
                                       unsigned int size,
                                       uint32_t value)
{
    BellatrixDebug *d = &m->debug;

    if (!d->enable_btrace || !d->btrace_reads)
    {
        return;
    }

    if (!machine_btrace_addr_match(m, addr))
    {
        return;
    }

    btrace_log_read(&d->btrace,
                    (uint32_t)m->tick_count,
                    machine_cpu_pc(m),
                    addr,
                    size,
                    value);
}

static inline void machine_btrace_write(BellatrixMachine *m,
                                        uint32_t addr,
                                        unsigned int size,
                                        uint32_t value)
{
    BellatrixDebug *d = &m->debug;

    if (!d->enable_btrace || !d->btrace_writes)
    {
        return;
    }

    if (!machine_btrace_addr_match(m, addr))
    {
        return;
    }

    btrace_log_write(&d->btrace,
                     (uint32_t)m->tick_count,
                     machine_cpu_pc(m),
                     addr,
                     size,
                     value);
}

static inline void machine_probe_emit(BellatrixMachine *m,
                                      ProbeEventType type,
                                      uint32_t a,
                                      uint32_t b)
{
    if (!m->debug.enable_probe)
    {
        return;
    }

    probe_emit(&m->debug.probe,
               type,
               (uint32_t)m->tick_count,
               machine_vpos(m),
               a,
               b);
}

/* ---------------------------------------------------------------------------
 * Floppy ↔ CIA-A glue
 * ------------------------------------------------------------------------- */

static void machine_sync_floppy_pra(BellatrixMachine *m)
{
    uint8_t prb = m->cia_b.prb;
    uint8_t ext = m->cia_a.ext_pra;

    bool sel0 = !(prb & 0x08u);
    bool sel1 = !(prb & 0x10u);
    bool sel2 = !(prb & 0x20u);
    bool sel3 = !(prb & 0x40u);
    bool motor_on = !(prb & 0x80u);

    uint8_t sel_mask = (uint8_t)((sel0 ? 0x1u : 0u) |
                                 (sel1 ? 0x2u : 0u) |
                                 (sel2 ? 0x4u : 0u) |
                                 (sel3 ? 0x8u : 0u));

    int selected_drive = -1;

    switch (sel_mask)
    {
    case 0x1:
        selected_drive = 0;
        break;
    case 0x2:
        selected_drive = 1;
        break;
    case 0x4:
        selected_drive = 2;
        break;
    case 0x8:
        selected_drive = 3;
        break;
    default:
        selected_drive = -1;
        break;
    }

    bool selected_df0 = (selected_drive == 0);

    /* Reset lines: active LOW, default HIGH */
    ext |= 0x04u; /* /DSKCHG */
    ext |= 0x10u; /* /TK0    */
    ext |= 0x20u; /* /RDY    */

    int ready = 0;
    int track0 = 0;
    int dskchg = 1;
    int idbit = 1;
    int idmode = 0;

    if (selected_df0)
    {
        ready = floppy_get_ready(&m->df0);
        track0 = floppy_get_track0(&m->df0);
        dskchg = floppy_get_dskchg(&m->df0, motor_on);
        idbit = floppy_get_idbit(&m->df0);

        /* ID mode = motor OFF */
        idmode = !motor_on;

        /* /DSKCHG: active LOW */
        if (idmode)
        {
            if (!idbit)
                ext &= (uint8_t)~0x04u;
        }
        else
        {
            if (!dskchg)
                ext &= (uint8_t)~0x04u;
        }

        /* /TK0: active LOW */
        if (track0)
            ext &= (uint8_t)~0x10u;

        /* /RDY: active LOW
         *
         * Motor OFF (ID mode): pull LOW to signal drive presence.
         * Motor ON:            reflect the ready flag (motor at speed).
         */
        if (idmode || ready)
            ext &= (uint8_t)~0x20u;
    }

    kprintf("[FLOPPY-PRA-SYNC] prb=%02x selmask=%x drive=%d sel0=%d sel1=%d sel2=%d sel3=%d motor=%d ext=%02x ready=%d track0=%d dskchg=%d idmode=%d idbit=%d\n",
            prb,
            sel_mask,
            selected_drive,
            sel0, sel1, sel2, sel3,
            motor_on,
            ext,
            ready,
            track0,
            dskchg,
            idmode,
            idbit);

    cia_set_external_pra(&m->cia_a, ext);
}

static void machine_floppy_update(BellatrixMachine *m)
{
    uint8_t prb = m->cia_b.prb;

    FloppySignals sig;
    sig.selected = !(prb & 0x08u); /* /SEL0 active LOW */
    sig.motor = !(prb & 0x80u);    /* /MTR  active LOW */
    sig.step = !(prb & 0x01u);     /* /STEP active LOW */
    sig.direction = (prb & 0x02u) ? 1 : 0;
    sig.side = !(prb & 0x04u); /* /SIDE active LOW */

    floppy_step(&m->df0, &sig);
    machine_sync_floppy_pra(m);
}

/* ---------------------------------------------------------------------------
 * Internal sync
 * ------------------------------------------------------------------------- */

static inline void machine_publish_ipl(BellatrixMachine *m, uint8_t ipl)
{
    if (ipl > 7)
    {
        ipl = 7;
    }

    if (ipl != m->current_ipl)
    {
        uint32_t pc = machine_cpu_pc(m);

        if (ipl > m->current_ipl && ipl > 0)
        {
            uint32_t vec_off = 0x60u + (uint32_t)ipl * 4u;
            uint32_t chip_val = bellatrix_chip_read32(&m->memory, vec_off);

            kprintf("[IPL-RISE] %u->%u  vec=%03x chip[%03x]=%08x m68k_pc=%08x\n",
                    (unsigned)m->current_ipl, (unsigned)ipl,
                    (unsigned)vec_off, (unsigned)vec_off, (unsigned)chip_val,
                    (unsigned)pc);

            machine_probe_emit(m, PROBE_EVT_IPL_RISE, (uint32_t)ipl, pc);
        }
        else
        {
            uint32_t vec_off = 0x60u + (uint32_t)m->current_ipl * 4u;
            uint32_t chip_val = bellatrix_chip_read32(&m->memory, vec_off);

            kprintf("[IPL-DROP] %u->%u  vec=%03x chip[%03x]=%08x m68k_pc=%08x\n",
                    (unsigned)m->current_ipl, (unsigned)ipl,
                    (unsigned)vec_off, (unsigned)vec_off, (unsigned)chip_val,
                    (unsigned)pc);

            machine_probe_emit(m, PROBE_EVT_IPL_DROP, (uint32_t)ipl, pc);
        }
    }

    m->current_ipl = ipl;
    if (m->cpu_backend && m->cpu_backend->set_ipl)
        m->cpu_backend->set_ipl(m->cpu_backend->ctx, (int)ipl);
}

static inline uint8_t machine_compute_ipl(BellatrixMachine *m)
{
    return paula_compute_ipl(&m->paula);
}

static inline void machine_step_components(BellatrixMachine *m, uint32_t ticks)
{
    uint16_t old_vpos;

    if (ticks == 0)
    {
        return;
    }

    old_vpos = machine_vpos(m);

    agnus_step(&m->agnus, ticks);

    /*
     * CIA runs at E-clock = CPU / 10.  Accumulate fractional ticks to
     * avoid drift on small quanta (e.g. ticks = 1 from bus access path).
     */
    m->cia_tick_acc += ticks;
    {
        uint64_t cia_ticks = m->cia_tick_acc / 10u;
        m->cia_tick_acc %= 10u;
        if (cia_ticks > 0)
        {
            cia_step(&m->cia_a, cia_ticks);
            cia_step(&m->cia_b, cia_ticks);
        }
    }

    paula_step(&m->paula, ticks);
    denise_step(&m->denise, ticks);

    m->tick_count += ticks;

    /* VBL: beam rollover -> SET VERTB in INTREQ once per frame.
     *
     * Do not call paula_irq_raise() here.
     * VBL is not an external level line; it is a custom-chip INTREQ latch bit.
     *
     * The guest clears it by writing INTREQ raw=0x0020.
     * The beam sets it again at the next frame rollover.
     */
    if (machine_vpos(m) < old_vpos)
    {
        paula_write(&m->paula, 0xDFF09Cu, 0x8020u, 2);
        machine_probe_emit(m, PROBE_EVT_VBL, 0x20u, 0);
    }
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

BellatrixMachine *bellatrix_machine_get(void)
{
    return &g_machine;
}

BellatrixDebug *bellatrix_machine_debug(void)
{
    return &g_machine.debug;
}

void bellatrix_machine_init(CpuBackend *cpu_backend)
{
    BellatrixMachine *m = &g_machine;

    memset(m, 0, sizeof(*m));
    m->cpu_backend = cpu_backend;

    bellatrix_memory_init(&m->memory);

    agnus_init(&m->agnus);
    denise_init(&m->denise);
    paula_init(&m->paula);
    cia_init(&m->cia_a, CIA_PORT_A);
    cia_init(&m->cia_b, CIA_PORT_B);
    rtc_init(&m->rtc, RTC_MODEL_OKI);

    agnus_attach_denise(&m->agnus, &m->denise);
    agnus_attach_paula(&m->agnus, &m->paula);
    agnus_attach_memory(&m->agnus, &m->memory);

    denise_attach_agnus(&m->denise, &m->agnus);

    paula_attach_agnus(&m->paula, &m->agnus);
    paula_attach_cia_a(&m->paula, &m->cia_a);
    paula_attach_cia_b(&m->paula, &m->cia_b);
    paula_attach_memory(&m->paula, m->memory.chip_ram, m->memory.chip_ram_size);

    cia_attach_paula(&m->cia_a, &m->paula);
    cia_attach_paula(&m->cia_b, &m->paula);

    floppy_init(&m->df0);
    paula_attach_drive(&m->paula, &m->df0);
    machine_sync_floppy_pra(m); /* set initial /CHNG, /TK0, /RDY on CIA-A ext_pra */

    machine_debug_init(m);

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
    rtc_reset(&m->rtc);

    machine_debug_reset(m);
    machine_sync_floppy_pra(m);

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

/* ---------------------------------------------------------------------------
 * Chipset register dispatch (no tick advance, no btrace — called by read/write)
 * The 68000 has a 16-bit bus; a size-4 access is always two word cycles.
 * bellatrix_machine_read/write split size=4 before calling these helpers so
 * chipset components (Agnus, Paula, Denise, CIA) only ever see size 1 or 2.
 * ------------------------------------------------------------------------- */

static uint32_t machine_dispatch_read(BellatrixMachine *m, uint32_t addr, unsigned int size)
{
    uint32_t value = 0;

    if (is_custom_addr(addr))
    {
        if (paula_handles_read(&m->paula, addr))
            value = paula_read(&m->paula, addr, size);
        else if (agnus_handles_read(&m->agnus, addr))
            value = agnus_read(&m->agnus, addr, size);
        else if (denise_handles_read(&m->denise, addr))
            value = denise_read(&m->denise, addr, size);
    }
    else if (is_cia_a_addr(addr))
    {
        uint8_t reg = (uint8_t)((addr >> 8) & 0x0F);

        value = cia_read_reg(&m->cia_a, reg);

        if (reg == 0u) /* CIA-A PRA */
        {
            kprintf("[CIAA-PRA-R-PC] pc=%08x addr=%06x size=%u val=%02x pra=%02x ddra=%02x ext=%02x\n",
                    (unsigned)machine_cpu_pc(m),
                    (unsigned)addr,
                    size,
                    (unsigned)(value & 0xFFu),
                    (unsigned)m->cia_a.pra,
                    (unsigned)m->cia_a.ddra,
                    (unsigned)m->cia_a.ext_pra);
        }

        machine_probe_emit(m, PROBE_EVT_CIA_READ, addr, value);
    }
    else if (is_cia_b_addr(addr))
    {
        uint8_t reg = (uint8_t)((addr >> 8) & 0x0F);

        value = cia_read_reg(&m->cia_b, reg);

        if (reg == 0u || reg == 1u) /* CIA-B PRA/PRB */
        {
            kprintf("[CIAB-R-PC] pc=%08x addr=%06x reg=%u size=%u val=%02x pra=%02x prb=%02x ddra=%02x ddrb=%02x extA=%02x extB=%02x\n",
                    (unsigned)machine_cpu_pc(m),
                    (unsigned)addr,
                    (unsigned)reg,
                    size,
                    (unsigned)(value & 0xFFu),
                    (unsigned)m->cia_b.pra,
                    (unsigned)m->cia_b.prb,
                    (unsigned)m->cia_b.ddra,
                    (unsigned)m->cia_b.ddrb,
                    (unsigned)m->cia_b.ext_pra,
                    (unsigned)m->cia_b.ext_prb);
        }

        machine_probe_emit(m, PROBE_EVT_CIA_READ, addr, value);
    }

    return value;
}

static void machine_dispatch_write(BellatrixMachine *m, uint32_t addr, uint32_t value, unsigned int size)
{
    if (is_custom_addr(addr))
    {
        uint16_t old_intreq = m->paula.intreq;

        if (paula_handles_write(&m->paula, addr))
            paula_write(&m->paula, addr, value, size);
        else if (agnus_handles_write(&m->agnus, addr))
            agnus_write(&m->agnus, addr, value, size);
        else if (denise_handles_write(&m->denise, addr))
            denise_write(&m->denise, addr, value, size);

        if ((addr & 0x00fffffeu) == 0x00dff09au)
            machine_probe_emit(m, PROBE_EVT_INTENA_WRITE, value, m->paula.intena);

        if ((addr & 0x00fffffeu) == 0x00dff09cu)
        {
            if (m->paula.intreq != old_intreq)
            {
                if (m->paula.intreq > old_intreq)
                    machine_probe_emit(m, PROBE_EVT_INTREQ_SET, value, m->paula.intreq);
                else
                    machine_probe_emit(m, PROBE_EVT_INTREQ_CLR, value, m->paula.intreq);
            }
        }
    }
    else if (is_cia_a_addr(addr))
    {
        cia_write_reg(&m->cia_a, (uint8_t)((addr >> 8) & 0x0F), (uint8_t)value);
        machine_probe_emit(m, PROBE_EVT_CIA_WRITE, addr, value);
    }
    else if (is_cia_b_addr(addr))
    {
        static const char *const cia_reg_names[16] = {
            "PRA", "PRB", "DDRA", "DDRB",
            "TALO", "TAHI", "TBLO", "TBHI",
            "TODLO", "TODMID", "TODHI", "UNUSED",
            "SDR", "ICR", "CRA", "CRB"
        };

        uint8_t cia_b_reg = (uint8_t)((addr >> 8) & 0x0Fu);

        kprintf("[CIAB-W] pc=%08x reg=%u (%s) val=%02x\n",
                (unsigned)machine_cpu_pc(m),
                (unsigned)cia_b_reg,
                cia_reg_names[cia_b_reg],
                (unsigned)(value & 0xFFu));

        cia_write_reg(&m->cia_b, cia_b_reg, (uint8_t)value);
        machine_floppy_update(m);

        if (cia_b_reg == 1u)
        {
            uint8_t prb = m->cia_b.prb;

            kprintf("[FLOPPY-CTRL] pc=%08x prb=%02x motor=%d sel0=%d sel1=%d sel2=%d sel3=%d step=%d dir=%d side=%d\n",
                    (unsigned)machine_cpu_pc(m), (unsigned)prb,
                    !(prb & 0x80u),
                    !(prb & 0x08u),
                    !(prb & 0x10u),
                    !(prb & 0x20u),
                    !(prb & 0x40u),
                    !(prb & 0x01u),
                    (prb & 0x02u) ? 1 : 0,
                    !(prb & 0x04u));

            kprintf("[FLOPPY-STAT] ready=%d track0=%d changed=%d cyl=%d id_count=%u motor=%d side=%d\n",
                    m->df0.ready,
                    m->df0.track0,
                    m->df0.disk_changed,
                    m->df0.cylinder,
                    m->df0.id_count,
                    m->df0.motor,
                    m->df0.side);
        }

        machine_probe_emit(m, PROBE_EVT_CIA_WRITE, addr, value);
    }
}

uint32_t bellatrix_machine_read(uint32_t addr, unsigned int size)
{
    BellatrixMachine *m = &g_machine;
    uint32_t value;

    machine_step_components(m, 1);

    if (size == 4)
    {
        uint32_t hi = machine_dispatch_read(m, addr, 2);
        uint32_t lo = machine_dispatch_read(m, addr + 2, 2);
        value = (hi << 16) | lo;
    }
    else
    {
        value = machine_dispatch_read(m, addr, size);
    }

    machine_btrace_read(m, addr, size, value);
    bellatrix_machine_sync_ipl();
    return value;
}

void bellatrix_machine_write(uint32_t addr, uint32_t value, unsigned int size)
{
    BellatrixMachine *m = &g_machine;

    machine_step_components(m, 1);
    machine_btrace_write(m, addr, size, value);

    if (size == 4)
    {
        machine_dispatch_write(m, addr, value >> 16, 2);
        machine_dispatch_write(m, addr + 2, value & 0xFFFF, 2);
    }
    else
    {
        machine_dispatch_write(m, addr, value, size);
    }

    bellatrix_machine_sync_ipl();
}

/* ---------------------------------------------------------------------------
 * Raw access to owned components
 * ------------------------------------------------------------------------- */

Agnus *bellatrix_machine_agnus(void)
{
    return &g_machine.agnus;
}

Denise *bellatrix_machine_denise(void)
{
    return &g_machine.denise;
}

Paula *bellatrix_machine_paula(void)
{
    return &g_machine.paula;
}

CIA *bellatrix_machine_cia_a(void)
{
    return &g_machine.cia_a;
}

CIA *bellatrix_machine_cia_b(void)
{
    return &g_machine.cia_b;
}

RTCState *bellatrix_machine_rtc(void)
{
    return &g_machine.rtc;
}

/* ---------------------------------------------------------------------------
 * Public floppy sync (called from bellatrix.c after CIA-B writes in Emu68 path)
 * ------------------------------------------------------------------------- */

void bellatrix_machine_floppy_update(void)
{
    machine_floppy_update(&g_machine);
}

int bellatrix_machine_insert_df0_adf(const uint8_t *adf, uint32_t adf_size)
{
    BellatrixMachine *m = &g_machine;

    if (!adf || adf_size == 0)
        return 0;

    floppy_insert(&m->df0, adf, adf_size);
    machine_sync_floppy_pra(m);

    kprintf("[FLOPPY] DF0 ADF inserted size=%u\n", (unsigned)adf_size);

    return 1;
}

void bellatrix_machine_eject_df0(void)
{
    BellatrixMachine *m = &g_machine;

    floppy_eject(&m->df0);
    machine_sync_floppy_pra(m);

    kprintf("[FLOPPY] DF0 ejected\n");
}

/* ---------------------------------------------------------------------------
 * btrace wrappers for callers that don't own the BTraceState
 * ------------------------------------------------------------------------- */

void bellatrix_machine_btrace_log(uint32_t addr, uint32_t value,
                                  unsigned int size, uint8_t dir, uint8_t impl)
{
    BellatrixMachine *m = &g_machine;

    if (!m->debug.enable_btrace)
        return;

    btrace_log(&m->debug.btrace,
               (uint32_t)m->tick_count,
               machine_cpu_pc(m),
               addr, value, size, dir, impl);
}

void bellatrix_machine_btrace_set_filter(uint16_t filter)
{
    btrace_set_filter(&g_machine.debug.btrace, filter);
}
