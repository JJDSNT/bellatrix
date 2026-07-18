#include "machine/expansion.h"

#include <stdlib.h>
#include <string.h>

#ifndef BELLATRIX_MAX_EXPANSIONS
#define BELLATRIX_MAX_EXPANSIONS 64
#endif

/*
 * Temporary internal registry.
 *
 * Later this should probably move into BellatrixMachine itself, for example:
 *
 *     machine->expansions.items
 *     machine->expansions.count
 *
 * For now this keeps the expansion layer simple and easy to integrate.
 */

typedef struct BellatrixExpansionRegistry {
    BellatrixExpansion *items[BELLATRIX_MAX_EXPANSIONS];
    size_t count;
} BellatrixExpansionRegistry;

static BellatrixExpansionRegistry g_registry;

static int expansion_id_exists(const char *id)
{
    size_t i;

    if (!id) {
        return 0;
    }

    for (i = 0; i < g_registry.count; ++i) {
        BellatrixExpansion *exp = g_registry.items[i];

        if (exp && exp->desc.id && strcmp(exp->desc.id, id) == 0) {
            return 1;
        }
    }

    return 0;
}

int bellatrix_expansion_register(
    BellatrixMachine *machine,
    const BellatrixExpansionDesc *desc
) {
    BellatrixExpansion *exp;
    int rc;

    if (!machine || !desc || !desc->id || !desc->name || !desc->ops) {
        return -1;
    }

    if (g_registry.count >= BELLATRIX_MAX_EXPANSIONS) {
        return -2;
    }

    if (expansion_id_exists(desc->id)) {
        return -3;
    }

    exp = (BellatrixExpansion *)calloc(1, sizeof(*exp));
    if (!exp) {
        return -4;
    }

    exp->desc = *desc;
    exp->machine = machine;
    exp->attached = 0;

    if (exp->desc.ops->attach) {
        rc = exp->desc.ops->attach(exp, machine);
        if (rc != 0) {
            free(exp);
            return rc;
        }
    }

    exp->attached = 1;
    g_registry.items[g_registry.count++] = exp;

    return 0;
}

BellatrixExpansion *bellatrix_expansion_find(
    BellatrixMachine *machine,
    const char *id
) {
    size_t i;

    if (!machine || !id) {
        return NULL;
    }

    for (i = 0; i < g_registry.count; ++i) {
        BellatrixExpansion *exp = g_registry.items[i];

        if (!exp) {
            continue;
        }

        if (exp->machine != machine) {
            continue;
        }

        if (exp->desc.id && strcmp(exp->desc.id, id) == 0) {
            return exp;
        }
    }

    return NULL;
}

void bellatrix_expansion_reset_all(BellatrixMachine *machine)
{
    size_t i;

    if (!machine) {
        return;
    }

    for (i = 0; i < g_registry.count; ++i) {
        BellatrixExpansion *exp = g_registry.items[i];

        if (!exp || exp->machine != machine || !exp->attached) {
            continue;
        }

        if (exp->desc.ops && exp->desc.ops->reset) {
            exp->desc.ops->reset(exp);
        }
    }
}

void bellatrix_expansion_shutdown_all(BellatrixMachine *machine)
{
    size_t i;

    if (!machine) {
        return;
    }

    for (i = g_registry.count; i > 0; --i) {
        BellatrixExpansion *exp = g_registry.items[i - 1];

        if (!exp || exp->machine != machine) {
            continue;
        }

        if (exp->attached && exp->desc.ops && exp->desc.ops->shutdown) {
            exp->desc.ops->shutdown(exp);
        }

        exp->attached = 0;

        if (exp->desc.ops && exp->desc.ops->destroy) {
            exp->desc.ops->destroy(exp);
        }

        free(exp);
        g_registry.items[i - 1] = NULL;
    }

    g_registry.count = 0;
}

int bellatrix_expansion_bus_read(
    BellatrixMachine *machine,
    uint32_t addr,
    unsigned int size,
    uint32_t *value
)
{
    size_t i;

    if (!machine || !value) {
        return 0;
    }

    for (i = 0; i < g_registry.count; ++i) {
        BellatrixExpansion *exp = g_registry.items[i];

        if (!exp || exp->machine != machine || !exp->attached ||
            !exp->desc.bus_ops || !exp->desc.bus_ops->owns_address ||
            !exp->desc.bus_ops->read) {
            continue;
        }

        if (!exp->desc.bus_ops->owns_address(exp, addr)) {
            continue;
        }

        *value = exp->desc.bus_ops->read(exp, addr, size);
        return 1;
    }

    return 0;
}

int bellatrix_expansion_bus_write(
    BellatrixMachine *machine,
    uint32_t addr,
    uint32_t value,
    unsigned int size
)
{
    size_t i;

    if (!machine) {
        return 0;
    }

    for (i = 0; i < g_registry.count; ++i) {
        BellatrixExpansion *exp = g_registry.items[i];

        if (!exp || exp->machine != machine || !exp->attached ||
            !exp->desc.bus_ops || !exp->desc.bus_ops->owns_address ||
            !exp->desc.bus_ops->write) {
            continue;
        }

        if (!exp->desc.bus_ops->owns_address(exp, addr)) {
            continue;
        }

        exp->desc.bus_ops->write(exp, addr, value, size);
        return 1;
    }

    return 0;
}
