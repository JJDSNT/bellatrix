#ifndef EMU68_MACHINE_INTERNAL_H
#define EMU68_MACHINE_INTERNAL_H

#include "cpu/emu68/emu68_machine.h"

enum emu68_machine_page_class {
    EMU68_PAGE_DIRECT = 1,
    EMU68_PAGE_EXTERNAL = 2,
    EMU68_PAGE_FAULT = 3,
    EMU68_PAGE_MIXED = 4
};

#define EMU68_MACHINE_PAGE_SHIFT 16u
#define EMU68_MACHINE_PAGE_COUNT (1u << (32u - EMU68_MACHINE_PAGE_SHIFT))

extern uint8_t emu68_machine_read_pages[EMU68_MACHINE_PAGE_COUNT];
extern uint8_t emu68_machine_write_pages[EMU68_MACHINE_PAGE_COUNT];

typedef struct emu68_machine_access_result {
    emu68_region_kind_t region_kind;
    uint32_t region_id;
    uint32_t flags;
} emu68_machine_access_result_t;

emu68_status_t emu68_machine_classify_access(
    uint32_t address, uint8_t width, emu68_access_kind_t kind,
    emu68_machine_access_result_t *result);

emu68_bus_result_t emu68_machine_dispatch_access(
    emu68_bus_access_t *access);

int emu68_machine_runtime_active(void);

#endif
