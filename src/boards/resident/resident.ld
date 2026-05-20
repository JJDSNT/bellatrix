/* src/boards/resident/resident.ld
 *
 * Minimal linker script for the CD automount ROM.
 *
 * Layout is controlled by source order in resident.c, not by this script:
 *   - Two global __asm__ blocks at the top of resident.c emit the romhdr
 *     (16 × 0xFF) and DiagArea at binary offsets 0x0000 and 0x0010.
 *   - diag_point() is the first C function, landing at 0x0020.
 *   - Everything else follows.
 *
 * The AmigaOS HUNK format produced by m68k-amigaos-ld ignores absolute VMAs
 * when multiple sections share address 0.  We keep a single .text output
 * section so objcopy -O binary produces one flat HUNK with the right layout.
 */

SECTIONS
{
    .text 0x00000000 :
    {
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(2);
    }

    /DISCARD/ : { *(.comment) *(.debug*) *(.note*) }
}
