#include "core/machine.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REG_ADKCON 0x00dff09eu
#define REG_DSKBYTR 0x00dff01au
#define REG_DSKPTH 0x00dff020u
#define REG_DSKPTL 0x00dff022u
#define REG_DSKLEN 0x00dff024u
#define REG_COP1LCH 0x00dff080u
#define REG_COP1LCL 0x00dff082u
#define REG_COPJMP1 0x00dff088u
#define REG_DMACON 0x00dff096u
#define REG_DMACONR 0x00dff002u
#define REG_DSKSYNC 0x00dff07eu
#define REG_INTENA 0x00dff09au
#define REG_INTENAR 0x00dff01cu
#define REG_INTREQ 0x00dff09cu
#define REG_INTREQR 0x00dff01eu
#define REG_BLTCON0 0x00dff040u
#define REG_BLTCON1 0x00dff042u
#define REG_BLTAFWM 0x00dff044u
#define REG_BLTALWM 0x00dff046u
#define REG_BLTCPTH 0x00dff048u
#define REG_BLTCPTL 0x00dff04au
#define REG_BLTAPTH 0x00dff050u
#define REG_BLTAPTL 0x00dff052u
#define REG_BLTDPTH 0x00dff054u
#define REG_BLTDPTL 0x00dff056u
#define REG_BLTSIZE 0x00dff058u
#define REG_BLTBMOD 0x00dff062u
#define REG_BLTAMOD 0x00dff064u
#define REG_BLTDMOD 0x00dff066u
#define REG_BLTBDAT 0x00dff072u
#define REG_VPOSR 0x00dff004u
#define REG_VHPOSR 0x00dff006u

#define ADKCON_SETCLR 0x8000u
#define ADKCON_WORDSYNC 0x0400u
#define DSKBYTR_DATA 0x8000u
#define DSKBYTR_DMAON 0x2000u
#define DSKBYTR_WORDSYNC 0x0800u
#define DSKLEN_DMAEN 0x8000u

static void failf(const char *expr, int expected, int actual, int line)
{
    fprintf(stderr,
            "FAIL line=%d %s expected=%d actual=%d\n",
            line, expr, expected, actual);
    exit(1);
}

#define CHECK_INT(expr, expected, actual)                                    \
    do                                                                       \
    {                                                                        \
        int expected__ = (expected);                                         \
        int actual__ = (actual);                                             \
        if (expected__ != actual__)                                          \
            failf((expr), expected__, actual__, __LINE__);                   \
    } while (0)

static uint8_t s_adf[FLOPPY_ADF_DD_SIZE];
static uint8_t s_chip_ram[BELLATRIX_CHIP_RAM_SIZE];

static int harness_overlay_state(const BellatrixMachine *m)
{
    if (!(m->cia_a.ddra & 0x01u))
        return 1;

    return (m->cia_a.pra & 0x01u) ? 1 : 0;
}

static BellatrixMachine *test_machine_init(void)
{
    BellatrixMachine *m;

    bellatrix_machine_init(NULL);
    m = bellatrix_machine_get();

    m->memory.chip_ram = s_chip_ram;
    m->memory.chip_ram_size = sizeof(s_chip_ram);
    m->memory.chip_ram_mask = sizeof(s_chip_ram) - 1u;

    paula_attach_memory(&m->paula, m->memory.chip_ram, m->memory.chip_ram_size);

    return m;
}

static void test_overlay_switch_via_cia_a(void)
{
    BellatrixMachine *m;

    m = test_machine_init();

    CHECK_INT("memory overlay defaults on",
              1,
              bellatrix_memory_overlay_enabled(&m->memory));
    CHECK_INT("harness overlay defaults on", 1, harness_overlay_state(m));

    bellatrix_machine_write(0x00bfe201u, 0x01u, 1);
    CHECK_INT("ddra bit0 becomes output", 0x01, m->cia_a.ddra);
    CHECK_INT("overlay still on until pra changes", 1, harness_overlay_state(m));

    bellatrix_machine_write(0x00bfe001u, 0x00u, 1);
    CHECK_INT("pra bit0 low means overlay off for harness path",
              0,
              harness_overlay_state(m));

    bellatrix_machine_write(0x00bfe001u, 0x01u, 1);
    CHECK_INT("pra bit0 high means overlay on for harness path",
              1,
              harness_overlay_state(m));
}

static void test_cia_irq_routes_to_paula(void)
{
    BellatrixMachine *m;

    m = test_machine_init();

    m->paula.intena = (uint16_t)(PAULA_INT_MASTER | PAULA_INT_PORTS | PAULA_INT_EXTER);

    cia_write_reg(&m->cia_a, CIA_REG_ICR, (uint8_t)(CIA_ICR_SETCLR | CIA_ICR_SP));
    cia_write_reg(&m->cia_a, CIA_REG_CRA, CIA_CRA_SPMODE);
    cia_write_reg(&m->cia_a, CIA_REG_SDR, 0x55u);

    CHECK_INT("cia-a irq pending", 1, cia_irq_pending(&m->cia_a));
    CHECK_INT("paula intreq has PORTS", PAULA_INT_PORTS, m->paula.intreq & PAULA_INT_PORTS);
    CHECK_INT("paula computes ipl2", 2, paula_compute_ipl(&m->paula));

    bellatrix_machine_sync_ipl();
    CHECK_INT("machine publishes ipl2", 2, m->current_ipl);

    (void)cia_read_reg(&m->cia_a, CIA_REG_ICR);
    bellatrix_machine_sync_ipl();

    CHECK_INT("cia-a irq cleared after icr read", 0, cia_irq_pending(&m->cia_a));
    CHECK_INT("paula ports line cleared", 0, m->paula.intreq & PAULA_INT_PORTS);
    CHECK_INT("machine ipl returns to 0", 0, m->current_ipl);

    cia_write_reg(&m->cia_b, CIA_REG_ICR, (uint8_t)(CIA_ICR_SETCLR | CIA_ICR_SP));
    cia_write_reg(&m->cia_b, CIA_REG_CRA, CIA_CRA_SPMODE);
    cia_write_reg(&m->cia_b, CIA_REG_SDR, 0xAAu);

    CHECK_INT("cia-b irq pending", 1, cia_irq_pending(&m->cia_b));
    CHECK_INT("paula intreq has EXTER", PAULA_INT_EXTER, m->paula.intreq & PAULA_INT_EXTER);
    CHECK_INT("paula computes ipl6", 6, paula_compute_ipl(&m->paula));

    bellatrix_machine_sync_ipl();
    CHECK_INT("machine publishes ipl6", 6, m->current_ipl);

    (void)cia_read_reg(&m->cia_b, CIA_REG_ICR);
    bellatrix_machine_sync_ipl();

    CHECK_INT("cia-b irq cleared after icr read", 0, cia_irq_pending(&m->cia_b));
    CHECK_INT("paula exter line cleared", 0, m->paula.intreq & PAULA_INT_EXTER);
    CHECK_INT("machine ipl returns to 0 after cia-b", 0, m->current_ipl);
}

static void test_floppy_signals_reflect_media_state(void)
{
    BellatrixMachine *m;
    uint8_t pra;

    m = test_machine_init();

    CHECK_INT("insert dummy adf", 1, bellatrix_machine_insert_df0_adf(s_adf, sizeof(s_adf)));

    bellatrix_machine_write(0x00bfd100u, 0x77u, 1);
    pra = cia_port_a_value(&m->cia_a);

    CHECK_INT("/DSKCHG low after insert", 0x00, pra & 0x04u);
    CHECK_INT("/WPRO low with inserted protected disk", 0x00, pra & 0x08u);
    CHECK_INT("/TK0 low at cylinder 0", 0x00, pra & 0x10u);
    CHECK_INT("/RDY low when motor is on", 0x00, pra & 0x20u);

    bellatrix_machine_write(0x00bfd100u, 0x76u, 1);
    bellatrix_machine_write(0x00bfd100u, 0x77u, 1);
    pra = cia_port_a_value(&m->cia_a);

    CHECK_INT("/DSKCHG high after step acknowledges media", 0x04, pra & 0x04u);

    bellatrix_machine_eject_df0();
    bellatrix_machine_write(0x00bfd100u, 0x77u, 1);
    pra = cia_port_a_value(&m->cia_a);

    CHECK_INT("/DSKCHG low after eject", 0x00, pra & 0x04u);
    CHECK_INT("/WPRO high with no disk", 0x08, pra & 0x08u);
}

static void test_paula_intreq_ack_reasserts_level_lines(void)
{
    BellatrixMachine *m;

    m = test_machine_init();

    bellatrix_machine_write(REG_INTENA,
                            PAULA_INT_MASTER | PAULA_INT_PORTS | 0x8000u,
                            2);

    cia_write_reg(&m->cia_a, CIA_REG_ICR, (uint8_t)(CIA_ICR_SETCLR | CIA_ICR_SP));
    cia_write_reg(&m->cia_a, CIA_REG_CRA, CIA_CRA_SPMODE);
    cia_write_reg(&m->cia_a, CIA_REG_SDR, 0x55u);
    bellatrix_machine_sync_ipl();

    CHECK_INT("ports irq reaches paula", PAULA_INT_PORTS, m->paula.intreq & PAULA_INT_PORTS);
    CHECK_INT("ports irq publishes ipl2", 2, m->current_ipl);

    bellatrix_machine_write(REG_INTREQ, PAULA_INT_PORTS, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("ack reasserts level-triggered ports irq",
              PAULA_INT_PORTS,
              m->paula.intreq & PAULA_INT_PORTS);
    CHECK_INT("ipl stays asserted while cia source is active", 2, m->current_ipl);

    (void)cia_read_reg(&m->cia_a, CIA_REG_ICR);
    bellatrix_machine_sync_ipl();

    CHECK_INT("cia source cleared", 0, cia_irq_pending(&m->cia_a));
    CHECK_INT("paula line clears after cia source drops", 0, m->paula.intreq & PAULA_INT_PORTS);
    CHECK_INT("ipl returns to idle", 0, m->current_ipl);
}

static void test_paula_disk_dma_irq_contract(void)
{
    BellatrixMachine *m;
    const uint32_t dskptr = 0x1000u;
    const uint16_t dsklen = (uint16_t)(DSKLEN_DMAEN | 32u);
    uint16_t dskbytr;

    m = test_machine_init();
    memset(s_chip_ram, 0, sizeof(s_chip_ram));

    CHECK_INT("insert dummy adf for disk dma",
              1,
              bellatrix_machine_insert_df0_adf(s_adf, sizeof(s_adf)));

    bellatrix_machine_write(REG_INTENA,
                            0x8000u | PAULA_INT_MASTER | PAULA_INT_DSKSYN | PAULA_INT_DSKBLK,
                            2);
    bellatrix_machine_write(REG_DSKSYNC, 0x4489u, 2);
    bellatrix_machine_write(REG_ADKCON, ADKCON_SETCLR | ADKCON_WORDSYNC, 2);
    bellatrix_machine_write(REG_DSKPTH, dskptr >> 16, 2);
    bellatrix_machine_write(REG_DSKPTL, dskptr & 0xFFFFu, 2);

    bellatrix_machine_write(REG_DSKLEN, dsklen, 2);
    CHECK_INT("first dsklen write only arms dma", 1, m->paula.disk.dma_armed);
    CHECK_INT("first dsklen write does not start dma", 0, m->paula.disk.dma_active);

    bellatrix_machine_write(REG_DSKLEN, dsklen, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("second dsklen write starts dma", 1, m->paula.disk.dma_active);
    CHECK_INT("sync irq latched immediately", PAULA_INT_DSKSYN, m->paula.intreq & PAULA_INT_DSKSYN);
    CHECK_INT("sync irq publishes ipl5", 5, m->current_ipl);

    dskbytr = (uint16_t)bellatrix_machine_read(REG_DSKBYTR, 2);
    CHECK_INT("dskbytr reports latched data", DSKBYTR_DATA, dskbytr & DSKBYTR_DATA);
    CHECK_INT("dskbytr reports dma active", DSKBYTR_DMAON, dskbytr & DSKBYTR_DMAON);
    CHECK_INT("dskbytr reports wordsync", DSKBYTR_WORDSYNC, dskbytr & DSKBYTR_WORDSYNC);

    bellatrix_machine_write(REG_INTREQ, PAULA_INT_DSKSYN, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("sync irq can be acked like edge event", 0, m->paula.intreq & PAULA_INT_DSKSYN);
    CHECK_INT("ipl drops after sync ack while dma still runs", 0, m->current_ipl);

    bellatrix_machine_advance(46000u);
    bellatrix_machine_sync_ipl();

    CHECK_INT("dma completes after countdown", 0, m->paula.disk.dma_active);
    CHECK_INT("dskblk irq latched on completion", PAULA_INT_DSKBLK, m->paula.intreq & PAULA_INT_DSKBLK);
    CHECK_INT("dskptr advanced by transfer length", dskptr + 64u, (int)m->paula.disk.dskptr);
    CHECK_INT("chip ram received encoded mfm bytes", 0xAA, s_chip_ram[dskptr]);
    CHECK_INT("dskblk publishes level 1", 1, m->current_ipl);

    bellatrix_machine_write(REG_INTREQ, PAULA_INT_DSKBLK, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("dskblk ack clears latch", 0, m->paula.intreq & PAULA_INT_DSKBLK);
    CHECK_INT("ipl returns to idle after dskblk ack", 0, m->current_ipl);
}

static void test_agnus_vblank_irq_real_path(void)
{
    BellatrixMachine *m;
    const uint32_t ticks_to_leave_vblank = (BEAM_PAL_VBL_END * BEAM_PAL_HPOS) + 1u;
    const uint32_t ticks_to_next_vblank =
        ((BEAM_PAL_LINES - BEAM_PAL_VBL_END) * BEAM_PAL_HPOS);

    m = test_machine_init();

    bellatrix_machine_write(REG_INTENA,
                            0x8000u | PAULA_INT_MASTER | PAULA_INT_VERTB,
                            2);

    bellatrix_machine_advance(ticks_to_leave_vblank);
    CHECK_INT("beam leaves initial vblank before edge test",
              0,
              beam_is_in_vblank(&m->agnus.beam));
    CHECK_INT("no vertb latched before next edge", 0, m->paula.intreq & PAULA_INT_VERTB);

    bellatrix_machine_advance(ticks_to_next_vblank);
    bellatrix_machine_sync_ipl();

    CHECK_INT("beam re-enters vblank", 1, beam_is_in_vblank(&m->agnus.beam));
    CHECK_INT("vertb latched on vblank entry", PAULA_INT_VERTB, m->paula.intreq & PAULA_INT_VERTB);
    CHECK_INT("vertb publishes ipl3", 3, m->current_ipl);

    bellatrix_machine_write(REG_INTREQ, PAULA_INT_VERTB, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("vertb ack clears latch", 0, m->paula.intreq & PAULA_INT_VERTB);
    CHECK_INT("ipl returns to idle after vertb ack", 0, m->current_ipl);
}

static void test_agnus_blitter_irq_real_path(void)
{
    BellatrixMachine *m;

    m = test_machine_init();

    bellatrix_machine_write(REG_INTENA,
                            0x8000u | PAULA_INT_MASTER | PAULA_INT_BLIT,
                            2);

    bellatrix_machine_write(0x00dff058u, 0x0041u, 2);
    CHECK_INT("blitter becomes busy after bltsize write", 1, m->agnus.blitter.busy);
    CHECK_INT("blitter irq starts cleared", 0, m->paula.intreq & PAULA_INT_BLIT);

    bellatrix_machine_advance(1u);
    bellatrix_machine_sync_ipl();

    CHECK_INT("blitter completes after programmed cycles", 0, m->agnus.blitter.busy);
    CHECK_INT("blit irq latched on completion", PAULA_INT_BLIT, m->paula.intreq & PAULA_INT_BLIT);
    CHECK_INT("blit publishes ipl3", 3, m->current_ipl);

    bellatrix_machine_write(REG_INTREQ, PAULA_INT_BLIT, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("blit ack clears latch", 0, m->paula.intreq & PAULA_INT_BLIT);
    CHECK_INT("ipl returns to idle after blit ack", 0, m->current_ipl);
}

static void test_agnus_blitter_line_mode_writes_chip_ram(void)
{
    BellatrixMachine *m;
    const uint32_t dst = 0x0100u;

    m = test_machine_init();
    memset(s_chip_ram, 0, sizeof(s_chip_ram));

    bellatrix_machine_write(REG_BLTCON0, 0x0BCAu, 2);
    bellatrix_machine_write(REG_BLTCON1, 0x0011u, 2);
    bellatrix_machine_write(REG_BLTCPTH, dst >> 16, 2);
    bellatrix_machine_write(REG_BLTCPTL, dst & 0xFFFFu, 2);
    bellatrix_machine_write(REG_BLTDPTH, dst >> 16, 2);
    bellatrix_machine_write(REG_BLTDPTL, dst & 0xFFFFu, 2);
    bellatrix_machine_write(REG_BLTAPTL, 0xFFFDu, 2);
    bellatrix_machine_write(REG_BLTBMOD, 0x0000u, 2);
    bellatrix_machine_write(REG_BLTAMOD, 0xFFFAu, 2);
    bellatrix_machine_write(REG_BLTDMOD, 0x0004u, 2);
    bellatrix_machine_write(REG_BLTBDAT, 0xFFFFu, 2);
    bellatrix_machine_write(REG_BLTSIZE, 0x00C2u, 2);

    bellatrix_machine_advance(6u);

    CHECK_INT("line blit completes", 0, m->agnus.blitter.busy);
    CHECK_INT("line mode writes expected horizontal mask",
              0xE000,
              bellatrix_chip_read16(&m->memory, dst));
}

static void test_agnus_blitter_fill_mode_matches_reference(void)
{
    BellatrixMachine *m;
    const uint32_t src = 0x0200u;
    const uint32_t dst = 0x0300u;

    m = test_machine_init();
    memset(s_chip_ram, 0, sizeof(s_chip_ram));

    bellatrix_mem_write16(&m->memory, src, 0x9009u);

    bellatrix_machine_write(REG_BLTCON0, 0x09F0u, 2);
    bellatrix_machine_write(REG_BLTCON1, 0x0008u, 2);
    bellatrix_machine_write(REG_BLTAFWM, 0xFFFFu, 2);
    bellatrix_machine_write(REG_BLTALWM, 0xFFFFu, 2);
    bellatrix_machine_write(REG_BLTAPTH, src >> 16, 2);
    bellatrix_machine_write(REG_BLTAPTL, src & 0xFFFFu, 2);
    bellatrix_machine_write(REG_BLTDPTH, dst >> 16, 2);
    bellatrix_machine_write(REG_BLTDPTL, dst & 0xFFFFu, 2);
    bellatrix_machine_write(REG_BLTAMOD, 0x0000u, 2);
    bellatrix_machine_write(REG_BLTDMOD, 0x0000u, 2);
    bellatrix_machine_write(REG_BLTSIZE, 0x0041u, 2);

    bellatrix_machine_advance(1u);

    CHECK_INT("fill blit completes", 0, m->agnus.blitter.busy);
    CHECK_INT("inclusive fill expands the source edges as in reference",
              0xE00E,
              bellatrix_chip_read16(&m->memory, dst));
}

static void test_agnus_copper_irq_real_path(void)
{
    BellatrixMachine *m;
    const uint32_t copper_list = 0x0000u;

    m = test_machine_init();
    memset(s_chip_ram, 0, sizeof(s_chip_ram));

    bellatrix_machine_write(REG_INTENA,
                            0x8000u | PAULA_INT_MASTER | PAULA_INT_COPER,
                            2);

    bellatrix_mem_write16(&m->memory, copper_list + 0u, 0x009Cu);
    bellatrix_mem_write16(&m->memory, copper_list + 2u, 0x8010u);
    bellatrix_mem_write16(&m->memory, copper_list + 4u, 0xFFFFu);
    bellatrix_mem_write16(&m->memory, copper_list + 6u, 0xFFFEu);

    bellatrix_machine_write(REG_COP1LCH, copper_list >> 16, 2);
    bellatrix_machine_write(REG_COP1LCL, copper_list & 0xFFFFu, 2);
    bellatrix_machine_write(REG_DMACON, 0x8000u | DMAF_DMAEN | DMAF_COPEN, 2);
    bellatrix_machine_write(REG_COPJMP1, 0u, 2);

    bellatrix_machine_advance(4u);
    bellatrix_machine_sync_ipl();

    CHECK_INT("copper move raises copper irq", PAULA_INT_COPER, m->paula.intreq & PAULA_INT_COPER);
    CHECK_INT("copper irq publishes ipl3", 3, m->current_ipl);
    CHECK_INT("copper program halts after sentinel", COPPER_STATE_HALTED, m->agnus.copper.state);

    bellatrix_machine_write(REG_INTREQ, PAULA_INT_COPER, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("copper irq ack clears latch", 0, m->paula.intreq & PAULA_INT_COPER);
    CHECK_INT("ipl returns to idle after copper ack", 0, m->current_ipl);
}

static void test_paula_intena_masks_and_unmasks_latched_video_irqs(void)
{
    BellatrixMachine *m;

    m = test_machine_init();

    bellatrix_machine_write(REG_INTENA, 0x8000u | PAULA_INT_MASTER, 2);
    agnus_intreq_set(&m->agnus, PAULA_INT_VERTB);
    bellatrix_machine_sync_ipl();

    CHECK_INT("vertb latch exists even when source mask is off",
              PAULA_INT_VERTB,
              m->paula.intreq & PAULA_INT_VERTB);
    CHECK_INT("masked vertb does not publish ipl", 0, m->current_ipl);

    bellatrix_machine_write(REG_INTENA, 0x8000u | PAULA_INT_VERTB, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("enabling vertb source mask publishes pending irq", 3, m->current_ipl);

    bellatrix_machine_write(REG_INTENA, PAULA_INT_VERTB, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("clearing vertb source mask drops ipl", 0, m->current_ipl);
    CHECK_INT("clearing source mask does not clear latched intreq",
              PAULA_INT_VERTB,
              m->paula.intreq & PAULA_INT_VERTB);

    bellatrix_machine_write(REG_INTENA, 0x8000u | PAULA_INT_VERTB, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("re-enabling source mask republishes latched vertb", 3, m->current_ipl);

    bellatrix_machine_write(REG_INTENA, PAULA_INT_MASTER, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("clearing master drops ipl even with latched vertb", 0, m->current_ipl);
    CHECK_INT("clearing master does not clear latched vertb",
              PAULA_INT_VERTB,
              m->paula.intreq & PAULA_INT_VERTB);

    bellatrix_machine_write(REG_INTENA, 0xC000u, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("re-enabling master republishes latched vertb", 3, m->current_ipl);

    bellatrix_machine_write(REG_INTREQ, PAULA_INT_VERTB, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("vertb ack finally clears latch", 0, m->paula.intreq & PAULA_INT_VERTB);
    CHECK_INT("ipl returns to idle after ack", 0, m->current_ipl);

    bellatrix_machine_write(REG_INTENA, PAULA_INT_MASTER, 2);
    bellatrix_machine_sync_ipl();

    bellatrix_machine_write(REG_INTENA, 0x8000u | PAULA_INT_BLIT, 2);
    agnus_intreq_set(&m->agnus, PAULA_INT_BLIT);
    bellatrix_machine_sync_ipl();

    CHECK_INT("blit masked by missing master stays latched", PAULA_INT_BLIT, m->paula.intreq & PAULA_INT_BLIT);
    CHECK_INT("blit masked by missing master keeps ipl idle", 0, m->current_ipl);

    bellatrix_machine_write(REG_INTENA, 0xC000u, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("enabling master publishes pending blit", 3, m->current_ipl);

    bellatrix_machine_write(REG_INTREQ, PAULA_INT_BLIT, 2);
    bellatrix_machine_sync_ipl();

    CHECK_INT("blit ack clears latch after master restore", 0, m->paula.intreq & PAULA_INT_BLIT);
    CHECK_INT("ipl returns to idle after blit ack", 0, m->current_ipl);
}

static void raise_cia_a_sp_irq(BellatrixMachine *m)
{
    cia_write_reg(&m->cia_a, CIA_REG_ICR, (uint8_t)(CIA_ICR_SETCLR | CIA_ICR_SP));
    cia_write_reg(&m->cia_a, CIA_REG_CRA, CIA_CRA_SPMODE);
    cia_write_reg(&m->cia_a, CIA_REG_SDR, 0x55u);
}

static void raise_cia_b_sp_irq(BellatrixMachine *m)
{
    cia_write_reg(&m->cia_b, CIA_REG_ICR, (uint8_t)(CIA_ICR_SETCLR | CIA_ICR_SP));
    cia_write_reg(&m->cia_b, CIA_REG_CRA, CIA_CRA_SPMODE);
    cia_write_reg(&m->cia_b, CIA_REG_SDR, 0xAAu);
}

static void test_paula_irq_priority_matrix(void)
{
    BellatrixMachine *m;

    m = test_machine_init();

    bellatrix_machine_write(REG_INTENA,
                            0xFFFFu,
                            2);

    agnus_intreq_set(&m->agnus, PAULA_INT_VERTB | PAULA_INT_BLIT | PAULA_INT_COPER);
    bellatrix_machine_sync_ipl();

    CHECK_INT("multiple level-3 sources stay at ipl3", 3, m->current_ipl);

    raise_cia_a_sp_irq(m);
    bellatrix_machine_sync_ipl();

    CHECK_INT("level-3 beats ports level-2", 3, m->current_ipl);

    agnus_intreq_set(&m->agnus, PAULA_INT_DSKSYN);
    bellatrix_machine_sync_ipl();

    CHECK_INT("dsksyn level-5 beats video level-3", 5, m->current_ipl);

    raise_cia_b_sp_irq(m);
    bellatrix_machine_sync_ipl();

    CHECK_INT("exter level-6 beats everything", 6, m->current_ipl);

    bellatrix_machine_write(REG_INTREQ,
                            PAULA_INT_VERTB | PAULA_INT_BLIT | PAULA_INT_COPER | PAULA_INT_DSKSYN,
                            2);
    (void)cia_read_reg(&m->cia_a, CIA_REG_ICR);
    bellatrix_machine_sync_ipl();

    CHECK_INT("cia-b exter still wins after lower sources clear", 6, m->current_ipl);

    (void)cia_read_reg(&m->cia_b, CIA_REG_ICR);
    bellatrix_machine_sync_ipl();

    CHECK_INT("ipl returns to idle after highest source clears", 0, m->current_ipl);
    CHECK_INT("all tested paula latches cleared",
              0,
              m->paula.intreq & (PAULA_INT_VERTB |
                                 PAULA_INT_BLIT |
                                 PAULA_INT_COPER |
                                 PAULA_INT_DSKSYN |
                                 PAULA_INT_PORTS |
                                 PAULA_INT_EXTER));
}

static void test_custom_readback_basics(void)
{
    BellatrixMachine *m;
    uint16_t dmaconr;
    uint16_t intenar;
    uint16_t intreqr;
    uint16_t vposr;
    uint16_t vhposr;
    uint16_t expected_vposr;
    uint16_t expected_vhposr;

    m = test_machine_init();

    dmaconr = (uint16_t)bellatrix_machine_read(REG_DMACONR, 2);
    intenar = (uint16_t)bellatrix_machine_read(REG_INTENAR, 2);
    intreqr = (uint16_t)bellatrix_machine_read(REG_INTREQR, 2);
    vposr = (uint16_t)bellatrix_machine_read(REG_VPOSR, 2);
    vhposr = (uint16_t)agnus_read_reg(&m->agnus, AGNUS_VHPOSR);
    expected_vposr = (uint16_t)(0x2000u | (((m->agnus.beam.vpos >> 8) & 0x01u) << 8));
    expected_vhposr = (uint16_t)(((m->agnus.beam.vpos & 0xFFu) << 8) |
                                 (m->agnus.beam.hpos & 0xFFu));

    CHECK_INT("dmaconr starts with blitter zero flag only", 0x2000, dmaconr);
    CHECK_INT("intenar starts clear", 0x0000, intenar);
    CHECK_INT("intreqr starts clear", 0x0000, intreqr);
    CHECK_INT("vposr matches current beam state and agnus chip id", expected_vposr, vposr);
    CHECK_INT("vhposr matches current beam state", expected_vhposr, vhposr);

    bellatrix_machine_write(REG_DMACON, 0x8000u | DMAF_DMAEN | DMAF_COPEN, 2);
    dmaconr = (uint16_t)bellatrix_machine_read(REG_DMACONR, 2);
    CHECK_INT("dmaconr reflects enabled dma and copper bits",
              DMAF_DMAEN | DMAF_COPEN,
              dmaconr & (DMAF_DMAEN | DMAF_COPEN));

    bellatrix_machine_write(REG_DMACON, DMAF_COPEN, 2);
    dmaconr = (uint16_t)bellatrix_machine_read(REG_DMACONR, 2);
    CHECK_INT("dmaconr clear write drops copper bit but keeps dmaen",
              DMAF_DMAEN,
              dmaconr & (DMAF_DMAEN | DMAF_COPEN));
}

static void test_paula_intenar_intreqr_readback(void)
{
    BellatrixMachine *m;
    uint16_t intenar;
    uint16_t intreqr;

    m = test_machine_init();

    bellatrix_machine_write(REG_INTENA,
                            0x8000u | PAULA_INT_MASTER | PAULA_INT_VERTB,
                            2);
    agnus_intreq_set(&m->agnus, PAULA_INT_VERTB);
    bellatrix_machine_sync_ipl();

    intenar = (uint16_t)bellatrix_machine_read(REG_INTENAR, 2);
    intreqr = (uint16_t)bellatrix_machine_read(REG_INTREQR, 2);

    CHECK_INT("intenar readback exposes programmed mask",
              PAULA_INT_MASTER | PAULA_INT_VERTB,
              intenar);
    CHECK_INT("intreqr readback exposes latched vertb",
              PAULA_INT_VERTB,
              intreqr & PAULA_INT_VERTB);
    CHECK_INT("latched vertb publishes ipl3 through readback path", 3, m->current_ipl);

    bellatrix_machine_write(REG_INTREQ, PAULA_INT_VERTB, 2);
    bellatrix_machine_sync_ipl();
    intreqr = (uint16_t)bellatrix_machine_read(REG_INTREQR, 2);

    CHECK_INT("intreqr readback clears after ack", 0, intreqr & PAULA_INT_VERTB);
    CHECK_INT("ipl returns to idle after vertb ack via readback test", 0, m->current_ipl);
}

static void test_raster_progression_readback(void)
{
    BellatrixMachine *m;
    uint32_t ticks_to_next_line;
    uint32_t ticks_to_next_frame;
    uint64_t start_frame;
    uint16_t vposr;
    uint16_t vhposr;

    m = test_machine_init();
    start_frame = m->agnus.beam.frame;

    bellatrix_machine_advance(1u);
    vposr = (uint16_t)agnus_read_reg(&m->agnus, AGNUS_VPOSR);
    vhposr = (uint16_t)agnus_read_reg(&m->agnus, AGNUS_VHPOSR);
    CHECK_INT("vposr matches beam state after one tick",
              (int)(0x2000u | (((m->agnus.beam.vpos >> 8) & 0x01u) << 8)),
              vposr);
    CHECK_INT("vhposr matches beam state after one tick",
              (int)(((m->agnus.beam.vpos & 0xFFu) << 8) |
                    (m->agnus.beam.hpos & 0xFFu)),
              vhposr);

    ticks_to_next_line = beam_line_hmax(&m->agnus.beam) - beam_hpos(&m->agnus.beam);
    bellatrix_machine_advance(ticks_to_next_line);
    vhposr = (uint16_t)agnus_read_reg(&m->agnus, AGNUS_VHPOSR);
    CHECK_INT("vhposr matches beam state at next line",
              (int)(((m->agnus.beam.vpos & 0xFFu) << 8) |
                    (m->agnus.beam.hpos & 0xFFu)),
              vhposr);

    bellatrix_machine_advance(4096u);
    vposr = (uint16_t)agnus_read_reg(&m->agnus, AGNUS_VPOSR);
    vhposr = (uint16_t)agnus_read_reg(&m->agnus, AGNUS_VHPOSR);

    CHECK_INT("vposr keeps tracking current beam state",
              (int)(0x2000u | (((m->agnus.beam.vpos >> 8) & 0x01u) << 8)),
              vposr);
    CHECK_INT("vhposr keeps tracking current beam state",
              (int)(((m->agnus.beam.vpos & 0xFFu) << 8) |
                    (m->agnus.beam.hpos & 0xFFu)),
              vhposr);

    ticks_to_next_frame =
        ((beam_frame_vmax(&m->agnus.beam) - beam_vpos(&m->agnus.beam)) * BEAM_PAL_HPOS) -
        beam_hpos(&m->agnus.beam);
    bellatrix_machine_advance(ticks_to_next_frame);
    vposr = (uint16_t)agnus_read_reg(&m->agnus, AGNUS_VPOSR);
    vhposr = (uint16_t)agnus_read_reg(&m->agnus, AGNUS_VHPOSR);

    CHECK_INT("frame wrap readback matches beam origin",
              (int)(((m->agnus.beam.vpos & 0xFFu) << 8) |
                    (m->agnus.beam.hpos & 0xFFu)),
              vhposr);
    CHECK_INT("frame wrap vposr matches chip id and beam state",
              (int)(0x2000u | (((m->agnus.beam.vpos >> 8) & 0x01u) << 8)),
              vposr);
    CHECK_INT("frame counter advanced after full raster period",
              (int)(start_frame + 1u),
              (int)m->agnus.beam.frame);
}

int main(void)
{
    test_overlay_switch_via_cia_a();
    test_cia_irq_routes_to_paula();
    test_floppy_signals_reflect_media_state();
    test_paula_intreq_ack_reasserts_level_lines();
    test_paula_disk_dma_irq_contract();
    test_agnus_vblank_irq_real_path();
    test_agnus_blitter_irq_real_path();
    test_agnus_blitter_line_mode_writes_chip_ram();
    test_agnus_blitter_fill_mode_matches_reference();
    test_agnus_copper_irq_real_path();
    test_paula_intena_masks_and_unmasks_latched_video_irqs();
    test_paula_irq_priority_matrix();
    test_custom_readback_basics();
    test_paula_intenar_intreqr_readback();
    test_raster_progression_readback();

    puts("bellatrix_integration_overlay: ok");
    return 0;
}
