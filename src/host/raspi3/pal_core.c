// src/host/raspi3/pal_core.c
//
// Bellatrix host runtime for Raspberry Pi 3.
//
// New design goals:
//   - pal_core is no longer a VBL shim
//   - pal_core becomes the host-side runtime/synchronization layer
//   - chipset time is owned by the Bellatrix runtime, not by a fixed 50 Hz loop
//   - multicore is optional and runtime-configurable
//
// Transitional compatibility:
//   - keeps legacy PAL_* entry points so the rest of the tree can migrate in stages
//   - provides weak runtime hooks so this file can be introduced before the
//     full Bellatrix runtime exists
//
// Expected next steps elsewhere in the project:
//   - implement bellatrix_runtime_host_init()
//   - implement bellatrix_runtime_host_step()
//   - implement bellatrix_runtime_notify_cpu_progress()
//   - implement bellatrix_runtime_get_pending_ipl()
//   - wire single-core polling into the CPU/JIT side when multicore is disabled

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "pal.h"

// ---------------------------------------------------------------------------
// BCM2836 local interrupt controller (RPi3)
//
// Physical address: 0x40000000, accessed through the TTBR1 kernel mapping.
// ---------------------------------------------------------------------------
#define LOCAL_INTC_BASE        (0xFFFFFF9000000000ULL + 0x40000000UL)

// ---------------------------------------------------------------------------
// Runtime roles / policies
// ---------------------------------------------------------------------------
enum {
    BELLATRIX_CORE_ROLE_CPU     = 0,
    BELLATRIX_CORE_ROLE_CHIPSET = 1,
};

enum {
    BELLATRIX_PRIO_NORMAL   = 0,
    BELLATRIX_PRIO_HIGH     = 1,
    BELLATRIX_PRIO_REALTIME = 2,
};

// ---------------------------------------------------------------------------
// Host runtime state
// ---------------------------------------------------------------------------
struct BellatrixPalRuntime {
    // Host timing
    uint64_t host_counter_freq;

    // Control
    atomic_uint runtime_ready;
    atomic_uint runtime_running;
    atomic_uint multicore_enabled;
    atomic_uint chipset_core_started;

    // Policy / configuration
    atomic_uint cpu_core_id;
    atomic_uint chipset_core_id;
    atomic_uint cpu_priority;
    atomic_uint chipset_priority;

    // Cross-domain state publication
    atomic_uint pending_ipl;
};

static struct BellatrixPalRuntime s_rt = {
    .host_counter_freq    = 0,
    .runtime_ready        = 0,
    .runtime_running      = 0,
    .multicore_enabled    = 0,  // default: single-core mode
    .chipset_core_started = 0,
    .cpu_core_id          = 0,
    .chipset_core_id      = 1,
    .cpu_priority         = BELLATRIX_PRIO_HIGH,
    .chipset_priority     = BELLATRIX_PRIO_REALTIME,
    .pending_ipl          = 0,
};

// ---------------------------------------------------------------------------
// Weak hooks to the new Bellatrix runtime.
//
// These allow pal_core.c to land before the new scheduler/runtime is fully
// implemented. Once the real runtime exists, its strong definitions override
// these fallbacks automatically.
// ---------------------------------------------------------------------------
__attribute__((weak))
void bellatrix_runtime_host_init(uint64_t host_counter_freq)
{
    (void)host_counter_freq;
}

__attribute__((weak))
void bellatrix_runtime_host_shutdown(void)
{
}

__attribute__((weak))
void bellatrix_runtime_host_step(uint64_t host_now, uint64_t host_freq)
{
    (void)host_now;
    (void)host_freq;
}

__attribute__((weak))
void bellatrix_runtime_notify_cpu_progress(uint32_t cycles)
{
    (void)cycles;
}

__attribute__((weak))
uint32_t bellatrix_runtime_get_pending_ipl(void)
{
    return atomic_load_explicit(&s_rt.pending_ipl, memory_order_acquire);
}

__attribute__((weak))
void bellatrix_runtime_mmio_barrier(void)
{
}

__attribute__((weak))
void bellatrix_runtime_set_multicore_enabled(int enabled)
{
    (void)enabled;
}

// ---------------------------------------------------------------------------
// Low-level host timing helpers
// ---------------------------------------------------------------------------
static inline uint64_t pal_read_cntpct(void)
{
    uint64_t v;
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(v));
    return v;
}

static inline uint64_t pal_read_cntfrq(void)
{
    uint64_t v;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(v));
    return v ? v : 62500000ULL; // QEMU raspi3b default fallback
}

static inline void pal_sev(void)
{
    asm volatile("sev" ::: "memory");
}

static inline void pal_wfe(void)
{
    asm volatile("wfe");
}

static inline void pal_dsb_sy(void)
{
    asm volatile("dsb sy" ::: "memory");
}

static inline void pal_dmb_ish(void)
{
    asm volatile("dmb ish" ::: "memory");
}

// ---------------------------------------------------------------------------
// Policy setters
//
// These are intentionally lightweight here. Real affinity / priority
// programming can be added later once the bootstrap / scheduler integration
// is finalized.
// ---------------------------------------------------------------------------
void PAL_Core_SetMulticoreEnabled(int enabled)
{
    atomic_store_explicit(&s_rt.multicore_enabled, enabled ? 1u : 0u, memory_order_release);
    bellatrix_runtime_set_multicore_enabled(enabled ? 1 : 0);
}

int PAL_Core_IsMulticoreEnabled(void)
{
    return (int)atomic_load_explicit(&s_rt.multicore_enabled, memory_order_acquire);
}

void PAL_Core_SetAffinity(uint32_t role, uint32_t core_id)
{
    if (role == BELLATRIX_CORE_ROLE_CPU) {
        atomic_store_explicit(&s_rt.cpu_core_id, core_id, memory_order_release);
    } else if (role == BELLATRIX_CORE_ROLE_CHIPSET) {
        atomic_store_explicit(&s_rt.chipset_core_id, core_id, memory_order_release);
    }
}

void PAL_Core_SetPriority(uint32_t role, uint32_t priority)
{
    if (role == BELLATRIX_CORE_ROLE_CPU) {
        atomic_store_explicit(&s_rt.cpu_priority, priority, memory_order_release);
    } else if (role == BELLATRIX_CORE_ROLE_CHIPSET) {
        atomic_store_explicit(&s_rt.chipset_priority, priority, memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// Runtime lifecycle
// ---------------------------------------------------------------------------
static void pal_runtime_init_once(void)
{
    if (atomic_load_explicit(&s_rt.runtime_ready, memory_order_acquire))
        return;

    s_rt.host_counter_freq = pal_read_cntfrq();

    bellatrix_runtime_host_init(s_rt.host_counter_freq);

    atomic_store_explicit(&s_rt.runtime_running, 1u, memory_order_release);
    atomic_store_explicit(&s_rt.runtime_ready, 1u, memory_order_release);
}

void PAL_Runtime_Init(void)
{
    pal_runtime_init_once();
}

void PAL_Runtime_Shutdown(void)
{
    atomic_store_explicit(&s_rt.runtime_running, 0u, memory_order_release);
    bellatrix_runtime_host_shutdown();
}

uint64_t PAL_Time_ReadCounter(void)
{
    return pal_read_cntpct();
}

uint64_t PAL_Time_GetFrequency(void)
{
    if (!s_rt.host_counter_freq)
        s_rt.host_counter_freq = pal_read_cntfrq();
    return s_rt.host_counter_freq;
}

// ---------------------------------------------------------------------------
// Single-core path
//
// When multicore is disabled, the CPU/JIT side should call PAL_Runtime_Poll()
// periodically (for example after a JIT block or in a bus/MMIO boundary).
// ---------------------------------------------------------------------------
void PAL_Runtime_Poll(void)
{
    if (!atomic_load_explicit(&s_rt.runtime_ready, memory_order_acquire))
        pal_runtime_init_once();

    if (!atomic_load_explicit(&s_rt.runtime_running, memory_order_acquire))
        return;

    const uint64_t now = pal_read_cntpct();
    bellatrix_runtime_host_step(now, s_rt.host_counter_freq);

    // Publish latest IPL snapshot for consumers that still rely on PAL.
    atomic_store_explicit(&s_rt.pending_ipl,
                          bellatrix_runtime_get_pending_ipl(),
                          memory_order_release);
}

// ---------------------------------------------------------------------------
// CPU-side notifications
//
// JIT / CPU side can report progress here. The real runtime decides how to
// consume this information.
// ---------------------------------------------------------------------------
void PAL_Runtime_ReportCpuProgress(uint32_t cycles)
{
    bellatrix_runtime_notify_cpu_progress(cycles);
}

// ---------------------------------------------------------------------------
// Chipset core main loop
//
// This is the new heart of pal_core.c.
// It is NOT a VBL loop.
// It is a runtime loop that advances Bellatrix time using the host counter.
// ---------------------------------------------------------------------------
static void chipset_core_loop(void)
{
    while (!atomic_load_explicit(&s_rt.runtime_ready, memory_order_acquire))
        pal_wfe();

    atomic_store_explicit(&s_rt.chipset_core_started, 1u, memory_order_release);

    while (atomic_load_explicit(&s_rt.runtime_running, memory_order_acquire)) {
        const uint64_t now = pal_read_cntpct();

        bellatrix_runtime_host_step(now, s_rt.host_counter_freq);

        // Publish latest IPL snapshot so the CPU side can observe it cheaply.
        atomic_store_explicit(&s_rt.pending_ipl,
                              bellatrix_runtime_get_pending_ipl(),
                              memory_order_release);

        // Transitional policy:
        //   - in a mature implementation, the chipset thread should sleep/wake
        //     based on a scheduler horizon or inter-core event queue
        //   - for now we keep a light WFE to avoid a hot spin
        pal_wfe();
    }

    atomic_store_explicit(&s_rt.chipset_core_started, 0u, memory_order_release);
}

// ---------------------------------------------------------------------------
// Core 1 bootstrap entry
//
// Called by the secondary core bootstrap path.
// ---------------------------------------------------------------------------
static void (*volatile s_chipset_entry)(void) = NULL;

void bellatrix_core1_entry(void)
{
    while (!s_chipset_entry)
        pal_wfe();

    s_chipset_entry();

    while (1)
        pal_wfe();
}

// ---------------------------------------------------------------------------
// Legacy-compatible PAL timer entry points
//
// These names are preserved to minimize churn during migration.
// They no longer configure a fixed VBL source.
// ---------------------------------------------------------------------------
void PAL_ChipsetTimer_Init(uint32_t hz, void (*cb)(void))
{
    (void)hz;
    (void)cb;

    pal_runtime_init_once();
}

void PAL_ChipsetTimer_Start(void)
{
    pal_runtime_init_once();
    atomic_store_explicit(&s_rt.runtime_running, 1u, memory_order_release);
    pal_sev();
}

void PAL_ChipsetTimer_Stop(void)
{
    atomic_store_explicit(&s_rt.runtime_running, 0u, memory_order_release);
}

// ---------------------------------------------------------------------------
// Launch chipset execution.
//
// If multicore is disabled, no secondary core is launched and the caller is
// expected to drive PAL_Runtime_Poll() from the CPU/JIT side.
// If multicore is enabled, core 1 is woken and runs chipset_core_loop().
// ---------------------------------------------------------------------------
void PAL_Core_LaunchChipset(void (*entry)(void))
{
    (void)entry; // reserved for future custom loop injection

    pal_runtime_init_once();

    if (!atomic_load_explicit(&s_rt.multicore_enabled, memory_order_acquire)) {
        // Single-core mode: no secondary core work loop.
        return;
    }

    s_chipset_entry = chipset_core_loop;

    pal_dsb_sy();
    pal_sev();
}

// ---------------------------------------------------------------------------
// Cheap publication / observation helpers
// ---------------------------------------------------------------------------
uint32_t PAL_Runtime_GetPendingIPL(void)
{
    return atomic_load_explicit(&s_rt.pending_ipl, memory_order_acquire);
}

void PAL_Runtime_WakeupChipset(void)
{
    pal_sev();
}

void PAL_Runtime_MmioBarrier(void)
{
    bellatrix_runtime_mmio_barrier();
}

// ---------------------------------------------------------------------------
// Video stubs (unchanged for now)
// ---------------------------------------------------------------------------
int PAL_Video_Init(uint32_t w, uint32_t h, uint32_t bpp)
{
    (void)w;
    (void)h;
    (void)bpp;
    return 0;
}

uint32_t *PAL_Video_GetBuffer(void)
{
    return 0;
}

void PAL_Video_Flip(void)
{
}

void PAL_Video_SetPalette(uint8_t idx, uint32_t rgb)
{
    (void)idx;
    (void)rgb;
}

// ---------------------------------------------------------------------------
// Generic memory sync helper
// ---------------------------------------------------------------------------
void PAL_Core_Sync(void)
{
    pal_dmb_ish();
}