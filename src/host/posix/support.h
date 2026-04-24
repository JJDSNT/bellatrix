// src/host/posix/support.h
//
// Host-side shim for Emu68's support.h.
// Provides kprintf and BE32 for the harness build (Linux x86_64).
// Included by chipset sources that do #include "support.h" — the harness
// CMakeLists puts src/host/posix/ first in the include path so this file
// shadows Emu68's support.h without modifying any chipset source.

#pragma once
#include <stdio.h>
#include <stdint.h>

#define kprintf(...)      printf(__VA_ARGS__)
#define BE32(x)           __builtin_bswap32(x)
#define BE16(x)           __builtin_bswap16(x)
/* LE16: Amiga palette is big-endian; on x86 host no swap is needed
 * (we have no real framebuffer in the harness anyway). */
#define LE16(x)           (x)
