// src/chipset/floppy/floppy_drive.c

#include "floppy_drive.h"

/* ------------------------------------------------------------------------- */
/* Init                                                                      */
/* ------------------------------------------------------------------------- */

void floppy_init(FloppyDrive *d)
{
    d->motor = 0;
    d->cylinder = 0;
    d->side = 0;

    d->ready = 0;
    d->track0 = 1;

    d->disk_inserted = 0;
    d->disk_changed = 1;   /* power-on: change latch set, no disk */

    d->step_latch = 1;

    d->id_data = 0xFFFFFFFF; // standard DD drive
    d->id_count = 0;

    d->adf = 0;
}

/* ------------------------------------------------------------------------- */
/* Media                                                                     */
/* ------------------------------------------------------------------------- */

void floppy_insert(FloppyDrive *d, const uint8_t *adf)
{
    d->adf = adf;
    d->disk_inserted = 1;
    d->disk_changed = 1; // LOW até primeiro step
}

void floppy_eject(FloppyDrive *d)
{
    d->adf = 0;
    d->disk_inserted = 0;
    d->disk_changed = 1;
}

/* ------------------------------------------------------------------------- */
/* Core Step                                                                 */
/* ------------------------------------------------------------------------- */

void floppy_step(FloppyDrive *d, const FloppySignals *sig)
{
    if (!sig->selected) {
        // Deselection edge (para ID)
        if (d->motor == 0) {
            d->id_count++;
        }
        return;
    }

    /* ------------------------------------------------------------- */
    /* Motor                                                         */
    /* ------------------------------------------------------------- */

    if (sig->motor) {
        if (!d->motor) {
            d->motor = 1;
            d->ready = 1;
            d->id_count = 0;
        }
    } else {
        if (d->motor) {
            d->motor = 0;
            d->ready = 0;
        }
    }

    /* ------------------------------------------------------------- */
    /* Side                                                          */
    /* ------------------------------------------------------------- */

    d->side = sig->side;

    /* ------------------------------------------------------------- */
    /* Step (edge detect)                                            */
    /* ------------------------------------------------------------- */

    if (sig->step && d->step_latch) {

        d->step_latch = 0;

        if (sig->direction) {
            d->cylinder++;
        } else {
            d->cylinder--;
            if (d->cylinder < 0)
                d->cylinder = 0;
        }

        // STEP limpa disk change
        d->disk_changed = 0;
    }

    if (!sig->step) {
        d->step_latch = 1;
    }

    /* ------------------------------------------------------------- */
    /* Track0                                                        */
    /* ------------------------------------------------------------- */

    d->track0 = (d->cylinder == 0);
}

/* ------------------------------------------------------------------------- */
/* Outputs                                                                   */
/* ------------------------------------------------------------------------- */

int floppy_get_ready(const FloppyDrive *d)
{
    return d->ready;
}

int floppy_get_track0(const FloppyDrive *d)
{
    return d->track0;
}

int floppy_get_dskchg(const FloppyDrive *d, int motor_on)
{
    // ativo LOW

    if (!motor_on) {
        // ID mode
        int bit = (d->id_data >> (31 - (d->id_count & 31))) & 1;
        return bit ? 1 : 0;
    }

    return d->disk_changed ? 0 : 1;
}