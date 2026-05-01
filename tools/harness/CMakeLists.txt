cmake_minimum_required(VERSION 3.14)
project(bellatrix_harness C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

enable_testing()

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

# Musashi's offline code generator is a host-side helper. Keep it out of
# sanitizer instrumentation so ASan builds still generate m68kops reliably.
target_compile_options(m68kmake PRIVATE -fno-sanitize=all)
target_link_options(m68kmake PRIVATE -fno-sanitize=all)

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
    -w
)

# ---------------------------------------------------------------------------
# Bellatrix chipset sources
# ---------------------------------------------------------------------------
set(BELLATRIX_SOURCES

    # Core
    ${SRC}/core/machine.c

    # Memory (nova base)
    ${SRC}/core/memory/memory.c
    ${SRC}/core/memory/chip_ram.c
    ${SRC}/core/memory/fast_ram.c
    ${SRC}/core/memory/overlay.c
    ${SRC}/core/memory/memory_map.c
    ${SRC}/core/memory/autoconfig.c

    # Agnus
    ${SRC}/chipset/agnus/agnus.c
    ${SRC}/chipset/agnus/beam.c
    ${SRC}/chipset/agnus/bitplanes.c
    ${SRC}/chipset/agnus/blitter.c
    ${SRC}/chipset/agnus/dma.c

    # Copper (NOVO LAYOUT)
    ${SRC}/chipset/agnus/copper/copper.c
    ${SRC}/chipset/agnus/copper/copper_regs.c
    ${SRC}/chipset/agnus/copper/copper_service.c

    # CIA
    ${SRC}/chipset/cia/cia.c
    ${SRC}/chipset/cia/cia_a.c
    ${SRC}/chipset/cia/cia_b.c
    ${SRC}/chipset/cia/cia_tod.c

    # Denise
    ${SRC}/chipset/denise/denise.c

    # Floppy
    ${SRC}/chipset/floppy/floppy_drive.c

    # Paula
    ${SRC}/chipset/paula/paula.c
    ${SRC}/chipset/paula/paula_disk.c
    ${SRC}/chipset/paula/uart.c

    # RTC
    ${SRC}/chipset/rtc/rtc.c

    # Input
    ${SRC}/input/keyboard.c

    # Debug
    ${SRC}/debug/btrace.c
    ${SRC}/debug/debug.c
    ${SRC}/debug/emu_debug.c
    ${SRC}/debug/os_debug.c
    ${SRC}/debug/probe.c

    # Host
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
    ${SRC}/host/posix
    ${SRC}
    ${SRC}/core
    ${SRC}/cpu
    ${SRC}/host
    ${MUSASHI}
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(harness PRIVATE
    BELLATRIX_HARNESS=1
)

# ---------------------------------------------------------------------------
# SDL2 (optional)
# ---------------------------------------------------------------------------
find_package(SDL2 QUIET)

if(SDL2_FOUND)
    message(STATUS "SDL2 found — harness will have display")
    target_compile_definitions(harness PRIVATE BELLATRIX_SDL2=1)
    target_link_libraries(harness PRIVATE SDL2::SDL2)
else()
    message(STATUS "SDL2 not found — harness headless")
endif()

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------
target_link_libraries(harness PRIVATE musashi m)

target_compile_options(harness PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
add_executable(bellatrix_unit_memory
    ${ROOT}/tests/unit/test_memory_map.c
    ${SRC}/core/memory/memory.c
    ${SRC}/core/memory/chip_ram.c
    ${SRC}/core/memory/fast_ram.c
    ${SRC}/core/memory/overlay.c
    ${SRC}/core/memory/memory_map.c
    ${SRC}/core/memory/autoconfig.c
)

target_include_directories(bellatrix_unit_memory PRIVATE
    ${SRC}/host/posix
    ${SRC}
    ${SRC}/core
)

target_compile_definitions(bellatrix_unit_memory PRIVATE
    BELLATRIX_HARNESS=1
)

target_compile_options(bellatrix_unit_memory PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

add_test(NAME bellatrix_unit_memory
         COMMAND bellatrix_unit_memory)

add_executable(bellatrix_unit_uart
    ${ROOT}/tests/unit/test_uart_null_modem.c
    ${SRC}/chipset/paula/uart.c
)

target_include_directories(bellatrix_unit_uart PRIVATE
    ${SRC}/host/posix
    ${SRC}
    ${SRC}/core
)

target_compile_definitions(bellatrix_unit_uart PRIVATE
    BELLATRIX_HARNESS=1
)

target_compile_options(bellatrix_unit_uart PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

add_test(NAME bellatrix_unit_uart
         COMMAND bellatrix_unit_uart)

add_executable(bellatrix_integration_overlay
    ${ROOT}/tests/integration/test_machine_overlay.c
    ${BELLATRIX_SOURCES}
)

target_include_directories(bellatrix_integration_overlay PRIVATE
    ${SRC}/host/posix
    ${SRC}
    ${SRC}/core
    ${SRC}/cpu
    ${SRC}/host
)

target_compile_definitions(bellatrix_integration_overlay PRIVATE
    BELLATRIX_HARNESS=1
)

target_link_libraries(bellatrix_integration_overlay PRIVATE m)

target_compile_options(bellatrix_integration_overlay PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

add_test(NAME bellatrix_integration_overlay
         COMMAND bellatrix_integration_overlay)

add_test(NAME bellatrix_harness_smoke
         COMMAND /bin/bash
                 ${ROOT}/tests/integration/harness_smoke.sh
                 ${CMAKE_BINARY_DIR}/harness
                 ${ROOT}/src/roms/KS31.rom)

add_test(NAME bellatrix_harness_boot_adf
         COMMAND /bin/bash
                 ${ROOT}/tests/integration/harness_boot_adf.sh
                 ${CMAKE_BINARY_DIR}/harness
                 ${ROOT}/src/roms/KS31.rom
                 ${ROOT}/src/disks/wb31.adf)

add_test(NAME bellatrix_harness_no_autoconfig
         COMMAND /bin/bash
                 ${ROOT}/tests/integration/harness_no_autoconfig.sh
                 ${CMAKE_BINARY_DIR}/harness
                 ${ROOT}/src/roms/KS31.rom
                 ${ROOT}/src/disks/wb31.adf)

set_tests_properties(bellatrix_unit_memory PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)

set_tests_properties(bellatrix_integration_overlay PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)

set_tests_properties(bellatrix_harness_smoke PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
    TIMEOUT 5
)

set_tests_properties(bellatrix_harness_boot_adf PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
    TIMEOUT 5
)

set_tests_properties(bellatrix_harness_no_autoconfig PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
    TIMEOUT 5
)
