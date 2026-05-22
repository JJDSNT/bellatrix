#ifndef BELLATRIX_MACHINE_BUS_H
#define BELLATRIX_MACHINE_BUS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct BellatrixMachine BellatrixMachine;

typedef enum BusAccessSize {
    BUS_SIZE_BYTE = 1,
    BUS_SIZE_WORD = 2,
    BUS_SIZE_LONG = 4
} BusAccessSize;

typedef struct BellatrixBus {
    BellatrixMachine *machine;

    bool trace_enabled;
} BellatrixBus;

void bellatrix_bus_init(BellatrixBus *bus,
                        BellatrixMachine *machine);

void bellatrix_bus_reset(BellatrixBus *bus);

uint32_t bellatrix_bus_read(BellatrixBus *bus,
                            uint32_t addr,
                            BusAccessSize size);

void bellatrix_bus_write(BellatrixBus *bus,
                         uint32_t addr,
                         uint32_t value,
                         BusAccessSize size);

uint8_t bellatrix_bus_read8(BellatrixBus *bus, uint32_t addr);
uint16_t bellatrix_bus_read16(BellatrixBus *bus, uint32_t addr);
uint32_t bellatrix_bus_read32(BellatrixBus *bus, uint32_t addr);

void bellatrix_bus_write8(BellatrixBus *bus, uint32_t addr, uint8_t value);
void bellatrix_bus_write16(BellatrixBus *bus, uint32_t addr, uint16_t value);
void bellatrix_bus_write32(BellatrixBus *bus, uint32_t addr, uint32_t value);

#endif
