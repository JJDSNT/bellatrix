#include "host/raspi3/vc_mailbox.h"

#include "support.h"
#include "mmu.h"

#define VC_MBOX_CH_PROP 8u
#define VC_MBOX_TX_FULL 0x80000000u
#define VC_MBOX_RX_EMPTY 0x40000000u
#define VC_MBOX_CHANMASK 0xFu
#define VC_FIRMWARE_STATUS_REQUEST 0u
#define VC_FIRMWARE_STATUS_SUCCESS 0x80000000u
#define VC_FIRMWARE_PROPERTY_END 0u
#define VC_FIRMWARE_GET_CLOCK_RATE 0x00030002u
#define VC_FIRMWARE_GET_THROTTLED 0x00030046u
#define VC_CLOCK_ID_ARM 3u
#define VC_CLOCK_ID_CORE 4u
#define VC_CLOCK_ID_PIXEL 9u
#define ARM_PERI_VIRT_BASE 0xF2000000u
#define VC_MBOX_READ_ADDR   (ARM_PERI_VIRT_BASE + 0xB880u)
#define VC_MBOX_STATUS_ADDR (ARM_PERI_VIRT_BASE + 0xB898u)
#define VC_MBOX_WRITE_ADDR  (ARM_PERI_VIRT_BASE + 0xB8A0u)

/* Bounded mailbox receive — a missing firmware response must degrade to the
 * caller's fallback, not hang boot with the console dark. */
#define VC_MBOX_RECV_SPINS 4000000u

static uint32_t vc_property_buffer[8] __attribute__((aligned(16)));

bool vc_mbox_recv(uint32_t channel, uint32_t *out)
{
    uint32_t response;
    uint32_t spins = VC_MBOX_RECV_SPINS;

    do {
        while (rd32le(VC_MBOX_STATUS_ADDR) & VC_MBOX_RX_EMPTY) {
            dsb();
            if (--spins == 0u) {
                kprintf("[VC] mailbox recv timeout (ch=%u)\n", (unsigned)channel);
                return false;
            }
        }

        dmb();
        response = rd32le(VC_MBOX_READ_ADDR);
        dmb();
    } while ((response & VC_MBOX_CHANMASK) != channel);

    if (out) {
        *out = response & ~VC_MBOX_CHANMASK;
    }
    return true;
}

void vc_mbox_send(uint32_t channel, uint32_t data)
{
    uint32_t value = (data & ~VC_MBOX_CHANMASK) | (channel & VC_MBOX_CHANMASK);

    while (rd32le(VC_MBOX_STATUS_ADDR) & VC_MBOX_TX_FULL) {
        dsb();
    }

    dmb();
    wr32le(VC_MBOX_WRITE_ADDR, value);
}

static uint32_t vc_get_clock_hz(uint32_t clock_id)
{
    vc_property_buffer[0] = LE32(sizeof(vc_property_buffer));
    vc_property_buffer[1] = LE32(VC_FIRMWARE_STATUS_REQUEST);
    vc_property_buffer[2] = LE32(VC_FIRMWARE_GET_CLOCK_RATE);
    vc_property_buffer[3] = LE32(8);
    vc_property_buffer[4] = 0;
    vc_property_buffer[5] = LE32(clock_id);
    vc_property_buffer[6] = 0;
    vc_property_buffer[7] = LE32(VC_FIRMWARE_PROPERTY_END);

    arm_flush_cache((uintptr_t)vc_property_buffer, sizeof(vc_property_buffer));
    vc_mbox_send(VC_MBOX_CH_PROP, (uint32_t)mmu_virt2phys((uintptr_t)vc_property_buffer));
    if (!vc_mbox_recv(VC_MBOX_CH_PROP, NULL)) {
        return 0;
    }
    arm_dcache_invalidate((uintptr_t)vc_property_buffer, sizeof(vc_property_buffer));

    if (LE32(vc_property_buffer[1]) != VC_FIRMWARE_STATUS_SUCCESS) {
        return 0;
    }
    return LE32(vc_property_buffer[6]);
}

uint32_t vc_get_core_clock_hz(void)
{
    return vc_get_clock_hz(VC_CLOCK_ID_CORE);
}

uint32_t vc_get_arm_clock_hz(void)
{
    return vc_get_clock_hz(VC_CLOCK_ID_ARM);
}

uint32_t vc_get_throttled(void)
{
    vc_property_buffer[0] = LE32(sizeof(vc_property_buffer));
    vc_property_buffer[1] = LE32(VC_FIRMWARE_STATUS_REQUEST);
    vc_property_buffer[2] = LE32(VC_FIRMWARE_GET_THROTTLED);
    vc_property_buffer[3] = LE32(4);
    vc_property_buffer[4] = 0;
    vc_property_buffer[5] = 0;    /* mask 0: do not clear sticky bits */
    vc_property_buffer[6] = 0;
    vc_property_buffer[7] = LE32(VC_FIRMWARE_PROPERTY_END);

    arm_flush_cache((uintptr_t)vc_property_buffer, sizeof(vc_property_buffer));
    vc_mbox_send(VC_MBOX_CH_PROP, (uint32_t)mmu_virt2phys((uintptr_t)vc_property_buffer));
    if (!vc_mbox_recv(VC_MBOX_CH_PROP, NULL)) {
        return 0xFFFFFFFFu;
    }
    arm_dcache_invalidate((uintptr_t)vc_property_buffer, sizeof(vc_property_buffer));

    if (LE32(vc_property_buffer[1]) != VC_FIRMWARE_STATUS_SUCCESS) {
        return 0xFFFFFFFFu;
    }
    return LE32(vc_property_buffer[5]);
}

/* HDMI pixel (TMDS) clock — needed to compute the audio CTS for clock
 * regeneration. Returns 0 if the firmware doesn't report it. */
uint32_t vc_get_pixel_clock_hz(void)
{
    return vc_get_clock_hz(VC_CLOCK_ID_PIXEL);
}
