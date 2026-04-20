#include <stdint.h>

/*
 * Helpers locais de tempo para raspi3.
 *
 * Neste momento eles ainda não entram em pal.h, porque o seu PAL público
 * atual está focado em:
 * - debug
 * - IPL
 * - timer de chipset
 * - vídeo
 * - core dedicado
 *
 * Então este arquivo fica como suporte interno do port inicial do código
 * do PiStorm, sem expandir a interface pública agora.
 */

#define RASPI3_LEGACY_SYS_TIMER_CLO   ((volatile uint32_t *)0xF2003004u)
#define RASPI3_LEGACY_SYS_TIMER_CHI   ((volatile uint32_t *)0xF2003008u)

#ifndef LE32
#define LE32(x) (x)
#endif

static inline uint64_t raspi3_read_legacy_system_timer(void)
{
    uint64_t hi = LE32(*RASPI3_LEGACY_SYS_TIMER_CHI);
    uint64_t lo = LE32(*RASPI3_LEGACY_SYS_TIMER_CLO);

    if (hi != LE32(*RASPI3_LEGACY_SYS_TIMER_CHI)) {
        hi = LE32(*RASPI3_LEGACY_SYS_TIMER_CHI);
        lo = LE32(*RASPI3_LEGACY_SYS_TIMER_CLO);
    }

    return (hi << 32) | lo;
}

static inline uint64_t raspi3_read_cntpct_el0(void)
{
    uint64_t value;
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(value));
    return value;
}

uint64_t raspi3_counter_get(void)
{
    return raspi3_read_cntpct_el0();
}

uint64_t raspi3_counter_freq(void)
{
    uint64_t value;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(value));
    return value;
}

void raspi3_delay_us(uint64_t delta_us)
{
    uint64_t target = raspi3_read_legacy_system_timer() + delta_us;

    while (raspi3_read_legacy_system_timer() < target) {
        asm volatile("" ::: "memory");
    }
}

void raspi3_delay_ticks(uint64_t ticks)
{
    uint64_t target = raspi3_read_cntpct_el0() + ticks;

    while (raspi3_read_cntpct_el0() < target) {
        asm volatile("" ::: "memory");
    }
}

void raspi3_delay_ticks_wfe(uint64_t ticks)
{
    uint64_t target = raspi3_read_cntpct_el0() + ticks;

    do {
        asm volatile("wfe");
    } while (raspi3_read_cntpct_el0() < target);
}

void raspi3_wait_for_event(void)
{
    asm volatile("wfe");
}