#include "io/bluetooth/bt_pairs.h"
#include <string.h>

static BTPair    s_pairs[BT_PAIRS_MAX];
static unsigned  s_count;
static bool      s_dirty;

static bool addr_eq(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

/* ---- hex helpers (no libc dependency) ---- */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static bool parse_addr(const char *s, uint8_t out[6])
{
    for (unsigned i = 0u; i < 6u; i++) {
        int hi = hex_val(s[0]), lo = hex_val(s[1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
        s += 2u;
        if (i < 5u && *s++ != ':') return false;
    }
    return true;
}

static char hex_digit(uint8_t v)
{
    if (v < 10u) return (char)('0' + v);
    return (char)('A' + v - 10u);
}

static unsigned fmt_addr(char *out, const uint8_t a[6])
{
    for (unsigned i = 0u; i < 6u; i++) {
        out[i * 3u]      = hex_digit((uint8_t)(a[i] >> 4));
        out[i * 3u + 1u] = hex_digit((uint8_t)(a[i] & 0x0Fu));
        out[i * 3u + 2u] = (i < 5u) ? ':' : '\0';
    }
    return 17u;
}

/* ---- public API ---- */

void bt_pairs_load(const char *text, uint32_t len)
{
    s_count = 0u;
    s_dirty = false;
    if (!text || !len) return;

    static const struct { char tag[2]; uint8_t val; } transports[] = {
        { {'C','L'}, 1u }, { {'L','E'}, 2u }, { {'D','M'}, 3u },
    };

    const char *p   = text;
    const char *end = text + len;

    while (p < end && s_count < BT_PAIRS_MAX) {
        /* skip blank lines / CR/LF */
        while (p < end && (*p == '\r' || *p == '\n' || *p == ' ')) p++;
        if (p >= end) break;

        const char *line = p;
        while (p < end && *p != '\r' && *p != '\n') p++;
        uint32_t line_len = (uint32_t)(p - line);
        /* minimum: "CL AA:BB:CC:DD:EE:FF x" = 22 chars */
        if (line_len < 22u) continue;

        uint8_t transport = 0u;
        for (unsigned t = 0u; t < 3u; t++) {
            if (line[0] == transports[t].tag[0] &&
                line[1] == transports[t].tag[1]) {
                transport = transports[t].val;
                break;
            }
        }
        if (!transport || line[2] != ' ') continue;

        uint8_t addr[6];
        if (!parse_addr(line + 3u, addr)) continue;

        BTPair *pair   = &s_pairs[s_count++];
        memcpy(pair->addr, addr, 6u);
        pair->transport = transport;

        /* name starts after "CL AA:BB:CC:DD:EE:FF " (3+17+1 = 21 chars) */
        const char *name = line + 21u;
        uint32_t nlen    = line_len - 21u;
        if (nlen >= BT_SCAN_NAME_LEN) nlen = BT_SCAN_NAME_LEN - 1u;
        memcpy(pair->name, name, nlen);
        pair->name[nlen] = '\0';
    }
}

unsigned bt_pairs_count(void) { return s_count; }

const BTPair *bt_pairs_get(unsigned i)
{
    return (i < s_count) ? &s_pairs[i] : NULL;
}

bool bt_pairs_is_known(const uint8_t addr[6])
{
    for (unsigned i = 0u; i < s_count; i++)
        if (addr_eq(s_pairs[i].addr, addr)) return true;
    return false;
}

bool bt_pairs_add(const BTScanResult *r)
{
    if (!r) return false;
    for (unsigned i = 0u; i < s_count; i++) {
        if (!addr_eq(s_pairs[i].addr, r->addr)) continue;
        bool changed = false;
        if (r->name[0] && !s_pairs[i].name[0]) {
            memcpy(s_pairs[i].name, r->name, BT_SCAN_NAME_LEN);
            changed = true;
        }
        uint8_t merged = s_pairs[i].transport | r->transport;
        if (merged != s_pairs[i].transport) {
            s_pairs[i].transport = merged;
            changed = true;
        }
        if (changed) s_dirty = true;
        return false; /* already in list */
    }
    if (s_count >= BT_PAIRS_MAX) return false;
    BTPair *pair   = &s_pairs[s_count++];
    memcpy(pair->addr, r->addr, 6u);
    pair->transport = r->transport;
    memcpy(pair->name, r->name, BT_SCAN_NAME_LEN);
    s_dirty = true;
    return true;
}

bool bt_pairs_remove(const uint8_t addr[6])
{
    for (unsigned i = 0u; i < s_count; i++) {
        if (!addr_eq(s_pairs[i].addr, addr)) continue;
        s_pairs[i] = s_pairs[--s_count];
        s_dirty = true;
        return true;
    }
    return false;
}

bool bt_pairs_dirty(void) { return s_dirty; }

uint32_t bt_pairs_serialise(char *buf, uint32_t cap)
{
    static const char *transport_str[4] = { "??", "CL", "LE", "DM" };
    uint32_t len = 0u;

    for (unsigned i = 0u; i < s_count; i++) {
        const BTPair *pair = &s_pairs[i];
        /* "CL AA:BB:CC:DD:EE:FF name\r\n" — max 2+1+17+1+24+2 = 47 */
        if (len + 48u >= cap) break;

        const char *t = transport_str[pair->transport & 3u];
        buf[len++] = t[0]; buf[len++] = t[1]; buf[len++] = ' ';
        len += fmt_addr(buf + len, pair->addr);
        buf[len++] = ' ';
        const char *name = pair->name[0] ? pair->name : "(unknown)";
        while (*name && len + 3u < cap) buf[len++] = *name++;
        buf[len++] = '\r'; buf[len++] = '\n';
    }

    /* pad remainder with spaces so fat32_overwrite_in_place doesn't
     * leave stale bytes from a longer previous write */
    while (len < cap - 1u) buf[len++] = ' ';
    buf[len] = '\0';
    return len;
}
