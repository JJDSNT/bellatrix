#include "machine/expansions/cdrom/cdrom_plugin.h"

#include "machine/expansions/cdrom/board/cdrom_board.h"
#include "machine/expansions/cdrom/cdrom_state.h"
#include "machine/expansions/cdrom/ide/cdrom_ide.h"
#include "machine/machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bellatrix_cdrom_attach(
    BellatrixExpansion *exp,
    BellatrixMachine *machine
)
{
    BellatrixCdromPluginState *state;

    (void)machine;

    state = (BellatrixCdromPluginState *)exp->desc.userdata;
    if (!state) {
        return -1;
    }

    bellatrix_cdrom_iso_backend_init(&state->iso_backend, &machine->iso);
    bellatrix_cdrom_iso_backend_get_ops(&state->iso_backend, &state->media_ops);
    bellatrix_cdrom_device_attach_media(&state->device, &state->media_ops);
    bellatrix_cdrom_device_insert(&state->device);
    bellatrix_atapi_cdrom_reset(&state->atapi);
    bellatrix_cdrom_ide_reset(&state->ide);

    return 0;
}

static void bellatrix_cdrom_reset(BellatrixExpansion *exp)
{
    BellatrixCdromPluginState *state;

    state = (BellatrixCdromPluginState *)exp->desc.userdata;
    if (!state) {
        return;
    }

    bellatrix_cdrom_device_clear_media_changed(&state->device);
    bellatrix_atapi_cdrom_reset(&state->atapi);
    bellatrix_cdrom_ide_reset(&state->ide);
}

static void bellatrix_cdrom_shutdown(BellatrixExpansion *exp)
{
    BellatrixCdromPluginState *state;

    state = (BellatrixCdromPluginState *)exp->desc.userdata;
    if (!state) {
        return;
    }

    bellatrix_cdrom_device_eject(&state->device);
    bellatrix_atapi_cdrom_reset(&state->atapi);
    bellatrix_cdrom_ide_reset(&state->ide);
}

static void bellatrix_cdrom_destroy(BellatrixExpansion *exp)
{
    BellatrixCdromPluginState *state;

    state = (BellatrixCdromPluginState *)exp->desc.userdata;
    if (!state) {
        return;
    }

    bellatrix_cdrom_board_destroy(state);
    free(state);
}

static const BellatrixExpansionOps g_cdrom_expansion_ops = {
    .attach = bellatrix_cdrom_attach,
    .reset = bellatrix_cdrom_reset,
    .shutdown = bellatrix_cdrom_shutdown,
    .destroy = bellatrix_cdrom_destroy,
};

int bellatrix_cdrom_plugin_register(
    BellatrixMachine *machine,
    const BellatrixPluginApi *api
)
{
    BellatrixCdromPluginState *state;
    BellatrixExpansionDesc desc;
    int rc;

    if (!machine || !api || !api->register_expansion || !api->plugin_root) {
        return -1;
    }

    state = (BellatrixCdromPluginState *)calloc(1, sizeof(*state));
    if (!state) {
        return -2;
    }

    snprintf(state->plugin_root,
             sizeof(state->plugin_root),
             "%s",
             api->plugin_root);

    bellatrix_cdrom_device_init(&state->device);
    bellatrix_cdrom_iso_backend_init(&state->iso_backend, NULL);
    memset(&state->media_ops, 0, sizeof(state->media_ops));
    bellatrix_atapi_cdrom_init(&state->atapi, &state->device);
    bellatrix_cdrom_ide_init(&state->ide);

    rc = bellatrix_cdrom_board_init(state, state->plugin_root);
    if (rc != 0) {
        free(state);
        return rc;
    }

    memset(&desc, 0, sizeof(desc));
    desc.id = "cdrom";
    desc.name = "CD-ROM Expansion";
    desc.kind = BELLATRIX_EXPANSION_BOARD;
    desc.priority = 100;
    desc.userdata = state;
    desc.zorro2_board = &state->board_desc;
    desc.ops = &g_cdrom_expansion_ops;

    rc = api->register_expansion(machine, &desc);
    if (rc != 0) {
        bellatrix_cdrom_board_destroy(state);
        free(state);
        return rc;
    }

    fprintf(stderr,
            "[PLUGIN:cdrom] registered Zorro II board using %s\n",
            state->resident_rom_path);

    return 0;
}

int bellatrix_plugin_init(
    BellatrixMachine *machine,
    const BellatrixPluginApi *api
)
{
    return bellatrix_cdrom_plugin_register(machine, api);
}
