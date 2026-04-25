// src/chipset/floppy/floppy_drive.h

#ifndef BELLATRIX_FLOPPY_DRIVE_H
#define BELLATRIX_FLOPPY_DRIVE_H

#include <stdint.h>

/*
 * Sinais vindos do CIA-B (PRB)
 *
 * /SELx   → seleção de drive (ativo LOW)
 * /MTR    → motor (1 = ON)
 * /STEP   → pulso de step
 * DIR     → direção (1 = +, 0 = -)
 * SIDE    → lado (0/1)
 */
typedef struct {
    int selected;   // 1 se este drive está selecionado
    int motor;      // 1 = ligado
    int step;       // pulso (edge detect externo)
    int direction;  // 1 = forward
    int side;       // 0 ou 1
} FloppySignals;

/*
 * Estado do drive (puramente lógico/mecânico)
 */
typedef struct {

    int motor;
    int cylinder;
    int side;

    int ready;
    int track0;

    int disk_inserted;
    int disk_changed;   // /DSKCHNG (latched until step)

    int step_latch;

    // ID sequence (motor OFF behavior)
    uint32_t id_data;
    int id_count;

    // mídia
    const uint8_t *adf;

} FloppyDrive;

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

void floppy_init(FloppyDrive *d);

void floppy_insert(FloppyDrive *d, const uint8_t *adf);
void floppy_eject(FloppyDrive *d);

/*
 * Step lógico (um tick de máquina)
 */
void floppy_step(FloppyDrive *d, const FloppySignals *sig);

/*
 * Sinais de saída para CIA/Paula
 */
int floppy_get_ready(const FloppyDrive *d);
int floppy_get_track0(const FloppyDrive *d);

/*
 * /DSKCHNG (ativo LOW)
 * retorna:
 *   0 = LOW (disk changed)
 *   1 = HIGH (ok)
 */
int floppy_get_dskchg(const FloppyDrive *d, int motor_on);

#endif