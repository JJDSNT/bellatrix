// src/chipset/paula/paula_disk.h

#ifndef BELLATRIX_PAULA_DISK_H
#define BELLATRIX_PAULA_DISK_H

#include <stdint.h>
#include <stddef.h>

#include "chipset/floppy/floppy_drive.h"

#define PAULA_INTREQ_DSKBLK  0x0002u
#define PAULA_INTREQ_DSKSYNC 0x1000u

typedef void (*PaulaDiskIntreqFn)(void *user, uint16_t bits);

typedef struct PaulaDisk {
    uint32_t dskptr;
    uint16_t dsklen;
    uint16_t dskbytr;
    uint16_t dsksync;
    uint16_t adkcon;

    int dma_active;
    int dma_armed;
    int write_mode;
    int sync_seen;
    int sync_irq_fired;
    uint16_t dskbytr_data;

    uint16_t armed_dsklen;
    uint32_t countdown;

    FloppyDrive *drive;

    uint8_t *chipram;
    size_t chipram_size;

    PaulaDiskIntreqFn intreq_cb;
    void *intreq_user;
} PaulaDisk;

void paula_disk_init(PaulaDisk *pd);

void paula_disk_attach_memory(PaulaDisk *pd, uint8_t *chipram, size_t size);
void paula_disk_attach_drive(PaulaDisk *pd, FloppyDrive *drive);
void paula_disk_set_intreq_callback(PaulaDisk *pd, PaulaDiskIntreqFn cb, void *user);

void paula_disk_write_dskpth(PaulaDisk *pd, uint16_t value);
void paula_disk_write_dskptl(PaulaDisk *pd, uint16_t value);
void paula_disk_write_dsklen(PaulaDisk *pd, uint16_t value);
void paula_disk_write_dsksync(PaulaDisk *pd, uint16_t value);
void paula_disk_write_adkcon(PaulaDisk *pd, uint16_t value);

uint16_t paula_disk_read_dskbytr(PaulaDisk *pd);

void paula_disk_step(PaulaDisk *pd, uint32_t cycles);

#endif
