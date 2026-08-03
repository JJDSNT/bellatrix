/*
 * Minimal, allocation-based FDT walker for post-heap platform discovery.
 *
 * Only handles what platform.c actually needs: walking down from the root
 * by absolute path, and reading properties of a node once found. Values are
 * returned as raw big-endian cells (mirrors what real FDT drivers do); use
 * dt_prop_u32() to read them as host-native ULONGs, which is a no-op on
 * this (big-endian) target.
 */
#ifndef PLATFORM_FDT_H
#define PLATFORM_FDT_H

#include <exec/lists.h>
#include <inttypes.h>

typedef struct
{
    struct MinNode  on_node;
    char           *on_name;
    struct MinList  on_children;
    struct MinList  on_properties;
} of_node_t;

typedef struct
{
    struct MinNode  op_node;
    char           *op_name;
    uint32_t        op_length;
    void           *op_value;
} of_property_t;

/* Parse `fdt` into a node tree (allocated with AllocVec, never freed --
 * this only ever runs once at boot) and return the root node, or NULL if
 * `fdt` is not a valid FDT blob. */
of_node_t *dt_parse(const void *fdt);

/* Look up an absolute path ("/soc/timer@7e003000") from the root parsed by
 * dt_parse(). Returns NULL if any path component is missing. The unit
 * address (the part after '@') is ignored when matching a component that
 * omits it. */
of_node_t *dt_find_node(const char *path);

of_property_t *dt_find_property(of_node_t *node, const char *name);

/* Read 32-bit cell `index` (0-based) of a property's raw value, converting
 * from FDT big-endian to host order. Caller must ensure the property is
 * long enough. */
uint32_t dt_prop_u32(of_property_t *prop, uint32_t index);

/* dt_find_property() + dt_prop_u32(..., 0), with a fallback for a missing
 * or too-short property. Used for "#address-cells" / "#size-cells". */
uint32_t dt_prop_u32_default(of_node_t *node, const char *name,
                             uint32_t default_value);

#endif /* PLATFORM_FDT_H */
