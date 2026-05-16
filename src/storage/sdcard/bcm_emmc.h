// src/storage/sdcard/bcm_emmc.h
// Bare-metal BCM2837 (RPi 3) EMMC block driver.
// Polling mode, read-only, single-block reads.

#pragma once
#include <stdint.h>
#include <stdbool.h>

// Returns true on success
bool bcm_emmc_init(void);

// Read one 512-byte sector into buf.  lba is the logical block address.
bool bcm_emmc_read_block(uint32_t lba, void *buf);

// Read nsectors consecutive 512-byte sectors
bool bcm_emmc_read_blocks(uint32_t lba, void *buf, uint32_t nsectors);
