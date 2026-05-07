#ifndef BELLATRIX_IO_SERIAL_MINIUART_BACKEND_H
#define BELLATRIX_IO_SERIAL_MINIUART_BACKEND_H

#include <stdint.h>
#include <stdbool.h>

typedef struct MiniUartBackend {
    bool open;
    uint32_t baud;
} MiniUartBackend;

bool miniuart_backend_open(MiniUartBackend *m, uint32_t baud);
void miniuart_backend_close(MiniUartBackend *m);

bool miniuart_backend_is_open(const MiniUartBackend *m);

bool miniuart_backend_read_byte(MiniUartBackend *m, uint8_t *byte_out);
bool miniuart_backend_write_byte(MiniUartBackend *m, uint8_t byte);

uint32_t miniuart_backend_read_lsr(void);

#endif
