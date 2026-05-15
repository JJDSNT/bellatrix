#include "support.h"

#include <setjmp.h>
#include <stdarg.h>

void *stderr = 0;

int fprintf(void *stream, const char *format, ...)
{
    va_list args;
    int ret = 0;
    (void)stream;

    va_start(args, format);
    vkprintf(format, args);
    va_end(args);
    return ret;
}

int vfprintf(void *stream, const char *format, va_list args)
{
    (void)stream;
    vkprintf(format, args);
    return 0;
}

int __isoc99_sscanf(const char *str, const char *format, ...)
{
    (void)str;
    (void)format;
    return 0;
}

int _setjmp(jmp_buf env)
{
    (void)env;
    return 0;
}

void longjmp(jmp_buf env, int value)
{
    (void)env;
    kprintf("[MUSASHI] longjmp(%d) requested; halting\n", value);
    for (;;) {
        asm volatile("wfe");
    }
}

void exit(int code)
{
    kprintf("[MUSASHI] exit(%d) requested; halting\n", code);
    for (;;) {
        asm volatile("wfe");
    }
}
