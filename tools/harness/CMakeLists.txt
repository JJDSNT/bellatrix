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
    ${SRC}/cpu/direct_region.c

    # Autoconfig + bus fabric
    ${SRC}/machine/autoconfig/autoconfig.c
    ${SRC}/machine/bus/board_registry.c
    ${SRC}/machine/bus/zorro_autoconfig.c
    ${SRC}/machine/bus/zorro2/zorro2_bus.c
    ${SRC}/machine/expansions/z2_fast_ram/z2_fast_ram.c
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
    ${ROOT}/emu68/include
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
    BELLATRIX_USE_MUSASHI_CPU=1
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

add_executable(bellatrix_unit_zorro_autoconfig
    ${ROOT}/tests/unit/test_zorro_autoconfig.c
    ${SRC}/cpu/direct_region.c
    ${SRC}/machine/autoconfig/autoconfig.c
    ${SRC}/machine/bus/zorro_autoconfig.c
    ${SRC}/machine/bus/zorro2/zorro2_bus.c
    ${SRC}/machine/bus/zorro3/zorro3.c
)
target_include_directories(bellatrix_unit_zorro_autoconfig PRIVATE
    ${SRC}/host/posix
    ${SRC}
)
target_compile_options(bellatrix_unit_zorro_autoconfig PRIVATE -Wall -Wextra -Werror)
add_test(NAME bellatrix_unit_zorro_autoconfig
         COMMAND bellatrix_unit_zorro_autoconfig)

add_executable(bellatrix_unit_direct_region
    ${ROOT}/tests/unit/test_direct_region.c
    ${SRC}/cpu/direct_region.c
)
target_include_directories(bellatrix_unit_direct_region PRIVATE ${SRC})
target_compile_options(bellatrix_unit_direct_region PRIVATE -Wall -Wextra -Werror)
add_test(NAME bellatrix_unit_direct_region
         COMMAND bellatrix_unit_direct_region)

add_executable(bellatrix_unit_superbuster_z3
    ${ROOT}/tests/unit/test_superbuster_z3.c
    ${SRC}/machine/autoconfig/autoconfig.c
    ${SRC}/machine/bus/superbuster/superbuster.c
    ${SRC}/machine/bus/board_registry.c
    ${SRC}/machine/bus/zorro3/zorro3.c
)
target_include_directories(bellatrix_unit_superbuster_z3 PRIVATE
    ${SRC}/host/posix
    ${SRC}
    ${ROOT}/emu68/include
)
target_compile_options(bellatrix_unit_superbuster_z3 PRIVATE -Wall -Wextra -Werror)
add_test(NAME bellatrix_unit_superbuster_z3
         COMMAND bellatrix_unit_superbuster_z3)

add_executable(bellatrix_unit_board_registry
    ${ROOT}/tests/unit/test_board_registry.c
    ${SRC}/machine/bus/board_registry.c
    ${SRC}/cpu/direct_region.c
)
target_include_directories(bellatrix_unit_board_registry PRIVATE
    ${SRC}/host/posix
    ${SRC}
    ${ROOT}/emu68/include
)
target_compile_options(bellatrix_unit_board_registry PRIVATE -Wall -Wextra -Werror)
add_test(NAME bellatrix_unit_board_registry
         COMMAND bellatrix_unit_board_registry)

# Pure host-testable protocol modules (no PAL, no machine dependencies).
add_executable(bellatrix_unit_timeline
    ${ROOT}/tests/unit/test_timeline.c
    ${SRC}/runtime/timeline.c
)
target_include_directories(bellatrix_unit_timeline PRIVATE ${SRC})
target_compile_options(bellatrix_unit_timeline PRIVATE -Wall -Wextra)
add_test(NAME bellatrix_unit_timeline COMMAND bellatrix_unit_timeline)

add_executable(bellatrix_unit_mmio_policy
    ${ROOT}/tests/unit/test_mmio_policy.c
    ${SRC}/cpu/mmio_policy.c
)
target_include_directories(bellatrix_unit_mmio_policy PRIVATE ${SRC})
target_compile_options(bellatrix_unit_mmio_policy PRIVATE -Wall -Wextra)
add_test(NAME bellatrix_unit_mmio_policy COMMAND bellatrix_unit_mmio_policy)

add_executable(bellatrix_unit_posted_writes
    ${ROOT}/tests/unit/test_posted_writes.c
    ${SRC}/runtime/posted_writes.c
)
target_include_directories(bellatrix_unit_posted_writes PRIVATE ${SRC})
target_compile_options(bellatrix_unit_posted_writes PRIVATE -Wall -Wextra)
add_test(NAME bellatrix_unit_posted_writes COMMAND bellatrix_unit_posted_writes)

# Real-thread SPSC stress under ThreadSanitizer (ISSUE-0049 Fase 2): proves
# the acquire/release pairs of the cross-core queues, not just their logic.
add_executable(bellatrix_tsan_posted_writes
    ${ROOT}/tests/unit/test_posted_writes_threads.c
    ${SRC}/runtime/posted_writes.c
)
target_include_directories(bellatrix_tsan_posted_writes PRIVATE ${SRC})
target_compile_options(bellatrix_tsan_posted_writes PRIVATE
    -Wall -Wextra -g -fsanitize=thread)
target_link_options(bellatrix_tsan_posted_writes PRIVATE -fsanitize=thread)
target_link_libraries(bellatrix_tsan_posted_writes PRIVATE pthread)
# setarch -R disables ASLR: TSAN aborts with "unexpected memory mapping"
# under the high-entropy layout of recent kernels (vm.mmap_rnd_bits=32).
add_test(NAME bellatrix_tsan_posted_writes
         COMMAND setarch ${CMAKE_HOST_SYSTEM_PROCESSOR} -R
                 $<TARGET_FILE:bellatrix_tsan_posted_writes>)
set_tests_properties(bellatrix_tsan_posted_writes PROPERTIES TIMEOUT 120)

add_executable(bellatrix_tsan_audio_queue
    ${ROOT}/tests/unit/test_audio_queue_threads.c
    ${SRC}/audio/output.c
)
target_include_directories(bellatrix_tsan_audio_queue PRIVATE
    ${SRC}
    ${ROOT}/external/rigel/include
)
target_compile_options(bellatrix_tsan_audio_queue PRIVATE
    -Wall -Wextra -g -fsanitize=thread)
target_link_options(bellatrix_tsan_audio_queue PRIVATE -fsanitize=thread)
target_link_libraries(bellatrix_tsan_audio_queue PRIVATE pthread)
add_test(NAME bellatrix_tsan_audio_queue
         COMMAND setarch ${CMAKE_HOST_SYSTEM_PROCESSOR} -R
                 $<TARGET_FILE:bellatrix_tsan_audio_queue>)
set_tests_properties(bellatrix_tsan_audio_queue PROPERTIES TIMEOUT 120)


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
                 ${ROOT}/src/disks/workbench31.adf)

add_test(NAME bellatrix_harness_no_autoconfig
         COMMAND /bin/bash
                 ${ROOT}/tests/integration/harness_no_autoconfig.sh
                 ${CMAKE_BINARY_DIR}/harness
                 ${ROOT}/src/roms/KS31.rom
                 ${ROOT}/src/disks/workbench31.adf)

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
