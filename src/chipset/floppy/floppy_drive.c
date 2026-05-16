// src/chipset/floppy/floppy_drive.c

#include "floppy_drive.h"
#include "support.h"

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
    d->disk_changed = 1; /* power-on: change latch set (no disk) */
    d->write_protected = 1;

    d->step_latch = 1;

    /*
     * Drive ID model follows Omega/vAmiga-style behavior:
     *
     * - The drive ID is shifted through /DSKCHG, not /DKRDY.
     * - A standard Amiga 3.5" DD drive has no explicit ID chip.
     * - Therefore the line is pulled HIGH during ID reads.
     *
     * 0xFFFFFFFF means every ID bit reads as 1.
     */
    d->id_data = 0xFFFFFFFFu;
    d->id_count = 0;

    d->adf = 0;
    d->adf_size = 0;
    d->read_offset = 0;
}

/* ------------------------------------------------------------------------- */
/* Media                                                                     */
/* ------------------------------------------------------------------------- */

void floppy_insert(FloppyDrive *d, const uint8_t *adf, uint32_t adf_size)
{
    d->adf = adf;
    d->adf_size = adf_size;

    d->disk_inserted = (adf != 0 && adf_size > 0);
    d->disk_changed = 1; /* LOW until first STEP */
    d->write_protected = 1;

    d->read_offset = 0;

    kprintf("[FLOPPY] media inserted size=%u bytes cyl=%d side=%d track0=%d\n",
            adf_size,
            d->cylinder,
            d->side,
            d->track0);
}

/* ------------------------------------------------------------------------- */

void floppy_eject(FloppyDrive *d)
{
    d->adf = 0;
    d->adf_size = 0;

    d->disk_inserted = 0;
    d->disk_changed = 1;
    d->write_protected = 0;

    d->ready = 0;
    d->motor = 0;

    d->read_offset = 0;

    kprintf("[FLOPPY] media ejected\n");
}

/* ------------------------------------------------------------------------- */
/* Core Step                                                                 */
/* ------------------------------------------------------------------------- */

void floppy_step(FloppyDrive *d, const FloppySignals *sig)
{
    if (!sig->selected)
    {
        /*
         * Deselection behavior for drive ID.
         *
         * Selection is external to the drive; deselecting DF0 must not
         * mechanically stop the motor by itself.
         *
         * Omega advances the ID shift counter on select -> deselect cycles
         * while the motor is OFF.
         */
        if (d->motor == 0)
            d->id_count++;

        return;
    }

    /* ------------------------------------------------------------- */
    /* Motor                                                         */
    /* ------------------------------------------------------------- */

    if (sig->motor)
    {
        if (!d->motor)
        {
            d->motor = 1;
            /*
             * /RDY should only assert once the drive is spinning a present
             * disk. An empty drive with motor ON must still report not-ready,
             * otherwise Kickstart treats DF0 as bootable media and follows an
             * error path instead of falling back to the insert-disk screen.
             */
            d->ready = floppy_has_media(d) ? 1 : 0;
            d->id_count = 0;
            kprintf("[FLOPPY] motor on ready=%d media=%d cyl=%d side=%d\n",
                    d->ready,
                    floppy_has_media(d),
                    d->cylinder,
                    d->side);
        }
    }
    else
    {
        if (d->motor)
        {
            d->motor = 0;
            d->ready = 0;
            kprintf("[FLOPPY] motor off cyl=%d side=%d\n",
                    d->cylinder,
                    d->side);
        }
    }

    /* ------------------------------------------------------------- */
    /* Side                                                          */
    /* ------------------------------------------------------------- */

    d->side = sig->side ? 1 : 0;

    /* ------------------------------------------------------------- */
    /* Step                                                          */
    /* ------------------------------------------------------------- */

    if (sig->step && d->step_latch)
    {
        int old_cylinder = d->cylinder;
        int old_dskchg = d->disk_changed;

        d->step_latch = 0;

        if (sig->direction)
        {
            /* DIR=1 means step out, toward cylinder 0. */
            if (d->cylinder > 0)
                d->cylinder--;
        }
        else
        {
            /* DIR=0 means step in, toward higher cylinders. */
            if (d->cylinder < (int)(FLOPPY_ADF_CYLINDERS - 1))
                d->cylinder++;
        }

        /*
         * /DSKCHG is a mechanical media-presence/change latch.
         *
         * A STEP pulse acknowledges a pending change only when there is
         * actual media in the drive. On an empty drive, keeping /DSKCHG
         * asserted LOW prevents the OS from treating DF0 as "disk present"
         * after the motor-off probe sequence.
         */
        d->disk_changed = floppy_has_media(d) ? 0 : 1;

        kprintf("[FLOPPY] step dir=%s cyl=%d->%d side=%d dskchg=%d->%d track0=%d media=%d\n",
                sig->direction ? "out" : "in",
                old_cylinder,
                d->cylinder,
                d->side,
                old_dskchg,
                d->disk_changed,
                d->cylinder == 0,
                floppy_has_media(d));
    }

    if (!sig->step)
        d->step_latch = 1;

    /* ------------------------------------------------------------- */
    /* Track0                                                        */
    /* ------------------------------------------------------------- */

    d->track0 = (d->cylinder == 0);
}

/* ------------------------------------------------------------------------- */
/* Media state                                                               */
/* ------------------------------------------------------------------------- */

int floppy_has_media(const FloppyDrive *d)
{
    return d->disk_inserted && d->adf != 0 && d->adf_size > 0;
}

/* ------------------------------------------------------------------------- */
/* Simplified ADF read                                                       */
/* ------------------------------------------------------------------------- */

uint32_t floppy_read_linear(FloppyDrive *d, uint8_t *dst, uint32_t bytes)
{
    uint32_t available;
    uint32_t to_copy;
    uint32_t i;

    if (!floppy_has_media(d))
        return 0;

    if (dst == 0 || bytes == 0)
        return 0;

    if (d->read_offset >= d->adf_size)
        return 0;

    available = d->adf_size - d->read_offset;
    to_copy = (bytes < available) ? bytes : available;

    for (i = 0; i < to_copy; i++)
        dst[i] = d->adf[d->read_offset + i];

    d->read_offset += to_copy;

    /*
     * First successful read acknowledges media change for this simplified path.
     * The mechanical model still also clears it on STEP.
     */
    d->disk_changed = 0;

    return to_copy;
}

/* ------------------------------------------------------------------------- */
/* Outputs                                                                   */
/* ------------------------------------------------------------------------- */

int floppy_get_ready(const FloppyDrive *d)
{
    return d->ready;
}

/* ------------------------------------------------------------------------- */

int floppy_get_track0(const FloppyDrive *d)
{
    return d->track0;
}

/* ------------------------------------------------------------------------- */

int floppy_get_dskchg(const FloppyDrive *d, int motor_on)
{
    (void)motor_on;

    /*
     * /DSKCHG is active LOW.
     *
     * LOW  = disk change pending / no disk acknowledged
     * HIGH = stable
     */
    return d->disk_changed ? 0 : 1;
}

/* ------------------------------------------------------------------------- */

int floppy_get_wpro(const FloppyDrive *d)
{
    /*
     * /WPRO is active LOW.
     *
     * LOW  = inserted disk is write-protected
     * HIGH = writable / no disk
     */
    if (!floppy_has_media(d))
        return 1;

    return d->write_protected ? 0 : 1;
}

int floppy_get_idbit(const FloppyDrive *d)
{
    return (int)((d->id_data >> (31u - (d->id_count & 31u))) & 1u);
}
