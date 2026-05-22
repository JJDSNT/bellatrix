#include "machine/expansions/cdrom/board/cdrom_board.h"

#include "machine/expansions/cdrom/ide/cdrom_ide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t bellatrix_cdrom_board_read8(void *userdata, uint32_t offset)
{
    BellatrixCdromPluginState *state;

    state = (BellatrixCdromPluginState *)userdata;
    if (!state) {
        return 0xFFu;
    }

    return bellatrix_cdrom_ide_read8(&state->ide, offset);
}

static void bellatrix_cdrom_board_write8(
    void *userdata,
    uint32_t offset,
    uint8_t value
)
{
    BellatrixCdromPluginState *state;

    state = (BellatrixCdromPluginState *)userdata;
    if (!state) {
        return;
    }

    bellatrix_cdrom_ide_write8(&state->ide, offset, value);
}

static void bellatrix_cdrom_board_reset(void *userdata)
{
    BellatrixCdromPluginState *state;

    state = (BellatrixCdromPluginState *)userdata;
    if (!state) {
        return;
    }

    bellatrix_cdrom_ide_reset(&state->ide);
}

static void bellatrix_cdrom_board_userdata_destroy(void *userdata)
{
    (void)userdata;
}

static const BellatrixZorro2BoardOps g_cdrom_board_ops = {
    .reset = bellatrix_cdrom_board_reset,
    .destroy = bellatrix_cdrom_board_userdata_destroy,
    .read8 = bellatrix_cdrom_board_read8,
    .write8 = bellatrix_cdrom_board_write8,
};

static void bellatrix_cdrom_board_emit_word_config(
    uint8_t *out,
    const uint16_t *words
)
{
    size_t i;

    for (i = 0; i < BELLATRIX_ZORRO2_CONFIG_WORDS; ++i) {
        out[i * 2] = (uint8_t)(words[i] >> 8);
        out[i * 2 + 1] = (uint8_t)(words[i] & 0xFFu);
    }
}

static void bellatrix_cdrom_board_build_config(
    BellatrixCdromPluginState *state
)
{
    static const uint16_t words[BELLATRIX_ZORRO2_CONFIG_WORDS] = {
        (uint16_t)((0xD1u << 8) & 0xF000u),
        (uint16_t)((0xD1u << 12) & 0xF000u),
        (uint16_t)(~(0x11u << 8) & 0xF000u),
        (uint16_t)(~(0x11u << 12) & 0xF000u),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x6D73u & 0xF000u),
        (uint16_t)(~(0x6D73u << 4) & 0xF000u),
        (uint16_t)(~(0x6D73u << 8) & 0xF000u),
        (uint16_t)(~(0x6D73u << 12) & 0xF000u),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~(0x10u << 8) & 0xF000u),
        (uint16_t)(~(0x10u << 12) & 0xF000u),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
    };

    bellatrix_cdrom_board_emit_word_config(state->config_data, words);
}

static int bellatrix_cdrom_board_load_resident_rom(
    BellatrixCdromPluginState *state,
    const char *plugin_root
)
{
    FILE *fp;
    long size;
    size_t read_size;

    if (!state || !plugin_root) {
        return -1;
    }

    snprintf(state->resident_rom_path,
             sizeof(state->resident_rom_path),
             "%s/resident/cdmount.rom",
             plugin_root);

    fp = fopen(state->resident_rom_path, "rb");
    if (!fp) {
        return -2;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fclose(fp);
        return -3;
    }

    state->resident_rom = (uint8_t *)calloc(1, (size_t)size);
    if (!state->resident_rom) {
        fclose(fp);
        return -4;
    }

    read_size = fread(state->resident_rom, 1, (size_t)size, fp);
    fclose(fp);

    if (read_size != (size_t)size) {
        free(state->resident_rom);
        state->resident_rom = NULL;
        return -5;
    }

    state->resident_rom_size = (size_t)size;

    return 0;
}

int bellatrix_cdrom_board_init(
    BellatrixCdromPluginState *state,
    const char *plugin_root
)
{
    int rc;

    if (!state || !plugin_root) {
        return -1;
    }

    rc = bellatrix_cdrom_board_load_resident_rom(state, plugin_root);
    if (rc != 0) {
        return rc;
    }

    bellatrix_cdrom_board_build_config(state);

    memset(&state->board_desc, 0, sizeof(state->board_desc));
    state->board_desc.id = "cdrom.z2";
    state->board_desc.config_data = state->config_data;
    state->board_desc.config_size = sizeof(state->config_data);
    state->board_desc.window_size = BELLATRIX_ZORRO2_CD_BOARD_WINDOW;
    state->board_desc.rom = state->resident_rom;
    state->board_desc.rom_size = state->resident_rom_size;
    state->board_desc.userdata = state;
    state->board_desc.ops = &g_cdrom_board_ops;

    return 0;
}

void bellatrix_cdrom_board_destroy(
    BellatrixCdromPluginState *state
)
{
    if (!state) {
        return;
    }

    free(state->resident_rom);
    state->resident_rom = NULL;
    state->resident_rom_size = 0;
}
