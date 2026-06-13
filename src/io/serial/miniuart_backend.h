#ifndef BELLATRIX_IO_SERIAL_MINIUART_BACKEND_H
#define BELLATRIX_IO_SERIAL_MINIUART_BACKEND_H

#include <stdint.h>
#include <stdbool.h>

typedef struct MiniUartBackend {
    bool open;
    uint32_t baud;
} MiniUartBackend;

bool miniuart_backend_open(MiniUartBackend *m, uint32_t baud);
/* The baud divisor tracks the VPU core clock; pass the real rate (mailbox
 * GET_CLOCK_RATE) when it may differ from the 250 MHz default. */
bool miniuart_backend_open_clk(MiniUartBackend *m, uint32_t baud, uint32_t clk_hz);
void miniuart_backend_close(MiniUartBackend *m);

bool miniuart_backend_is_open(const MiniUartBackend *m);

bool miniuart_backend_read_byte(MiniUartBackend *m, uint8_t *byte_out);
bool miniuart_backend_write_byte(MiniUartBackend *m, uint8_t byte);

uint32_t miniuart_backend_read_lsr(void);

#endif
