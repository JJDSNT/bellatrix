#ifndef EMU68_MACHINE_EMIT_H
#define EMU68_MACHINE_EMIT_H

#include <stdint.h>

enum {
    EMU68_MACHINE_META_PROGRAM = 1u << 9,
    EMU68_MACHINE_META_CPU = 2u << 9,
    EMU68_MACHINE_META_USE_SFC = 1u << 14,
    EMU68_MACHINE_META_USE_DFC = 1u << 15
};

extern uint32_t emu68_machine_translation_metadata;

uint32_t *emu68_machine_emit_load(uint32_t *ptr, uint8_t address_reg,
                                  uint8_t value_reg, uint8_t width,
                                  int sign_extend, uint32_t metadata);
uint32_t *emu68_machine_emit_store(uint32_t *ptr, uint8_t address_reg,
                                   uint8_t value_reg, uint8_t width,
                                   uint32_t metadata);
uint32_t *emu68_machine_emit_load_offset(
    uint32_t *ptr, uint8_t base_reg, int32_t offset, uint8_t value_reg,
    uint8_t width, int sign_extend, uint32_t metadata);
uint32_t *emu68_machine_emit_store_offset(
    uint32_t *ptr, uint8_t base_reg, int32_t offset, uint8_t value_reg,
    uint8_t width, uint32_t metadata);

#endif
