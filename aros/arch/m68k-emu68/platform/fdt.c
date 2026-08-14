/*
 * Minimal, allocation-based FDT walker for post-heap platform discovery.
 * See fdt.h. Ported from the tree-building approach in
 * arch/aarch64-raspi/boot/devicetree.c, trimmed to what platform.c needs
 * and using AllocVec() instead of a pre-heap malloc() replacement, since
 * this only ever runs after the AROS heap exists.
 */
#include "fdt.h"

#include <aros/macros.h>
#include <exec/memory.h>
#include <proto/exec.h>

#define FDT_MAGIC       0xd00dfeedUL
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

struct FdtHeader
{
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

static of_node_t *dt_root;
static const uint32_t *dt_cursor;
static const uint32_t *dt_struct_end;
static const char *dt_strings;

static uint32_t str_len(const char *s)
{
    uint32_t n = 0;

    while (s[n])
        n++;

    return n;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }

    return *a == *b;
}

/* Match a node name against a path component. If the component has no
 * '@' unit address, ignore the node's own unit address suffix (standard DT
 * lookup convention: "/soc/timer" matches "timer@7e003000"). */
static int name_matches(const char *node_name, const char *component)
{
    while (*component)
    {
        if (*node_name != *component)
            return 0;
        node_name++;
        component++;
    }

    return *node_name == 0 || *node_name == '@';
}

static of_node_t *dt_build_node(of_node_t *parent)
{
    of_node_t *node = AllocVec(sizeof(*node), MEMF_PUBLIC | MEMF_CLEAR);

    if (!node)
        return NULL;

    NEWLIST((struct List *)&node->on_children);
    NEWLIST((struct List *)&node->on_properties);
    node->on_name = (char *)dt_cursor;
    dt_cursor += (str_len((const char *)dt_cursor) + 4) / 4;

    if (parent)
        ADDTAIL((struct List *)&parent->on_children, (struct Node *)node);

    while (dt_cursor < dt_struct_end)
    {
        uint32_t token = AROS_BE2LONG(*dt_cursor++);

        switch (token)
        {
        case FDT_BEGIN_NODE:
            dt_build_node(node);
            break;

        case FDT_PROP:
        {
            of_property_t *prop;
            uint32_t length, name_offset;

            if (dt_cursor + 2 > dt_struct_end)
                return node;

            length = AROS_BE2LONG(dt_cursor[0]);
            name_offset = AROS_BE2LONG(dt_cursor[1]);
            dt_cursor += 2;

            prop = AllocVec(sizeof(*prop), MEMF_PUBLIC | MEMF_CLEAR);
            if (prop)
            {
                prop->op_length = length;
                prop->op_name = (char *)&dt_strings[name_offset];
                prop->op_value = length ? (void *)dt_cursor : NULL;
                ADDTAIL((struct List *)&node->on_properties,
                        (struct Node *)prop);
            }

            dt_cursor += (length + 3) / 4;
            break;
        }

        case FDT_NOP:
            break;

        case FDT_END_NODE:
            return node;

        default:
            return node;
        }
    }

    return node;
}

of_node_t *dt_parse(const void *fdt)
{
    const struct FdtHeader *header = fdt;
    const uint8_t *base = fdt;

    dt_root = NULL;

    if (!header || AROS_BE2LONG(header->magic) != FDT_MAGIC)
        return NULL;

    dt_strings = (const char *)(base + AROS_BE2LONG(header->off_dt_strings));
    dt_cursor = (const uint32_t *)(base + AROS_BE2LONG(header->off_dt_struct));
    dt_struct_end = dt_cursor +
                    AROS_BE2LONG(header->size_dt_struct) / sizeof(uint32_t);

    while (dt_cursor < dt_struct_end)
    {
        uint32_t token = AROS_BE2LONG(*dt_cursor++);

        if (token == FDT_BEGIN_NODE)
        {
            dt_root = dt_build_node(NULL);
            break;
        }
        if (token == FDT_END)
            break;
    }

    return dt_root;
}

of_node_t *dt_root_node(void)
{
    return dt_root;
}

of_node_t *dt_find_node(const char *path)
{
    of_node_t *node = dt_root;

    if (!node || *path != '/')
        return NULL;

    while (*path && node)
    {
        char component[64];
        uint32_t i = 0;
        of_node_t *child, *found = NULL;

        path++;
        while (*path && *path != '/' && i < sizeof(component) - 1)
            component[i++] = *path++;
        component[i] = 0;

        if (i == 0)
            continue;

        ForeachNode((struct List *)&node->on_children, child)
        {
            if (name_matches(child->on_name, component))
            {
                found = child;
                break;
            }
        }

        node = found;
    }

    return node;
}

of_property_t *dt_find_property(of_node_t *node, const char *name)
{
    of_property_t *prop;

    if (!node)
        return NULL;

    ForeachNode((struct List *)&node->on_properties, prop)
    {
        if (str_eq(prop->op_name, name))
            return prop;
    }

    return NULL;
}

uint32_t dt_prop_u32(of_property_t *prop, uint32_t index)
{
    const uint32_t *cells = (const uint32_t *)prop->op_value;

    return AROS_BE2LONG(cells[index]);
}

uint32_t dt_prop_u32_default(of_node_t *node, const char *name,
                             uint32_t default_value)
{
    of_property_t *prop = dt_find_property(node, name);

    if (!prop || prop->op_length < sizeof(uint32_t))
        return default_value;

    return dt_prop_u32(prop, 0);
}
