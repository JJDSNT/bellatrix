#ifndef BELLATRIX_BT_UNISTD_H
#define BELLATRIX_BT_UNISTD_H

#include <stdint.h>

// Bellatrix bare-metal doesn't have a full unistd.h.
// We provide stubs or redirects to system functions.

#include "host/raspi3/time.h"

static inline int usleep(uint64_t usec) {
    raspi3_delay_us(usec);
    return 0;
}

static inline unsigned int sleep(unsigned int seconds) {
    raspi3_delay_us((uint64_t)seconds * 1000000uLL);
    return 0;
}

// Do NOT include the real unistd.h to avoid conflicts with A64.h (e.g. brk)

#endif // BELLATRIX_BT_UNISTD_H
