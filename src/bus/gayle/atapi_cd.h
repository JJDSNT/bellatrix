// src/bus/gayle/atapi_cd.h

#ifndef BELLATRIX_BUS_GAYLE_ATAPI_CD_H
#define BELLATRIX_BUS_GAYLE_ATAPI_CD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "storage/iso/iso_image.h"

#define ATAPI_PACKET_SIZE 12u

typedef enum AtapiPhase {
    ATAPI_PHASE_IDLE = 0,
    ATAPI_PHASE_PACKET_IN,
    ATAPI_PHASE_DATA_IN,
    ATAPI_PHASE_DATA_OUT,
    ATAPI_PHASE_STATUS
} AtapiPhase;

typedef struct AtapiCd {
    IsoImage *iso;

    AtapiPhase phase;

    uint8_t packet[ATAPI_PACKET_SIZE];
    uint8_t packet_pos;

    uint8_t *data;
    size_t data_capacity;
    size_t data_len;
    size_t data_pos;

    uint8_t sense_key;
    uint8_t asc;
    uint8_t ascq;

    bool media_changed;
} AtapiCd;

void atapi_cd_init(AtapiCd *cd, IsoImage *iso, uint8_t *buffer, size_t buffer_size);

void atapi_cd_reset(AtapiCd *cd);

bool atapi_cd_media_present(const AtapiCd *cd);

bool atapi_cd_media_changed(const AtapiCd *cd);

void atapi_cd_clear_media_changed(AtapiCd *cd);

void atapi_cd_begin_packet(AtapiCd *cd);

bool atapi_cd_write_packet_word(AtapiCd *cd, uint16_t word);

bool atapi_cd_has_data(const AtapiCd *cd);

uint16_t atapi_cd_read_data_word(AtapiCd *cd);

size_t atapi_cd_remaining_data(const AtapiCd *cd);

uint8_t atapi_cd_status(const AtapiCd *cd);

uint8_t atapi_cd_error(const AtapiCd *cd);

#endif