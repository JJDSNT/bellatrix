#ifndef AROS_M68K_EMU68_BOOT_H
#define AROS_M68K_EMU68_BOOT_H

#include <stdint.h>

#define EMU68_BOOT_MAGIC   0x45363842UL /* "E68B" */
#define EMU68_BOOT_ABI     7

#define EMU68_BOOT_FDT_VALID       (1UL << 0)
#define EMU68_BOOT_MEMORY_VALID    (1UL << 1)
#define EMU68_BOOT_BOOTARGS_VALID  (1UL << 2)
#define EMU68_BOOT_FRAMEBUFFER     (1UL << 3)
#define EMU68_BOOT_EXEC_READY      (1UL << 4)
#define EMU68_BOOT_KERNEL_READY    (1UL << 5)
#define EMU68_BOOT_COLDSTART_READY (1UL << 6)
#define EMU68_BOOT_SCHEDULER_ENTER (1UL << 7)
#define EMU68_BOOT_TASK_RUNNING    (1UL << 8)
#define EMU68_BOOT_TIMER_DEVICE    (1UL << 10)
#define EMU68_BOOT_TIMER_TICKING   (1UL << 11)
#define EMU68_BOOT_TIMER_WAKEUP    (1UL << 12)
#define EMU68_BOOT_TIMER_COHERENT  (1UL << 13)
#define EMU68_BOOT_TIMER_CONCURRENT (1UL << 14)
#define EMU68_BOOT_TIMER_ABORT      (1UL << 15)
#define EMU68_BOOT_TIMER_PREEMPT    (1UL << 16)
#define EMU68_BOOT_TIMER_SOAK       (1UL << 17)

#define EMU68_STAGE_ENTRY          0x45303031UL /* "E001" */
#define EMU68_STAGE_EXEC_READY     0x45303032UL /* "E002" */
#define EMU68_STAGE_SINGLETASK     0x45303033UL /* "E003" */
#define EMU68_STAGE_KERNEL_READY   0x45303034UL /* "E004" */
#define EMU68_STAGE_COLDSTART      0x45303035UL /* "E005" */
#define EMU68_STAGE_MULTITASKING   0x45303036UL /* "E006" */
#define EMU68_STAGE_SCHEDULER      0x45303037UL /* "E007" */
#define EMU68_STAGE_TASK_RUNNING   0x45303038UL /* "E008" */
#define EMU68_STAGE_TIMER_RUNNING  0x45303039UL /* "E009" */
#define EMU68_STAGE_TIMER_DEVICE   0x45303130UL /* "E010" */
#define EMU68_STAGE_TIMER_WAKEUP   0x45303131UL /* "E011" */
#define EMU68_STAGE_TIMER_COHERENT 0x45303132UL /* "E012" */
#define EMU68_STAGE_TIMER_CONCURRENT 0x45303133UL /* "E013" */
#define EMU68_STAGE_TIMER_ABORT    0x45303134UL /* "E014" */
#define EMU68_STAGE_TIMER_SOAK     0x45303135UL /* "E015" */
#define EMU68_STAGE_TIMER_MINUTE1  0x45303136UL /* "E016" */
#define EMU68_STAGE_TIMER_MINUTE2  0x45303137UL /* "E017" */
#define EMU68_STAGE_TIMER_COMPLETE 0x45303138UL /* "E018" */
#define EMU68_STAGE_SCHED_RETURN   0x45303139UL /* "E019" */

struct Emu68BootContext
{
    uint32_t magic;
    uint32_t abi_version;
    uint32_t flags;

    const void *fdt;
    uint32_t fdt_size;

    void *framebuffer;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;

    uint32_t memory_base;
    uint32_t memory_size;
    /* End of Emu68's own memory pools, from /emu68 host-mem. Zero if the
     * running Emu68 does not publish it. */
    uint32_t host_mem_end;

    const char *bootargs;
    uint32_t bootargs_size;

    uint32_t timer_soak_seconds;

    void *exec_base;
    uint32_t stage;
};

extern struct Emu68BootContext emu68_boot_context;

void emu68_bootstrap(const void *fdt, void *framebuffer, uint32_t pitch,
                     uint32_t width, uint32_t height)
    __attribute__((noreturn));

void emu68_console_init(void *framebuffer, uint32_t pitch,
                        uint32_t width, uint32_t height);
int emu68_console_putc(int chr);
void emu68_console_puts(const char *text);
void emu68_set_stage(uint32_t stage);

/* Exec jump-table watch -- boot/trapprobe.c. Armed once ExecBase exists,
 * checked from the platform timer tick. */
void emu68_lvo_guard_arm(void);
void emu68_lvo_guard_check(void);

#endif
