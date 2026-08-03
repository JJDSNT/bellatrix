/*
 * Emu68 relocates ET_REL images at runtime. Its loader places non-writable
 * allocated sections first, followed by writable allocated sections, then
 * applies the supported m68k RELA relocations.
 */
SECTIONS
{
    .text : ALIGN(4)
    {
        __aros_resident_start = .;
        KEEP(*(.text.boot))

        /*
         * Port-local ROMTags, ahead of every module's. These belong to the
         * bootstrap rather than to any module, so there is no module body for
         * them to sit at the head of; putting them first keeps them out of
         * every rt_EndSkip span there is.
         */
        KEEP(*(.aros.romtag))

        /*
         * One statement covering both, so the linker keeps each input file's
         * .text and .rodata adjacent. This is what arch/m68k-amiga/boot/
         * aros-rom.ld:15 does, and it is not cosmetic.
         *
         * krnRomTagScanner() sweeps this range linearly and, on every ROMTag
         * it finds, leaps straight to that tag's rt_EndSkip -- a symbol at the
         * end of the module the tag belongs to. Gathering all .text and then
         * all .rodata separately tears every module in two and stretches those
         * leaps across unrelated modules: dosboot's ended up spanning roughly
         * 70KB, silently swallowing every ROMTag the linker had placed in
         * between. rom/devs/sdcard's boot-wait resident was lost that way, and
         * so was boot/selftest.c's until it was moved to .aros.romtag.
         */
        *(.text .text.* .rodata .rodata.*)
        __aros_resident_end = .;
    }

    .data : ALIGN(4)
    {
        *(.data .data.*)
    }

    .bss : ALIGN(4)
    {
        *(.bss .bss.* COMMON)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
    }
}
