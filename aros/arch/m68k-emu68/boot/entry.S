/*
 * Initial Emu68 entry point.
 *
 * The native (non-PiStorm) Emu68 initramfs loader starts an m68k ELF with:
 *   A6 = flattened device tree
 *   A7 = supervisor stack
 *   D0 = framebuffer pitch
 *   D1 = framebuffer width
 *   D2 = framebuffer height
 *   A0 = framebuffer address
 *
 * A Bellatrix-patched Emu68 adds, at the moment the first m68k instruction
 * runs (patches/emu68/0011):
 *   D3 = SoC free-running microsecond counter
 *   D4 = 0xB0071115, so a stock Emu68's leftover registers are not mistaken
 *        for an origin
 *
 * Emu68 loads ET_REL images by section and enters at the beginning of the
 * read-only allocation. Keep this stub first in .text.boot.
 */

	.section .text.boot,"ax",@progbits
	.balign 4
	.globl _start
	.type _start,@function

_start:
	/*
	 * GCC's m68k ABI passes arguments on the stack, right to left.
	 * Preserve Emu68's register ABI in a normal C call:
	 *
	 * emu68_bootstrap(fdt, framebuffer, pitch, width, height,
	 *                 boot_us, boot_stamp)
	 */
	move.l	%d4,-(%sp)
	move.l	%d3,-(%sp)
	move.l	%d2,-(%sp)
	move.l	%d1,-(%sp)
	move.l	%d0,-(%sp)
	move.l	%a0,-(%sp)
	move.l	%a6,-(%sp)
	jsr	emu68_bootstrap
	lea	28(%sp),%sp

	/*
	 * emu68_bootstrap() is noreturn. Keep a safe fallback if that contract
	 * is ever accidentally broken.
	 */
1:
	stop	#0x2700
	bra.s	1b

	.size _start,.-_start
