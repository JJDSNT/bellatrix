#include "io/bluetooth/bt_diag.h"
#include "support.h"

#include <stdarg.h>
#include <string.h>

#define BT_DIAG_CAP 8192u

static char     s_ring[BT_DIAG_CAP];
static uint32_t s_head;     /* total bytes ever written (mod CAP = position) */

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

static void ring_put(const char *s, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++)
        s_ring[(s_head + i) % BT_DIAG_CAP] = s[i];
    s_head += n;
}

void bt_diag_log(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (n <= 0)
        return;
    if ((unsigned)n >= sizeof(line))
        n = (int)sizeof(line) - 1;

    ring_put(line, (uint32_t)n);
    if (line[n - 1] != '\n')
        ring_put("\n", 1u);

    /* Mirror to the console while it is still ours (kprintf no-ops once the
     * PL011 is handed to the BT controller). */
    kprintf("%s", line);
}

uint32_t bt_diag_snapshot(char *out, uint32_t cap)
{
    uint32_t total = (s_head < BT_DIAG_CAP) ? s_head : BT_DIAG_CAP;
    uint32_t start = (s_head < BT_DIAG_CAP) ? 0u : (s_head % BT_DIAG_CAP);
    uint32_t n = 0u;

    if (cap == 0u)
        return 0u;

    for (uint32_t i = 0u; i < total && n + 1u < cap; i++)
        out[n++] = s_ring[(start + i) % BT_DIAG_CAP];
    out[n] = '\0';
    return n;
}
