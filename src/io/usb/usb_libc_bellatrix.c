#include "support.h"
#include "tlsf.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *buf;
    size_t size;
    size_t pos;
} bellatrix_usb_vsnprintf_ctx_t;

static void bellatrix_usb_vsnprintf_putc(void *data, char c)
{
    bellatrix_usb_vsnprintf_ctx_t *ctx = (bellatrix_usb_vsnprintf_ctx_t *)data;

    if (ctx->size > 0 && ctx->pos + 1 < ctx->size) {
        ctx->buf[ctx->pos++] = c;
        ctx->buf[ctx->pos] = 0;
    }
}

int bellatrix_usb_vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
    bellatrix_usb_vsnprintf_ctx_t ctx = {str, size, 0};

    if (size > 0) {
        str[0] = 0;
    }

    vkprintf_pc(bellatrix_usb_vsnprintf_putc, &ctx, format, ap);
    return (int)ctx.pos;
}

int bellatrix_usb_snprintf(char *str, size_t size, const char *format, ...)
{
    int written;
    va_list ap;

    va_start(ap, format);
    written = bellatrix_usb_vsnprintf(str, size, format, ap);
    va_end(ap);
    return written;
}

long bellatrix_usb_strtol(const char *nptr, char **endptr, int base)
{
    const char *p = nptr;
    long value = 0;
    int negative = 0;

    if (!p) {
        if (endptr) {
            *endptr = (char *)nptr;
        }
        return 0;
    }

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        ++p;
    }

    if (*p == '+') {
        ++p;
    } else if (*p == '-') {
        negative = 1;
        ++p;
    }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (base == 0) {
        base = 10;
    }

    while (*p) {
        int digit;

        if (*p >= '0' && *p <= '9') {
            digit = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            digit = 10 + (*p - 'a');
        } else if (*p >= 'A' && *p <= 'F') {
            digit = 10 + (*p - 'A');
        } else {
            break;
        }

        if (digit >= base) {
            break;
        }

        value = (value * base) + digit;
        ++p;
    }

    if (endptr) {
        *endptr = (char *)p;
    }

    return negative ? -value : value;
}

void *usb_osal_malloc(size_t size)
{
    extern void *tlsf;
    return tlsf_malloc(tlsf, size);
}

void usb_osal_free(void *ptr)
{
    extern void *tlsf;
    tlsf_free(tlsf, ptr);
}
