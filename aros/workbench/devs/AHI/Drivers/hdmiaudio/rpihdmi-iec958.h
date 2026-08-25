#ifndef RPIHDMI_MAIWORD_H
#define RPIHDMI_MAIWORD_H

#include <exec/types.h>

/*
 * Convert an AHI mix buffer (signed 16-bit stereo) into MAI FIFO words.
 * One word per channel sample: left, right, left, ...
 */
void convert_mix_to_mai(WORD *src, ULONG *dst, ULONG frames);

#endif /* RPIHDMI_MAIWORD_H */
