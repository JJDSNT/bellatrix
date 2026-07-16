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
//   - implement bellatrix_runtime_chipset_step()
//   - implement bellatrix_runtime_publish_cpu_cycles()
//   - implement bellatrix_runtime_get_pending_ipl()
//   - wire single-core polling into the CPU/JIT side when multicore is disabled

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "pal.h"
#include "host/osd.h"
#include "runtime/core_chipset.h"
#include "runtime/cpu_progress.h"
#include "runtime/topology.h"
#include "support.h"

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
    .cpu_core_id          = BELLATRIX_CORE_CPU,
    .chipset_core_id      = BELLATRIX_CORE_CHIPSET,
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
void bellatrix_runtime_chipset_step(uint64_t host_now, uint64_t host_freq)
{
    (void)host_now;
    (void)host_freq;
}

__attribute__((weak))
void bellatrix_runtime_io_step(uint64_t host_now, uint64_t host_freq)
{
    (void)host_now;
    (void)host_freq;
}

__attribute__((weak))
void bellatrix_runtime_publish_cpu_cycles(uint32_t cycles)
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

static _Atomic uint32_t s_event_stream_hz;
static uint64_t s_event_stream_saved_cntkctl;

/* Configure the architectural timer event stream on the calling PE. WFE is
 * then released by the selected CNTPCT bit edge even when no producer sends
 * SEV. This is the PiStorm housekeeper mechanism at a lower chipset cadence. */
static uint32_t pal_event_stream_enable(uint32_t target_hz)
{
    uint64_t freq = pal_read_cntfrq();
    uint64_t ctl;
    uint32_t bit = 0u;
    uint32_t actual;

    if (target_hz == 0u || freq == 0u)
        return 0u;
    while (bit < 15u && freq / (1ull << (bit + 1u)) > target_hz)
        bit++;
    actual = (uint32_t)(freq / (1ull << (bit + 1u)));

    asm volatile("mrs %0, CNTKCTL_EL1" : "=r"(ctl));
    s_event_stream_saved_cntkctl = ctl;
    ctl &= ~((uint64_t)0x0fu << 4u);
    ctl |= (uint64_t)1u << 2u;       /* EVNTEN */
    ctl |= (uint64_t)bit << 4u;      /* EVNTI */
    ctl &= ~((uint64_t)1u << 3u);    /* EVNTDIR: low-to-high edge */
    asm volatile("msr CNTKCTL_EL1, %0\n\tisb" :: "r"(ctl) : "memory");
    atomic_store_explicit(&s_event_stream_hz, actual, memory_order_release);
    return actual;
}

/* Enable a private architectural event stream on the calling PE without
 * publishing it as the chipset event source. The host-reactor role uses this
 * to wake deferred services; the register is per-core. */
static uint32_t pal_local_event_stream_enable(uint32_t target_hz)
{
    uint64_t freq = pal_read_cntfrq();
    uint64_t ctl;
    uint32_t bit = 0u;

    if (target_hz == 0u || freq == 0u)
        return 0u;
    while (bit < 15u && freq / (1ull << (bit + 1u)) > target_hz)
        bit++;

    asm volatile("mrs %0, CNTKCTL_EL1" : "=r"(ctl));
    ctl &= ~((uint64_t)0x0fu << 4u);
    ctl |= (uint64_t)1u << 2u;
    ctl |= (uint64_t)bit << 4u;
    ctl &= ~((uint64_t)1u << 3u);
    asm volatile("msr CNTKCTL_EL1, %0\n\tisb" ::
                 "r"(ctl) : "memory");
    return (uint32_t)(freq / (1ull << (bit + 1u)));
}

static void pal_event_stream_disable(void)
{
    atomic_store_explicit(&s_event_stream_hz, 0u, memory_order_release);
    asm volatile("msr CNTKCTL_EL1, %0\n\tisb" ::
                 "r"(s_event_stream_saved_cntkctl) : "memory");
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
    bellatrix_runtime_chipset_step(now, s_rt.host_counter_freq);

    // Single-core has no independent host reactor, so physical IO must
    // be serviced here. Throttle to ~1ms: usb_host_step pumps the whole
    // CherryUSB event chain and is far too heavy for every MMIO access.
    if (!PAL_Core_IsMulticoreEnabled()) {
        static uint64_t io_last;
        const uint64_t io_interval = s_rt.host_counter_freq / 1000u;
        if (now - io_last >= io_interval) {
            io_last = now;
            bellatrix_runtime_io_step(now, s_rt.host_counter_freq);
        }
    }

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
    bellatrix_runtime_publish_cpu_cycles(cycles);
}

// ---------------------------------------------------------------------------
// Core 2 — Chipset (Rigel) main loop
// ---------------------------------------------------------------------------
static void chipset_core_loop(void)
{
    uint32_t event_hz;

    while (!atomic_load_explicit(&s_rt.runtime_ready, memory_order_acquire))
        pal_wfe();

    event_hz = pal_event_stream_enable(250000u);
    atomic_store_explicit(&s_rt.chipset_core_started, 1u, memory_order_release);
    kprintf("[CORE2] timer event stream: %u Hz\n", (unsigned)event_hz);

    while (atomic_load_explicit(&s_rt.runtime_running, memory_order_acquire)) {
        const uint64_t now = pal_read_cntpct();

        bellatrix_runtime_chipset_step(now, s_rt.host_counter_freq);

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
    pal_event_stream_disable();
}

// ---------------------------------------------------------------------------
// Core 3 host-reactor worker, active in every multicore backend selection.
// bellatrix_runtime_io_step() owns both physical IO and host presentation, so
// Backend-specific code must not duplicate or replace this service contract.
// ---------------------------------------------------------------------------
static void host_reactor_loop(void)
{
    uint32_t event_hz;

    while (!atomic_load_explicit(&s_rt.runtime_ready, memory_order_acquire))
        pal_wfe();

    event_hz = pal_local_event_stream_enable(1000u);
    kprintf("[HOST] Core %u reactor event stream: %u Hz\n",
            (unsigned)BELLATRIX_CORE_HOST_REACTOR, (unsigned)event_hz);

    while (atomic_load_explicit(&s_rt.runtime_running, memory_order_acquire)) {
        const uint64_t now = pal_read_cntpct();
        bellatrix_runtime_io_step(now, s_rt.host_counter_freq);
        pal_wfe();
    }
}

// ---------------------------------------------------------------------------
// Secondary core bootstrap entries
//
// Each is called by secondary_boot() for its physical PE. Core 1 parks as the
// auxiliary role; Core 2/3 wait for their chipset/reactor assignments.
// ---------------------------------------------------------------------------
static void (*volatile s_chipset_entry)(void) = NULL;
static void (*volatile s_host_entry)(void)    = NULL;

void bellatrix_core1_entry(void)
{
    /* Core 1 is the unified topology's auxiliary PE. */
    while (1)
        pal_wfe();
}

void bellatrix_core2_entry(void)
{
    /* Core 2 — Chipset (Rigel). Parks until PAL_Core_LaunchChipset(). */
    while (!s_chipset_entry)
        pal_wfe();

    s_chipset_entry();

    while (1)
        pal_wfe();
}

void bellatrix_core3_entry(void)
{
    /* Core 3 parks until assigned the secondary host-reactor role. */
    while (!s_host_entry)
        pal_wfe();

    s_host_entry();

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
    core_chipset_timeline_request_pause(false);
    pal_sev();
}

void PAL_ChipsetTimer_Stop(void)
{
    core_chipset_timeline_request_pause(true);
    atomic_store_explicit(&s_rt.runtime_running, 0u, memory_order_release);
    pal_sev();
}

// ---------------------------------------------------------------------------
// Launch chipset execution.
//
// If multicore is disabled, no secondary core is launched and the caller is
// expected to drive PAL_Runtime_Poll() from the CPU/JIT side.
// If multicore is enabled, core 2 is woken and runs chipset_core_loop().
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

void PAL_Core_LaunchHostReactor(void)
{
    pal_runtime_init_once();

    if (!atomic_load_explicit(&s_rt.multicore_enabled, memory_order_acquire))
        return;

    s_host_entry = host_reactor_loop;

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

int PAL_Runtime_EventStreamActive(void)
{
    return atomic_load_explicit(&s_event_stream_hz, memory_order_acquire) != 0u;
}

uint32_t PAL_Runtime_EventStreamHz(void)
{
    return atomic_load_explicit(&s_event_stream_hz, memory_order_acquire);
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

int PAL_Video_Resize(uint32_t w, uint32_t h, uint32_t bpp)
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
    static uint32_t flip_count = 0;
    flip_count++;
    osd_render((uint64_t)flip_count);
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

// ---------------------------------------------------------------------------
// Host display event helpers (no-op on bare metal)
// ---------------------------------------------------------------------------
int pal_sdl_poll_events(void) { return 1; }
int pal_sdl_mouse_right_down(void) { return 0; }
int pal_sdl_mouse_button_down(unsigned button)
{
    (void)button;
    return 0;
}
void pal_sdl_consume_mouse_delta(int *dx, int *dy)
{
    if (dx)
        *dx = 0;
    if (dy)
        *dy = 0;
}
int pal_sdl_any_key_down(void) { return 0; }
int pal_sdl_pop_key_event(PAL_KeyEvent *event)
{
    (void)event;
    return 0;
}
void pal_sdl_set_title(const char *title) { (void)title; }
