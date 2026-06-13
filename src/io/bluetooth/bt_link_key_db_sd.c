#include "io/bluetooth/bt_link_key_db_sd.h"
#include <string.h>

typedef struct {
    uint8_t addr[6];
    uint8_t key[16];
    uint8_t key_type;
} BTLinkKeyEntry;

static BTLinkKeyEntry s_keys[BT_LINK_KEY_MAX];
static unsigned       s_count;
static bool           s_dirty;

/* ---- hex helpers ---- */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static char hex_digit(uint8_t v)
{
    return (v < 10u) ? (char)('0' + v) : (char)('A' + v - 10u);
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

static unsigned fmt_addr(char *out, const uint8_t a[6])
{
    for (unsigned i = 0u; i < 6u; i++) {
        out[i * 3u]     = hex_digit((uint8_t)(a[i] >> 4));
        out[i * 3u + 1] = hex_digit((uint8_t)(a[i] & 0x0Fu));
        out[i * 3u + 2] = (i < 5u) ? ':' : '\0';
    }
    return 17u;
}

static int find_entry(const uint8_t addr[6])
{
    for (unsigned i = 0u; i < s_count; i++)
        if (memcmp(s_keys[i].addr, addr, 6u) == 0) return (int)i;
    return -1;
}

/* ---- btstack_link_key_db_t interface ---- */

static void lkdb_open(void) {}
static void lkdb_set_local_bd_addr(bd_addr_t bd_addr) { (void)bd_addr; }
static void lkdb_close(void) {}

static int lkdb_get_link_key(bd_addr_t bd_addr, link_key_t link_key,
                              link_key_type_t *type)
{
    int idx = find_entry(bd_addr);
    if (idx < 0) return 0;
    memcpy(link_key, s_keys[idx].key, 16u);
    if (type) *type = (link_key_type_t)s_keys[idx].key_type;
    return 1;
}

static void lkdb_put_link_key(bd_addr_t bd_addr, link_key_t link_key,
                               link_key_type_t type)
{
    int idx = find_entry(bd_addr);
    if (idx < 0) {
        if (s_count >= BT_LINK_KEY_MAX) return;
        idx = (int)s_count++;
        memcpy(s_keys[idx].addr, bd_addr, 6u);
    }
    memcpy(s_keys[idx].key, link_key, 16u);
    s_keys[idx].key_type = (uint8_t)type;
    s_dirty = true;
}

static void lkdb_delete_link_key(bd_addr_t bd_addr)
{
    int idx = find_entry(bd_addr);
    if (idx < 0) return;
    s_keys[idx] = s_keys[--s_count];
    s_dirty = true;
}

static int lkdb_iterator_init(btstack_link_key_iterator_t *it)
{
    it->context = (void *)(uintptr_t)0u;
    return 1;
}

static int lkdb_iterator_get_next(btstack_link_key_iterator_t *it,
                                   bd_addr_t bd_addr, link_key_t link_key,
                                   link_key_type_t *type)
{
    unsigned idx = (unsigned)(uintptr_t)it->context;
    if (idx >= s_count) return 0;
    memcpy(bd_addr, s_keys[idx].addr, 6u);
    memcpy(link_key, s_keys[idx].key, 16u);
    if (type) *type = (link_key_type_t)s_keys[idx].key_type;
    it->context = (void *)(uintptr_t)(idx + 1u);
    return 1;
}

static void lkdb_iterator_done(btstack_link_key_iterator_t *it) { (void)it; }

static const btstack_link_key_db_t s_lkdb_sd = {
    lkdb_open,
    lkdb_set_local_bd_addr,
    lkdb_close,
    lkdb_get_link_key,
    lkdb_put_link_key,
    lkdb_delete_link_key,
    lkdb_iterator_init,
    lkdb_iterator_get_next,
    lkdb_iterator_done,
};

const btstack_link_key_db_t *bt_link_key_db_sd_instance(void)
{
    return &s_lkdb_sd;
}

/* ---- load / serialise ---- */

void bt_link_key_db_sd_load(const char *text, uint32_t len)
{
    s_count = 0u;
    s_dirty = false;
    if (!text || !len) return;

    const char *p   = text;
    const char *end = text + len;

    while (p < end && s_count < BT_LINK_KEY_MAX) {
        while (p < end && (*p == '\r' || *p == '\n' || *p == ' ')) p++;
        if (p >= end) break;

        const char *line = p;
        while (p < end && *p != '\r' && *p != '\n') p++;
        uint32_t line_len = (uint32_t)(p - line);

        /* minimum: "AA:BB:CC:DD:EE:FF TT XXXXXXXX..." = 17+1+2+1+32 = 53 */
        if (line_len < 53u) continue;

        uint8_t addr[6];
        if (!parse_addr(line, addr)) continue;
        if (line[17] != ' ') continue;

        int thi = hex_val(line[18]), tlo = hex_val(line[19]);
        if (thi < 0 || tlo < 0) continue;
        uint8_t key_type = (uint8_t)((thi << 4) | tlo);

        if (line[20] != ' ') continue;

        uint8_t key[16];
        const char *ks = line + 21u;
        bool ok = true;
        for (unsigned i = 0u; i < 16u; i++) {
            int hi = hex_val(ks[i * 2u]), lo = hex_val(ks[i * 2u + 1u]);
            if (hi < 0 || lo < 0) { ok = false; break; }
            key[i] = (uint8_t)((hi << 4) | lo);
        }
        if (!ok) continue;

        BTLinkKeyEntry *e = &s_keys[s_count++];
        memcpy(e->addr, addr, 6u);
        memcpy(e->key, key, 16u);
        e->key_type = key_type;
    }
}

uint32_t bt_link_key_db_sd_serialise(char *buf, uint32_t cap)
{
    uint32_t len = 0u;

    for (unsigned i = 0u; i < s_count; i++) {
        if (len + 56u >= cap) break;
        const BTLinkKeyEntry *e = &s_keys[i];
        len += fmt_addr(buf + len, e->addr);
        buf[len++] = ' ';
        buf[len++] = hex_digit((uint8_t)(e->key_type >> 4));
        buf[len++] = hex_digit((uint8_t)(e->key_type & 0x0Fu));
        buf[len++] = ' ';
        for (unsigned k = 0u; k < 16u; k++) {
            buf[len++] = hex_digit((uint8_t)(e->key[k] >> 4));
            buf[len++] = hex_digit((uint8_t)(e->key[k] & 0x0Fu));
        }
        buf[len++] = '\r';
        buf[len++] = '\n';
    }

    while (len < cap - 1u) buf[len++] = ' ';
    buf[len] = '\0';
    return len;
}

bool bt_link_key_db_sd_dirty(void)        { return s_dirty; }
void bt_link_key_db_sd_clear_dirty(void)  { s_dirty = false; }
