// src/chipset/agnus/copper/copper_service.h

#pragma once

#include <stdint.h>

/* forward declarations */

typedef struct CopperState CopperState;
typedef struct AgnusState AgnusState;

/* ------------------------------------------------------------------------- */
/* service state                                                             */
/* ------------------------------------------------------------------------- */

typedef struct CopperService
{
    int enabled;

    /*
     * Número de ciclos executados imediatamente após um wake
     * (WAIT resolvido). Isso garante que MOVEs críticos rodem
     * antes do snapshot de vídeo.
     */
    uint32_t wake_budget;

} CopperService;

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void copper_service_init(CopperService *svc);
void copper_service_reset(CopperService *svc);

/* ------------------------------------------------------------------------- */
/* execution                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Execução normal (chamada pelo agnus_step)
 */
void copper_service_step(CopperService *svc,
                         CopperState *c,
                         AgnusState *agnus,
                         uint64_t budget);

/*
 * Poll de WAIT (chamado após atualização do beam)
 */
void copper_service_poll(CopperService *svc,
                         CopperState *c,
                         AgnusState *agnus);

/*
 * Reload no VBL (substitui chamada direta ao copper_vbl_reload)
 */
void copper_service_vbl_reload(CopperService *svc,
                              CopperState *c);