// src/chipset/floppy/floppy_drive.h

#ifndef BELLATRIX_FLOPPY_DRIVE_H
#define BELLATRIX_FLOPPY_DRIVE_H

#include <stdint.h>

/*
 * Standard Amiga DD ADF geometry.
 *
 * This is the logical disk image geometry, not the raw MFM stream.
 */
#define FLOPPY_ADF_DD_SIZE            901120u
#define FLOPPY_ADF_SECTOR_SIZE       512u
#define FLOPPY_ADF_SECTORS_PER_TRACK 11u
#define FLOPPY_ADF_SIDES             2u
#define FLOPPY_ADF_CYLINDERS         80u

/*
 * Sinais vindos do CIA-B (PRB)
 *
 * /SELx   -> seleção de drive (ativo LOW no hardware, já normalizado aqui)
 * /MTR    -> motor (1 = ON, já normalizado aqui)
 * /STEP   -> pulso de step
 * DIR     -> direção (1 = +, 0 = -)
 * SIDE    -> lado (0/1)
 */
typedef struct {
    int selected;   /* 1 se este drive está selecionado */
    int motor;      /* 1 = ligado */
    int step;       /* pulso */
    int direction;  /* 1 = forward */
    int side;       /* 0 ou 1 */
} FloppySignals;

/*
 * Estado do drive.
 *
 * Este struct mistura:
 * - estado mecânico/lógico do drive;
 * - estado da mídia inserida;
 * - cursor inicial simplificado de leitura linear do ADF.
 *
 * A leitura linear é propositalmente temporária: serve para fechar o ciclo
 * inicial DSKLEN -> ADF -> Chip RAM -> DSKBLK antes da emulação MFM/trilha.
 */
typedef struct {

    int motor;
    int cylinder;
    int side;

    int ready;
    int track0;

    int disk_inserted;
    int disk_changed;   /* /DSKCHG latched until step */

    int step_latch;

    /*
     * ID sequence.
     *
     * Alguns drives retornam uma sequência de identificação quando
     * desselicionados com motor desligado.
     */
    uint32_t id_data;
    int id_count;

    /*
     * Mídia ADF montada.
     *
     * O buffer pertence ao harness/machine, não ao drive.
     * O drive apenas referencia esse conteúdo.
     */
    const uint8_t *adf;
    uint32_t adf_size;

    /*
     * Cursor simplificado para leitura linear inicial.
     *
     * Não representa ainda a posição física real do cabeçote em fluxo MFM.
     */
    uint32_t read_offset;

} FloppyDrive;

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

void floppy_init(FloppyDrive *d);

void floppy_insert(FloppyDrive *d, const uint8_t *adf, uint32_t adf_size);
void floppy_eject(FloppyDrive *d);

/*
 * Step lógico do drive.
 */
void floppy_step(FloppyDrive *d, const FloppySignals *sig);

/*
 * Estado de mídia.
 */
int floppy_has_media(const FloppyDrive *d);

/*
 * Leitura simplificada inicial.
 *
 * Retorna a quantidade de bytes copiados.
 */
uint32_t floppy_read_linear(
    FloppyDrive *d,
    uint8_t *dst,
    uint32_t bytes
);

/*
 * Sinais de saída para CIA/Paula.
 */
int floppy_get_ready(const FloppyDrive *d);
int floppy_get_track0(const FloppyDrive *d);

/*
 * /DSKCHG ativo LOW.
 *
 * Retorna:
 *   0 = LOW  = disk changed / no disk acknowledged
 *   1 = HIGH = stable
 */
int floppy_get_dskchg(const FloppyDrive *d, int motor_on);

int floppy_get_idbit(const FloppyDrive *d);

#endif