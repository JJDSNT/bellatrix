#include "usb_osal.h"

#include "usbh_core.h"
#include "host/raspi3/time.h"
#include "support.h"
#if BELLATRIX_ENABLE_USBSTACK
#include "usb_dwc2_reg.h"

#define BELLATRIX_USB_OSAL_DWC2_REG_BASE 0xF2980000UL

static inline uint32_t bellatrix_usb_osal_rd32(uint32_t off)
{
    return rd32le((uintptr_t)(BELLATRIX_USB_OSAL_DWC2_REG_BASE + off));
}
#endif

#define BELLATRIX_USB_OSAL_BUS_ID 0u

typedef struct {
    usb_thread_entry_t entry;
    void *arg;
} BellatrixUSBThread;

typedef struct {
    uint32_t count;
    uint32_t max_count;
} BellatrixUSBSem;

typedef struct {
    int locked;
} BellatrixUSBMutex;

typedef struct {
    uint32_t max_msgs;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uintptr_t *items;
} BellatrixUSBMq;

typedef struct BellatrixUSBTimerState {
    struct BellatrixUSBTimerState *next;
    struct usb_osal_timer *owner;
    uint64_t deadline_us;
    bool active;
} BellatrixUSBTimerState;

static BellatrixUSBTimerState *g_usb_timer_head;

void usb_osal_timer_poll(void);

static uint64_t bellatrix_usb_now_us(void)
{
    return raspi3_read_legacy_system_timer();
}

static uint64_t bellatrix_usb_timer_delay_us(const struct usb_osal_timer *timer)
{
    uint32_t timeout_ms;

    timeout_ms = timer ? timer->timeout_ms : 0u;
    if (timeout_ms == 0u) {
        timeout_ms = 1u;
    }
    return (uint64_t)timeout_ms * 1000u;
}

static BellatrixUSBTimerState *bellatrix_usb_timer_state(struct usb_osal_timer *timer)
{
    return timer ? (BellatrixUSBTimerState *)timer->timer : NULL;
}

usb_osal_thread_t usb_osal_thread_create(const char *name, uint32_t stack_size, uint32_t prio, usb_thread_entry_t entry, void *args)
{
    BellatrixUSBThread *thread;

    (void)name;
    (void)stack_size;
    (void)prio;

    thread = usb_osal_malloc(sizeof(*thread));
    if (!thread) {
        return NULL;
    }

    thread->entry = entry;
    thread->arg = args;
    return (usb_osal_thread_t)thread;
}

void usb_osal_thread_delete(usb_osal_thread_t thread)
{
    if (thread) {
        usb_osal_free(thread);
    }
}

void usb_osal_thread_schedule_other(void)
{
}

usb_osal_sem_t usb_osal_sem_create(uint32_t initial_count)
{
    BellatrixUSBSem *sem = usb_osal_malloc(sizeof(*sem));
    if (!sem) {
        return NULL;
    }

    sem->count = initial_count;
    sem->max_count = initial_count ? initial_count : 1u;
    return (usb_osal_sem_t)sem;
}

usb_osal_sem_t usb_osal_sem_create_counting(uint32_t max_count)
{
    BellatrixUSBSem *sem = usb_osal_malloc(sizeof(*sem));
    if (!sem) {
        return NULL;
    }

    sem->count = 0;
    sem->max_count = max_count ? max_count : 1u;
    return (usb_osal_sem_t)sem;
}

void usb_osal_sem_delete(usb_osal_sem_t sem)
{
    if (sem) {
        usb_osal_free(sem);
    }
}

int usb_osal_sem_take(usb_osal_sem_t sem, uint32_t timeout)
{
    BellatrixUSBSem *state = (BellatrixUSBSem *)sem;
    uint32_t spins = 0;
    uint32_t max_spins;

    if (!state) {
        return -1;
    }

    max_spins = (timeout == USB_OSAL_WAITING_FOREVER) ? 1000000u : (timeout + 1u) * 256u;

    while (state->count == 0 && spins < max_spins) {
        usb_osal_timer_poll();
        USBH_IRQHandler(BELLATRIX_USB_OSAL_BUS_ID);
        usb_osal_thread_schedule_other();
        spins++;
    }

    if (state->count == 0) {
#if BELLATRIX_ENABLE_USBSTACK
        kprintf("[USB] sem timeout: spins=%u GAHBCFG=0x%08x GUSBCFG=0x%08x GRSTCTL=0x%08x GINTSTS=0x%08x GINTMSK=0x%08x HFNUM=0x%08x HNPTXSTS=0x%08x HPTXSTS=0x%08x HAINT=0x%08x HAINTMSK=0x%08x HCINT0=0x%08x HCINTMSK0=0x%08x HCCHAR0=0x%08x HCSPLT0=0x%08x HCTSIZ0=0x%08x HCDMA0=0x%08x\n",
                (unsigned)spins,
                (unsigned)bellatrix_usb_osal_rd32(0x008U),
                (unsigned)bellatrix_usb_osal_rd32(0x00cU),
                (unsigned)bellatrix_usb_osal_rd32(0x010U),
                (unsigned)bellatrix_usb_osal_rd32(0x014U),
                (unsigned)bellatrix_usb_osal_rd32(0x018U),
                (unsigned)bellatrix_usb_osal_rd32(USB_OTG_HOST_BASE + 0x008U),
                (unsigned)bellatrix_usb_osal_rd32(0x02cU),
                (unsigned)bellatrix_usb_osal_rd32(USB_OTG_HOST_BASE + 0x010U),
                (unsigned)bellatrix_usb_osal_rd32(USB_OTG_HOST_BASE + 0x014U),
                (unsigned)bellatrix_usb_osal_rd32(USB_OTG_HOST_BASE + 0x018U),
                (unsigned)bellatrix_usb_osal_rd32(USB_OTG_HOST_CHANNEL_BASE + 0x008U),
                (unsigned)bellatrix_usb_osal_rd32(USB_OTG_HOST_CHANNEL_BASE + 0x00cU),
                (unsigned)bellatrix_usb_osal_rd32(USB_OTG_HOST_CHANNEL_BASE + 0x000U),
                (unsigned)bellatrix_usb_osal_rd32(USB_OTG_HOST_CHANNEL_BASE + 0x004U),
                (unsigned)bellatrix_usb_osal_rd32(USB_OTG_HOST_CHANNEL_BASE + 0x010U),
                (unsigned)bellatrix_usb_osal_rd32(USB_OTG_HOST_CHANNEL_BASE + 0x014U));
        kprintf("[USB] sem timeout(2): GOTGCTL=0x%08x HCFG=0x%08x HFIR=0x%08x HPRT=0x%08x PCGCCTL=0x%08x\n",
                (unsigned)bellatrix_usb_osal_rd32(0x000U),
                (unsigned)bellatrix_usb_osal_rd32(USB_OTG_HOST_BASE + 0x000U),
                (unsigned)bellatrix_usb_osal_rd32(USB_OTG_HOST_BASE + 0x004U),
                (unsigned)bellatrix_usb_osal_rd32(USB_OTG_HOST_BASE + 0x040U),
                (unsigned)bellatrix_usb_osal_rd32(0xE00U));
#endif
        return -1;
    }

    state->count--;
    return 0;
}

int usb_osal_sem_give(usb_osal_sem_t sem)
{
    BellatrixUSBSem *state = (BellatrixUSBSem *)sem;

    if (!state) {
        return -1;
    }

    if (state->count < state->max_count) {
        state->count++;
    }
    return 0;
}

void usb_osal_sem_reset(usb_osal_sem_t sem)
{
    BellatrixUSBSem *state = (BellatrixUSBSem *)sem;
    if (state) {
        state->count = 0;
    }
}

usb_osal_mutex_t usb_osal_mutex_create(void)
{
    BellatrixUSBMutex *mutex = usb_osal_malloc(sizeof(*mutex));
    if (!mutex) {
        return NULL;
    }

    mutex->locked = 0;
    return (usb_osal_mutex_t)mutex;
}

void usb_osal_mutex_delete(usb_osal_mutex_t mutex)
{
    if (mutex) {
        usb_osal_free(mutex);
    }
}

int usb_osal_mutex_take(usb_osal_mutex_t mutex)
{
    BellatrixUSBMutex *state = (BellatrixUSBMutex *)mutex;

    if (!state || state->locked) {
        return -1;
    }

    state->locked = 1;
    return 0;
}

int usb_osal_mutex_give(usb_osal_mutex_t mutex)
{
    BellatrixUSBMutex *state = (BellatrixUSBMutex *)mutex;

    if (!state) {
        return -1;
    }

    state->locked = 0;
    return 0;
}

usb_osal_mq_t usb_osal_mq_create(uint32_t max_msgs)
{
    BellatrixUSBMq *mq;

    if (max_msgs == 0) {
        return NULL;
    }

    mq = usb_osal_malloc(sizeof(*mq));
    if (!mq) {
        return NULL;
    }

    mq->items = usb_osal_malloc(sizeof(uintptr_t) * max_msgs);
    if (!mq->items) {
        usb_osal_free(mq);
        return NULL;
    }

    mq->max_msgs = max_msgs;
    mq->head = 0;
    mq->tail = 0;
    mq->count = 0;
    return (usb_osal_mq_t)mq;
}

void usb_osal_mq_delete(usb_osal_mq_t mq)
{
    BellatrixUSBMq *state = (BellatrixUSBMq *)mq;
    if (!state) {
        return;
    }

    usb_osal_free(state->items);
    usb_osal_free(state);
}

int usb_osal_mq_send(usb_osal_mq_t mq, uintptr_t addr)
{
    BellatrixUSBMq *state = (BellatrixUSBMq *)mq;

    if (!state || state->count == state->max_msgs) {
        return -1;
    }

    state->items[state->tail] = addr;
    state->tail = (state->tail + 1u) % state->max_msgs;
    state->count++;
    return 0;
}

int usb_osal_mq_recv(usb_osal_mq_t mq, uintptr_t *addr, uint32_t timeout)
{
    BellatrixUSBMq *state = (BellatrixUSBMq *)mq;

    (void)timeout;
    if (!state || !addr || state->count == 0) {
        return -1;
    }

    *addr = state->items[state->head];
    state->head = (state->head + 1u) % state->max_msgs;
    state->count--;
    return 0;
}

struct usb_osal_timer *usb_osal_timer_create(const char *name, uint32_t timeout_ms, usb_timer_handler_t handler, void *argument, bool is_period)
{
    struct usb_osal_timer *timer;
    BellatrixUSBTimerState *state;

    (void)name;
    timer = usb_osal_malloc(sizeof(*timer));
    if (!timer) {
        return NULL;
    }

    state = usb_osal_malloc(sizeof(*state));
    if (!state) {
        usb_osal_free(timer);
        return NULL;
    }

    memset(state, 0, sizeof(*state));
    state->owner = timer;

    timer->handler = handler;
    timer->argument = argument;
    timer->is_period = is_period;
    timer->timeout_ms = timeout_ms;
    timer->timer = state;
    return timer;
}

void usb_osal_timer_delete(struct usb_osal_timer *timer)
{
    BellatrixUSBTimerState *state;
    BellatrixUSBTimerState **link;

    if (timer) {
        state = bellatrix_usb_timer_state(timer);
        if (state) {
            link = &g_usb_timer_head;
            while (*link) {
                if (*link == state) {
                    *link = state->next;
                    break;
                }
                link = &(*link)->next;
            }
            usb_osal_free(state);
        }
        usb_osal_free(timer);
    }
}

void usb_osal_timer_start(struct usb_osal_timer *timer)
{
    BellatrixUSBTimerState *state;

    state = bellatrix_usb_timer_state(timer);
    if (!state) {
        return;
    }

    state->deadline_us = bellatrix_usb_now_us() + bellatrix_usb_timer_delay_us(timer);
    if (!state->active) {
        state->next = g_usb_timer_head;
        g_usb_timer_head = state;
        state->active = true;
    }
}

void usb_osal_timer_stop(struct usb_osal_timer *timer)
{
    BellatrixUSBTimerState *state;
    BellatrixUSBTimerState **link;

    state = bellatrix_usb_timer_state(timer);
    if (!state || !state->active) {
        return;
    }

    link = &g_usb_timer_head;
    while (*link) {
        if (*link == state) {
            *link = state->next;
            break;
        }
        link = &(*link)->next;
    }
    state->next = NULL;
    state->active = false;
}

size_t usb_osal_enter_critical_section(void)
{
    return 0;
}

void usb_osal_leave_critical_section(size_t flag)
{
    (void)flag;
}

void usb_osal_msleep(uint32_t delay)
{
    raspi3_delay_us((uint64_t)delay * 1000u);
}

void usb_osal_timer_poll(void)
{
    BellatrixUSBTimerState **link;
    BellatrixUSBTimerState *state;
    struct usb_osal_timer *timer;
    uint64_t now_us;

    now_us = bellatrix_usb_now_us();
    link = &g_usb_timer_head;
    while (*link) {
        state = *link;
        if (!state->active || now_us < state->deadline_us) {
            link = &state->next;
            continue;
        }

        *link = state->next;
        state->next = NULL;
        state->active = false;

        timer = state->owner;
        if (timer && timer->handler) {
            timer->handler(timer->argument);
        }

        if (timer && timer->is_period && !state->active) {
            usb_osal_timer_start(timer);
        }
    }
}
