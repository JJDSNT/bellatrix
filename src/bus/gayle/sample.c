/*
 * Example integration between:
 *
 *   Gayle IDE transport
 *       ↓
 *   Generic ATAPI CD-ROM device
 *       ↓
 *   ISO backend
 *
 * This is NOT a final implementation.
 * It demonstrates the architectural boundaries.
 */

#include "bus/gayle/gayle_ide.h"

#include "expansions/cdrom/media/cdrom_device.h"
#include "expansions/cdrom/media/cdrom_atapi.h"
#include "expansions/cdrom/media/cdrom_iso_backend.h"

#include "storage/iso/iso_image.h"

/* ------------------------------------------------------------------------- */
/* Global device instances                                                    */
/* ------------------------------------------------------------------------- */

static iso_image_t                g_iso;
static BellatrixCdromIsoBackend   g_iso_backend;
static BellatrixCdromMediaOps     g_iso_ops;

static BellatrixCdromDevice       g_cdrom;
static BellatrixAtapiCdrom        g_atapi;

/* ------------------------------------------------------------------------- */
/* Initialization                                                             */
/* ------------------------------------------------------------------------- */

bool bellatrix_cdrom_mount_iso(
    const uint8_t *iso_data,
    uint32_t iso_size
)
{
    /*
     * Example only.
     *
     * Replace with your actual ISO mounting logic.
     */

    if (!iso_image_init(&g_iso, iso_data, iso_size))
        return false;

    bellatrix_cdrom_iso_backend_init(&g_iso_backend, &g_iso);

    bellatrix_cdrom_iso_backend_get_ops(
        &g_iso_backend,
        &g_iso_ops
    );

    bellatrix_cdrom_device_init(&g_cdrom);

    bellatrix_cdrom_device_attach_media(
        &g_cdrom,
        &g_iso_ops
    );

    bellatrix_atapi_cdrom_init(
        &g_atapi,
        &g_cdrom
    );

    return true;
}

/* ------------------------------------------------------------------------- */
/* PACKET command handling                                                    */
/* ------------------------------------------------------------------------- */

bool gayle_atapi_packet(
    const uint8_t *packet,
    uint8_t **data,
    uint32_t *data_len,
    bool *check_condition
)
{
    BellatrixAtapiResult result;

    if (!bellatrix_atapi_cdrom_execute_packet(
            &g_atapi,
            packet,
            12,
            &result
        )) {

        *check_condition = result.check_condition;
        *data            = NULL;
        *data_len        = 0;

        return false;
    }

    *data            = (uint8_t *)result.transfer_buffer;
    *data_len        = result.transfer_length;
    *check_condition = result.check_condition;

    return true;
}
