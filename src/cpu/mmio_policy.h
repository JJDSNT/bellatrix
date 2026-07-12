#ifndef BELLATRIX_CPU_MMIO_POLICY_H
#define BELLATRIX_CPU_MMIO_POLICY_H

#include <stdint.h>

typedef enum BellatrixMmioPolicy {
    BELLATRIX_MMIO_DIRECT = 0,
    BELLATRIX_MMIO_POSTED = 1,
    BELLATRIX_MMIO_SYNC = 2,
    BELLATRIX_MMIO_PUBLISHED = 3,
} BellatrixMmioPolicy;

BellatrixMmioPolicy bellatrix_mmio_policy(uint32_t normalized_addr,
                                          unsigned int size,
                                          int is_write);

#endif
