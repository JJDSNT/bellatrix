#ifndef BELLATRIX_AUTOCONFIG_H
#define BELLATRIX_AUTOCONFIG_H

#include <stdint.h>

/*
 * The Zorro 2 config window at $E80000 is nibble-wide: each config byte
 * occupies two consecutive byte addresses (stride 2). The nibble value sits
 * in D7-D4 of each byte; D3-D0 are don't-care (pulled low in our emulation).
 *
 * 16 autoconfig bytes → 32 nibbles → 64 config_data entries.
 */
#define AUTOCONFIG_ROM_BYTES   16u
#define AUTOCONFIG_NIBBLES     32u
#define AUTOCONFIG_DATA_SIZE   64u  /* size of the output config_data array */

/* er_Type bit fields */
#define AC_TYPE_Z2         0xC0u  /* Zorro II board type (bits 7-6 = 11) */
#define AC_TYPE_Z3         0x80u  /* Zorro III board type (bits 7-6 = 10) */
#define AC_TYPE_MEMLIST    0x20u  /* ERTF_MEMLIST: add to free memory list */
#define AC_TYPE_DIAGVALID  0x10u  /* ERTF_DIAGVALID: DiagArea ROM present  */
#define AC_TYPE_CHAIN      0x08u  /* ERTF_CHAINEDCONFIG                    */

/* Size codes for er_Type bits 2-0 */
#define AC_SIZE_8MB    0x00u
#define AC_SIZE_64KB   0x01u
#define AC_SIZE_128KB  0x02u
#define AC_SIZE_256KB  0x03u
#define AC_SIZE_512KB  0x04u
#define AC_SIZE_1MB    0x05u
#define AC_SIZE_2MB    0x06u
#define AC_SIZE_4MB    0x07u

/*
 * Config window write register offsets (byte offsets from $E80000).
 *
 * The nibble-wide config space encodes logical byte N at physical offset N*4.
 * WriteExpansionByte(board, N, v) writes to board + N*4.
 *
 *   index 17 → offset 0x44 → Z3 high byte (bits 31-24) only
 *   index 18 → offset 0x48 → ec_BaseAddress (bits 23-16); triggers assignment
 *   index 19 → offset 0x4C → ec_Shutup
 */
#define AC_OFF_Z3_HI    0x44u  /* Z3 only: latch bits 31-24 of base address */
#define AC_OFF_BASE_HI  0x48u  /* ec_BaseAddress: write board base >> 16    */
#define AC_OFF_SHUTUP   0x4Cu  /* ec_Shutup: tell board to go away          */

/*
 * autoconfig_build - encode 16 raw autoconfig bytes into the 64-byte
 * nibble-wide config_data array.
 *
 * Each source byte at index N produces:
 *   out[N*4]   = high nibble of bytes[N] in bits D7-D4
 *   out[N*4+2] = low  nibble of bytes[N] in bits D7-D4
 *   out[N*4+1] = out[N*4+3] = 0x00  (odd-offset padding)
 */
void autoconfig_build(uint8_t *out, const uint8_t *bytes);

#endif
