cmake_minimum_required(VERSION 3.14)
project(bellatrix_harness C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

enable_testing()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
get_filename_component(ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)  # bellatrix/
set(SRC     ${ROOT}/src)
set(MUSASHI ${ROOT}/external/musashi)
set(LIDE    ${ROOT}/external/lide.device)
set(ODFS    ${ROOT}/external/ODFileSystem)
set(RTGCARD ${ROOT}/cards/bellatrix.card)
set(PLUGINS_OUT ${CMAKE_BINARY_DIR}/plugins)

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

target_compile_definitions(musashi PRIVATE
    BELLATRIX_HARNESS=1
)

# ---------------------------------------------------------------------------
# lide.rom — build from source via Docker, embed as C array
# ---------------------------------------------------------------------------
set(LIDE_ROM     ${LIDE}/lide.rom)
set(LIDE_ROM_C   ${CMAKE_BINARY_DIR}/lide_rom_data.c)

add_custom_command(
    OUTPUT  ${LIDE_ROM}
    COMMAND ${ROOT}/scripts/build-lide-rom.sh ${LIDE_ROM}
    DEPENDS
        ${LIDE}/device.c
        ${LIDE}/ata.c
        ${LIDE}/atapi.c
        ${LIDE}/scsi.c
        ${LIDE}/iotask.c
        ${LIDE}/lide_alib.c
        ${LIDE}/bootrom/bootldr.S
        ${LIDE}/bootrom/reloc.S
        ${LIDE}/bootrom/mungerom.py
        ${ROOT}/scripts/build-lide-rom.sh
        ${ROOT}/scripts/make_lide_rom.py
    COMMENT "Building lide.rom via Docker m68k toolchain"
)

add_custom_command(
    OUTPUT  ${LIDE_ROM_C}
    COMMAND Python3::Interpreter ${ROOT}/scripts/rom_to_c.py ${LIDE_ROM} ${LIDE_ROM_C}
    DEPENDS ${LIDE_ROM} ${ROOT}/scripts/rom_to_c.py
    COMMENT "Embedding lide.rom as C array"
)

# ---------------------------------------------------------------------------
# rtg.rom — bellatrix.rtg DiagArea loader + P96 card, build via Docker,
# embed as C array (served by the RTG board's own ROM region, see
# src/machine/expansions/rtg/rtg.c). Independent of the lide ROM above.
# ---------------------------------------------------------------------------
set(RTG_ROM   ${RTGCARD}/rtg.rom)
set(RTG_ROM_C ${CMAKE_BINARY_DIR}/rtg_rom_data.c)

add_custom_command(
    OUTPUT  ${RTG_ROM}
    COMMAND ${ROOT}/scripts/build-rtg-rom.sh ${RTG_ROM}
    DEPENDS
        ${RTGCARD}/src/card.c
        ${RTGCARD}/src/boardinfo.h
        ${RTGCARD}/src/settings.h
        ${RTGCARD}/Makefile
        ${RTGCARD}/bootrom/cardldr.S
        ${RTGCARD}/bootrom/reloc.S
        ${RTGCARD}/bootrom/defs.i
        ${RTGCARD}/bootrom/Makefile
        ${ROOT}/scripts/build-rtg-rom.sh
    COMMENT "Building rtg.rom (bellatrix.card + DiagArea loader) via Docker"
)

add_custom_command(
    OUTPUT  ${RTG_ROM_C}
    COMMAND Python3::Interpreter ${ROOT}/scripts/rom_to_c.py
            ${RTG_ROM} ${RTG_ROM_C} --symbol rtg_rom
    DEPENDS ${RTG_ROM} ${ROOT}/scripts/rom_to_c.py
    COMMENT "Embedding rtg.rom as C array"
)

add_library(rtg_rom_data STATIC ${RTG_ROM_C})
set_target_properties(rtg_rom_data PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# ODFileSystem handler — build via Docker, embed for second ROM bank
# ---------------------------------------------------------------------------
set(ODFS_BIN ${ODFS}/build/amiga/ODFileSystem)
set(ODFS_C   ${CMAKE_BINARY_DIR}/odfs_data.c)

# Track ALL ODFS sources — a stale embedded handler (e.g. cache_block.c
# edited without triggering a rebuild) is indistinguishable from a real
# regression and has cost us a full debugging session (ISSUE-0023).
file(GLOB_RECURSE ODFS_SOURCES
    ${ODFS}/core/*.c
    ${ODFS}/backends/*.c
    ${ODFS}/platform/amiga/*.c
    ${ODFS}/platform/amiga/*.S
    ${ODFS}/include/*.h
    ${ODFS}/Makefile
)

add_custom_command(
    OUTPUT  ${ODFS_BIN}
    COMMAND ${ROOT}/scripts/build-odfs.sh ${ODFS_BIN}
    DEPENDS
        ${ODFS_SOURCES}
        ${ROOT}/scripts/build-odfs.sh
        ${ROOT}/scripts/odfs_crosstools_compat.h
    COMMENT "Building ODFileSystem (m68k) via Docker"
)

add_custom_command(
    OUTPUT  ${ODFS_C}
    COMMAND Python3::Interpreter ${ROOT}/scripts/rom_to_c.py
            ${ODFS_BIN} ${ODFS_C} --symbol odfs
    DEPENDS ${ODFS_BIN} ${ROOT}/scripts/rom_to_c.py
    COMMENT "Embedding ODFileSystem binary as C array"
)

add_library(odfs_data STATIC ${ODFS_C})
set_target_properties(odfs_data PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(lide_rom_data STATIC ${LIDE_ROM_C})

set_target_properties(lide_rom_data PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# ---------------------------------------------------------------------------
# Bellatrix chipset sources
# ---------------------------------------------------------------------------
add_subdirectory(${ROOT}/external/rigel ${CMAKE_BINARY_DIR}/rigel-build EXCLUDE_FROM_ALL)

set(BELLATRIX_SOURCES

    # Core
    ${SRC}/machine/machine_rigel.c
    ${SRC}/machine/machine_rigel_trace.c
    ${SRC}/machine/machine_rigel_bus.c
    ${SRC}/machine/machine_rigel_step.c
    ${SRC}/machine/expansion.c
    ${SRC}/cpu/cpu_bridge.c
    ${SRC}/plugin/plugin_loader.c
    ${SRC}/plugin/plugin_manifest.c

    # Autoconfig + bus fabric
    ${SRC}/machine/autoconfig/autoconfig.c
    ${SRC}/machine/bus/zorro2/zorro2_bus.c
    ${SRC}/machine/bus/zorro3/zorro3.c
    ${SRC}/machine/bus/superbuster/superbuster.c

    # lide.device CD-ROM expansion
    ${SRC}/machine/expansions/lide_cdrom/ata_ide.c
    ${SRC}/machine/expansions/lide_cdrom/atapi_cdrom.c
    ${SRC}/machine/expansions/lide_cdrom/lide_cdrom.c
    ${SRC}/machine/expansions/rtg/rtg.c

    # Memory
    ${SRC}/machine/memory/memory.c
    ${SRC}/machine/memory/chip_ram.c
    ${SRC}/machine/memory/fast_ram.c
    ${SRC}/machine/memory/slow_ram.c
    ${SRC}/machine/memory/overlay.c
    ${SRC}/machine/memory/memory_map.c

    ${SRC}/io/serial/null_modem.c
    ${SRC}/io/serial/serial_backend.c
    ${SRC}/io/serial/pty_backend.c
    ${SRC}/io/serial/uart_host.c
    ${SRC}/cpu/cpu_backend.c

    ${SRC}/storage/iso/iso_image.c

    # Input
    ${SRC}/machine/input/controller_port.c
    ${SRC}/machine/input/keyboard.c

    # Audio
    ${SRC}/audio/mixer.c
    ${SRC}/audio/output.c

    # Debug
    ${SRC}/debug/btrace.c
    ${SRC}/debug/debug.c
    ${SRC}/debug/emu_debug.c
    ${SRC}/debug/os_debug.c
    ${SRC}/debug/probe.c

    # Host
    ${SRC}/host/posix/pal_posix.c
    ${SRC}/host/raspi3/miniuart_backend.c
    ${SRC}/host/raspi3/pl011_backend.c

    # Runtime
    ${SRC}/runtime/affinity.c
)

# ---------------------------------------------------------------------------
# ODFileSystem host library (libodfs)
# Provides ISO 9660 / Rock Ridge / Joliet / UDF reading on the host side.
# ---------------------------------------------------------------------------
add_library(odfs STATIC
    ${ODFS}/core/error.c
    ${ODFS}/core/log.c
    ${ODFS}/core/node.c
    ${ODFS}/core/namefix.c
    ${ODFS}/core/cache_block.c
    ${ODFS}/core/charset.c
    ${ODFS}/core/ancestry.c
    ${ODFS}/core/mount.c
    ${ODFS}/core/session.c
    ${ODFS}/backends/iso9660/iso9660.c
    ${ODFS}/backends/rock_ridge/rock_ridge.c
    ${ODFS}/backends/joliet/joliet.c
    ${ODFS}/backends/udf/udf.c
    ${ODFS}/backends/hfs/hfs.c
    ${ODFS}/backends/hfsplus/hfsplus.c
    ${ODFS}/backends/cdda/cdda.c
    ${ODFS}/platform/host/file_media.c
)

target_include_directories(odfs PUBLIC
    ${ODFS}/include
    ${ODFS}/backends
)

target_compile_definitions(odfs PRIVATE
    ODFS_PLATFORM_HOST=1
)

# Suppress warnings from the external library
target_compile_options(odfs PRIVATE -w)

# ---------------------------------------------------------------------------
# Harness executable
# ---------------------------------------------------------------------------
add_executable(harness
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/screenshot.c
    ${CMAKE_CURRENT_SOURCE_DIR}/musashi_backend.c
    ${SRC}/storage/iso/odfs_iso.c
    ${BELLATRIX_SOURCES}
)

target_include_directories(harness PRIVATE
    ${SRC}/host/posix
    ${SRC}
    ${SRC}/cpu
    ${SRC}/host
    ${SRC}/boards
    ${MUSASHI}
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ODFS}/include
    ${ODFS}/backends
    ${ROOT}/external/rigel/src
    ${ROOT}/external/rigel/src/chipset
)

target_compile_definitions(harness PRIVATE BELLATRIX_USE_RIGEL_CHIPSET=1)

target_link_options(harness PRIVATE -rdynamic)

target_compile_definitions(harness PRIVATE
    BELLATRIX_HARNESS=1
    BELLATRIX_ENABLE_PTY_BACKEND=1
    ODFS_PLATFORM_HOST=1
    BELLATRIX_ENABLE_ODFS=1
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
find_package(Threads REQUIRED)
target_link_libraries(harness PRIVATE musashi m dl Threads::Threads)
target_link_libraries(harness PRIVATE lide_rom_data odfs_data odfs rigel rtg_rom_data)

target_compile_options(harness PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
add_executable(bellatrix_unit_memory
    ${ROOT}/tests/unit/test_memory_map.c
    ${SRC}/machine/memory/memory.c
    ${SRC}/machine/memory/chip_ram.c
    ${SRC}/machine/memory/fast_ram.c
    ${SRC}/machine/memory/slow_ram.c
    ${SRC}/machine/memory/overlay.c
    ${SRC}/machine/memory/memory_map.c
    ${SRC}/machine/autoconfig/autoconfig.c
    ${SRC}/machine/bus/zorro2/zorro2_bus.c
)

target_include_directories(bellatrix_unit_memory PRIVATE
    ${SRC}/host/posix
    ${SRC}
)

target_compile_definitions(bellatrix_unit_memory PRIVATE
    BELLATRIX_HARNESS=1
)

target_compile_options(bellatrix_unit_memory PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

add_test(NAME bellatrix_unit_memory
         COMMAND bellatrix_unit_memory)


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

# ROM boot display regression tests: each ROM must reach its boot screen
# (display programmed + non-flat frame) with no media inserted.
# Skipped (exit 77) when the ROM file is not present.
foreach(rom_boot
        "ks13;KS13.rom;300"
        "ks20;KS20.rom;600"
        "ks31;KS31.rom;600"
        "aros;aros.rom;800")
    list(GET rom_boot 0 rb_name)
    list(GET rom_boot 1 rb_rom)
    list(GET rom_boot 2 rb_frame)
    add_test(NAME bellatrix_rom_boot_${rb_name}
             COMMAND /bin/bash
                     ${ROOT}/tests/integration/harness_rom_boot.sh
                     ${CMAKE_BINARY_DIR}/harness
                     ${ROOT}/src/roms/${rb_rom}
                     ${rb_frame})
    set_tests_properties(bellatrix_rom_boot_${rb_name} PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
        TIMEOUT 300
        SKIP_RETURN_CODE 77
    )
endforeach()

add_library(lide_cdrom MODULE
    ${SRC}/machine/autoconfig/autoconfig.c
    ${SRC}/machine/bus/zorro2/zorro2_bus.c
    ${SRC}/machine/bus/zorro3/zorro3.c
    ${SRC}/machine/bus/superbuster/superbuster.c
    ${SRC}/machine/expansions/lide_cdrom/ata_ide.c
    ${SRC}/machine/expansions/lide_cdrom/atapi_cdrom.c
    ${SRC}/machine/expansions/lide_cdrom/lide_cdrom.c
    ${SRC}/storage/iso/iso_image.c
)

set_target_properties(lide_cdrom PROPERTIES
    PREFIX ""
    OUTPUT_NAME "lide_cdrom"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/modules/lide_cdrom
)

target_include_directories(lide_cdrom PRIVATE
    ${SRC}/host/posix
    ${SRC}
)

target_compile_definitions(lide_cdrom PRIVATE
    BELLATRIX_HARNESS=1
    BELLATRIX_ENABLE_ODFS=1
)

target_link_libraries(lide_cdrom PRIVATE lide_rom_data odfs_data)
