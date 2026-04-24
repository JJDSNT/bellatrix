cmake_minimum_required(VERSION 3.14)
project(bellatrix_harness C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
set(ROOT    ${CMAKE_SOURCE_DIR}/../..)   # bellatrix/
set(SRC     ${ROOT}/src)
set(MUSASHI ${ROOT}/external/musashi)

# ---------------------------------------------------------------------------
# Generate m68kops.c + m68kops.h from Musashi's code generator
# ---------------------------------------------------------------------------
add_executable(m68kmake ${MUSASHI}/m68kmake.c)

add_custom_command(
    OUTPUT  ${CMAKE_BINARY_DIR}/m68kops.c
            ${CMAKE_BINARY_DIR}/m68kops.h
    COMMAND m68kmake ${CMAKE_BINARY_DIR}/ ${MUSASHI}/m68k_in.c
    DEPENDS m68kmake ${MUSASHI}/m68k_in.c
    COMMENT "Generating Musashi m68kops"
)

# ---------------------------------------------------------------------------
# Musashi library
# ---------------------------------------------------------------------------
add_library(musashi STATIC
    ${MUSASHI}/m68kcpu.c
    ${MUSASHI}/m68kdasm.c
    ${MUSASHI}/softfloat/softfloat.c
    ${CMAKE_BINARY_DIR}/m68kops.c
)

target_include_directories(musashi PUBLIC
    ${MUSASHI}
    ${CMAKE_BINARY_DIR}   # for generated m68kops.h
)

target_compile_options(musashi PRIVATE
    -w    # suppress Musashi's many warnings
)

# ---------------------------------------------------------------------------
# Bellatrix chipset sources (no Emu68, no ARM)
# ---------------------------------------------------------------------------
set(BELLATRIX_SOURCES
    ${SRC}/core/machine.c
    ${SRC}/memory/memory.c
    ${SRC}/chipset/agnus/agnus.c
    ${SRC}/chipset/agnus/beam.c
    ${SRC}/chipset/agnus/bitplanes.c
    ${SRC}/chipset/agnus/blitter.c
    ${SRC}/chipset/agnus/copper.c
    ${SRC}/chipset/agnus/dma.c
    ${SRC}/chipset/cia/cia.c
    ${SRC}/chipset/cia/cia_a.c
    ${SRC}/chipset/cia/cia_b.c
    ${SRC}/chipset/cia/cia_tod.c
    ${SRC}/chipset/denise/denise.c
    ${SRC}/chipset/paula/paula.c
    ${SRC}/chipset/paula/uart.c
    ${SRC}/chipset/rtc/rtc.c
    ${SRC}/debug/btrace.c
    ${SRC}/debug/debug.c
    ${SRC}/debug/emu_debug.c
    ${SRC}/debug/os_debug.c
    ${SRC}/debug/probe.c
    ${SRC}/host/posix/pal_posix.c
)

# ---------------------------------------------------------------------------
# Harness executable
# ---------------------------------------------------------------------------
add_executable(harness
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/musashi_backend.c
    ${BELLATRIX_SOURCES}
)

target_include_directories(harness PRIVATE
    # posix/ first so support.h shadows any Emu68 version
    ${SRC}/host/posix
    ${SRC}
    ${SRC}/cpu
    ${SRC}/host
    ${MUSASHI}
    ${CMAKE_BINARY_DIR}   # for generated m68kops.h
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(harness PRIVATE
    BELLATRIX_HARNESS=1
)

# SDL2 — optional.  Without it the harness runs headless (no display window).
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    message(STATUS "SDL2 found — harness will have display (interactive mode)")
    target_compile_definitions(harness PRIVATE BELLATRIX_SDL2=1)
    target_link_libraries(harness PRIVATE SDL2::SDL2)
else()
    message(STATUS "SDL2 not found — harness will run headless only")
endif()

target_link_libraries(harness PRIVATE musashi m)

target_compile_options(harness PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)
