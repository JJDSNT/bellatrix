/* Explicit slow bridge used by generated guest-memory code.
 *
 * Arguments and results use private mailboxes so the generated code does not
 * have to clobber ABI argument registers before they are saved. The bridge
 * preserves every JIT-visible GPR, SIMD register and status register. Normal
 * external traffic therefore crosses an ordinary call boundary, never an ARM
 * exception boundary.
 */

    .text
    .align  4
    .global emu68_machine_native_bridge
    .type   emu68_machine_native_bridge, %function
emu68_machine_native_bridge:
    sub     sp, sp, #800
    str     x0,       [sp, #768]
    str     x1,       [sp, #776]
    str     x30,      [sp, #784]
    stp     x2,  x3,  [sp, #0]
    stp     x4,  x5,  [sp, #16]
    stp     x6,  x7,  [sp, #32]
    stp     x8,  x9,  [sp, #48]
    stp     x10, x11, [sp, #64]
    stp     x12, x13, [sp, #80]
    stp     x14, x15, [sp, #96]
    stp     x16, x17, [sp, #112]
    stp     x18, x19, [sp, #128]
    stp     x20, x21, [sp, #144]
    stp     x22, x23, [sp, #160]
    stp     x24, x25, [sp, #176]
    stp     x26, x27, [sp, #192]
    stp     x28, x29, [sp, #208]
    str     x30,      [sp, #224]
    mrs     x3, nzcv
    mrs     x4, fpcr
    mrs     x5, fpsr
    stp     x3, x4,   [sp, #232]
    str     x5,       [sp, #248]

    stp     q0,  q1,  [sp, #256]
    stp     q2,  q3,  [sp, #288]
    stp     q4,  q5,  [sp, #320]
    stp     q6,  q7,  [sp, #352]
    stp     q8,  q9,  [sp, #384]
    stp     q10, q11, [sp, #416]
    stp     q12, q13, [sp, #448]
    stp     q14, q15, [sp, #480]
    stp     q16, q17, [sp, #512]
    stp     q18, q19, [sp, #544]
    stp     q20, q21, [sp, #576]
    stp     q22, q23, [sp, #608]
    stp     q24, q25, [sp, #640]
    stp     q26, q27, [sp, #672]
    stp     q28, q29, [sp, #704]
    stp     q30, q31, [sp, #736]

    adrp    x6, emu68_machine_bridge_tu_return
    ldr     x6, [x6, #:lo12:emu68_machine_bridge_tu_return]
    str     x6, [sp, #224]
    adrp    x6, emu68_machine_bridge_address
    ldr     w0, [x6, #:lo12:emu68_machine_bridge_address]
    adrp    x6, emu68_machine_bridge_write_value
    ldr     x1, [x6, #:lo12:emu68_machine_bridge_write_value]
    adrp    x6, emu68_machine_bridge_metadata
    ldr     w2, [x6, #:lo12:emu68_machine_bridge_metadata]
    mov     x3, x30
    mov     x4, sp
    bl      emu68_machine_bridge_dispatch

    adrp    x6, emu68_machine_bridge_read_value
    str     x0, [x6, #:lo12:emu68_machine_bridge_read_value]
    adrp    x6, emu68_machine_bridge_outcome
    str     x1, [x6, #:lo12:emu68_machine_bridge_outcome]

    ldp     q0,  q1,  [sp, #256]
    ldp     q2,  q3,  [sp, #288]
    ldp     q4,  q5,  [sp, #320]
    ldp     q6,  q7,  [sp, #352]
    ldp     q8,  q9,  [sp, #384]
    ldp     q10, q11, [sp, #416]
    ldp     q12, q13, [sp, #448]
    ldp     q14, q15, [sp, #480]
    ldp     q16, q17, [sp, #512]
    ldp     q18, q19, [sp, #544]
    ldp     q20, q21, [sp, #576]
    ldp     q22, q23, [sp, #608]
    ldp     q24, q25, [sp, #640]
    ldp     q26, q27, [sp, #672]
    ldp     q28, q29, [sp, #704]
    ldp     q30, q31, [sp, #736]

    ldp     x3, x4, [sp, #232]
    ldr     x5,     [sp, #248]
    msr     nzcv, x3
    msr     fpcr, x4
    msr     fpsr, x5
    ldp     x2,  x3,  [sp, #0]
    ldp     x4,  x5,  [sp, #16]
    ldp     x6,  x7,  [sp, #32]
    ldp     x8,  x9,  [sp, #48]
    ldp     x10, x11, [sp, #64]
    ldp     x12, x13, [sp, #80]
    ldp     x14, x15, [sp, #96]
    ldp     x16, x17, [sp, #112]
    ldp     x18, x19, [sp, #128]
    ldp     x20, x21, [sp, #144]
    ldp     x22, x23, [sp, #160]
    ldp     x24, x25, [sp, #176]
    ldp     x26, x27, [sp, #192]
    ldp     x28, x29, [sp, #208]
    ldr     x0,       [sp, #768]
    ldr     x1,       [sp, #776]
    ldr     x30,      [sp, #784]
    add     sp, sp, #800
    ret

    .size emu68_machine_native_bridge, .-emu68_machine_native_bridge

    .align  4
    .global emu68_machine_resume_native
    .type   emu68_machine_resume_native, %function
emu68_machine_resume_native:
    sub     sp, sp, #16
    adrp    x9, emu68_machine_resume_frame
    ldr     x9, [x9, #:lo12:emu68_machine_resume_frame]

    adrp    x10, emu68_machine_bridge_read_value
    adrp    x11, emu68_machine_resume_value
    ldr     x12, [x11, #:lo12:emu68_machine_resume_value]
    str     x12, [x10, #:lo12:emu68_machine_bridge_read_value]
    adrp    x10, emu68_machine_bridge_outcome
    adrp    x11, emu68_machine_resume_outcome
    ldr     x12, [x11, #:lo12:emu68_machine_resume_outcome]
    str     x12, [x10, #:lo12:emu68_machine_bridge_outcome]
    adrp    x10, emu68_machine_resume_address
    ldr     x30, [x10, #:lo12:emu68_machine_resume_address]
    str     x30, [sp]

    ldp     x10, x11, [x9, #232]
    ldr     x12,      [x9, #248]
    msr     nzcv, x10
    msr     fpcr, x11
    msr     fpsr, x12

    ldp     q0,  q1,  [x9, #256]
    ldp     q2,  q3,  [x9, #288]
    ldp     q4,  q5,  [x9, #320]
    ldp     q6,  q7,  [x9, #352]
    ldp     q8,  q9,  [x9, #384]
    ldp     q10, q11, [x9, #416]
    ldp     q12, q13, [x9, #448]
    ldp     q14, q15, [x9, #480]
    ldp     q16, q17, [x9, #512]
    ldp     q18, q19, [x9, #544]
    ldp     q20, q21, [x9, #576]
    ldp     q22, q23, [x9, #608]
    ldp     q24, q25, [x9, #640]
    ldp     q26, q27, [x9, #672]
    ldp     q28, q29, [x9, #704]
    ldp     q30, q31, [x9, #736]

    ldp     x2,  x3,  [x9, #0]
    ldp     x4,  x5,  [x9, #16]
    ldp     x6,  x7,  [x9, #32]
    ldr     x8,       [x9, #48]
    ldr     x10,      [x9, #64]
    ldr     x11,      [x9, #72]
    ldr     x12,      [x9, #80]
    ldr     x13,      [x9, #88]
    ldp     x14, x15, [x9, #96]
    ldp     x16, x17, [x9, #112]
    ldp     x18, x19, [x9, #128]
    ldp     x20, x21, [x9, #144]
    ldp     x22, x23, [x9, #160]
    ldp     x24, x25, [x9, #176]
    ldp     x26, x27, [x9, #192]
    ldp     x28, x29, [x9, #208]
    ldr     x0,       [x9, #768]
    ldr     x1,       [x9, #776]
    ldr     x9,       [x9, #56]
    ldr     x30, [sp]
    add     sp, sp, #16
    br      x30

    .size emu68_machine_resume_native, .-emu68_machine_resume_native
