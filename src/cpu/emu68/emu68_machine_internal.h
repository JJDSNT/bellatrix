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
#define EMU68_MACHINE_PAGE_SIZE (1u << EMU68_MACHINE_PAGE_SHIFT)
#define EMU68_MACHINE_PAGE_COUNT (1u << (32u - EMU68_MACHINE_PAGE_SHIFT))

extern uint8_t emu68_machine_read_pages[EMU68_MACHINE_PAGE_COUNT];
extern uint8_t emu68_machine_write_pages[EMU68_MACHINE_PAGE_COUNT];

typedef struct emu68_machine_access_result {
    emu68_region_kind_t region_kind;
    uint32_t region_id;
    uint32_t flags;
} emu68_machine_access_result_t;

typedef enum emu68_machine_bridge_outcome {
    EMU68_BRIDGE_COMPLETE = 0,
    EMU68_BRIDGE_PENDING = 1,
    EMU68_BRIDGE_BUS_ERROR = 2
} emu68_machine_bridge_outcome_t;

typedef struct emu68_machine_bridge_result {
    uint64_t value;
    uint64_t outcome;
} emu68_machine_bridge_result_t;

enum {
    EMU68_BRIDGE_META_WIDTH_MASK = 0xffu,
    EMU68_BRIDGE_META_WRITE = 1u << 8,
    EMU68_BRIDGE_META_SPACE_SHIFT = 9,
    EMU68_BRIDGE_META_SPACE_MASK = 3u << EMU68_BRIDGE_META_SPACE_SHIFT,
    EMU68_BRIDGE_META_FC_SHIFT = 11,
    EMU68_BRIDGE_META_FC_MASK = 7u << EMU68_BRIDGE_META_FC_SHIFT
};

emu68_status_t emu68_machine_classify_access(
    uint32_t address, uint8_t width, emu68_access_kind_t kind,
    emu68_machine_access_result_t *result);

emu68_bus_result_t emu68_machine_dispatch_access(
    emu68_bus_access_t *access);

emu68_machine_bridge_result_t emu68_machine_bridge_dispatch(
    uint32_t address, uint64_t value, uint32_t metadata,
    uintptr_t native_resume, const void *native_frame);

emu68_machine_bridge_result_t emu68_machine_native_bridge(
    uint32_t address, uint64_t value, uint32_t metadata);

extern uint8_t *emu68_machine_resume_frame;
extern uintptr_t emu68_machine_resume_address;
extern uint64_t emu68_machine_resume_value;
extern uint64_t emu68_machine_resume_outcome;
extern uint32_t emu68_machine_bridge_address;
extern uint64_t emu68_machine_bridge_write_value;
extern uint32_t emu68_machine_bridge_metadata;
extern uintptr_t emu68_machine_bridge_tu_return;
extern uint64_t emu68_machine_bridge_read_value;
extern uint64_t emu68_machine_bridge_outcome;

int emu68_machine_prepare_native_resume(void);
int emu68_machine_native_access_pending(void);
int emu68_machine_native_bus_error_pending(void);
int emu68_machine_dispatch_quantum_progress(uint64_t retired_instructions,
                                            uint32_t pc);
void emu68_machine_resume_native(void);

int emu68_machine_runtime_active(void);

#endif
