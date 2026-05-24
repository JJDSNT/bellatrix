#include <stddef.h>
#include <string.h>
#include "tlsf.h"

/* Bare-metal calloc/free backed by the emu68 TLSF heap.
 * The 'tlsf' pool handle is initialised in emu68's start.c before
 * any Bellatrix code runs, so these are safe to call from init. */

extern void *tlsf;

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void  *p     = tlsf_malloc(tlsf, total);
    if (p)
        memset(p, 0, total);
    return p;
}

void free(void *ptr)
{
    if (ptr)
        tlsf_free(tlsf, ptr);
}
