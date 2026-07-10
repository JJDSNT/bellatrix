/* AAPCS64 boundary around Emu68's historically non-returning MainLoop.
 *
 * The JIT pins guest state in registers that AAPCS64 calls callee-saved.
 * Continuous upstream execution never returned, so this was unobservable.
 * Bellatrix run windows do return to C and must restore the host caller. */

    .text
    .align  4
    .global MainLoopWindow
    .type   MainLoopWindow, %function
MainLoopWindow:
    sub     sp, sp, #224
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    stp     q8, q9, [sp, #96]
    stp     q10, q11, [sp, #128]
    stp     q12, q13, [sp, #160]
    stp     q14, q15, [sp, #192]

    bl      MainLoop

    ldp     q8, q9, [sp, #96]
    ldp     q10, q11, [sp, #128]
    ldp     q12, q13, [sp, #160]
    ldp     q14, q15, [sp, #192]
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #224
    ret
    .size   MainLoopWindow, .-MainLoopWindow
