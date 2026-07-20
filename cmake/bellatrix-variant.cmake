# cmake/bellatrix-variant.cmake
# Entire Bellatrix variant definition for the Emu68 build: options, compile
# definitions, source lists, Rigel chipset, Musashi backend, BT/USB stacks
# and the launcher.  Included by the (patched) emu68/CMakeLists.txt when
# VARIANT=bellatrix, so source/option changes here never require touching
# the Emu68 patch again.
#
# Context: runs inside emu68/CMakeLists.txt, so CMAKE_SOURCE_DIR is emu68/
# and the Bellatrix tree is at ${CMAKE_SOURCE_DIR}/..

if(NOT ${TARGET} STREQUAL "raspi64")
    message(FATAL_ERROR "Bellatrix variant is supported on raspi64 target only.")
endif()

set(BELLATRIX_INCLUDE_DIRS "")
set(BELLATRIX_SOURCES "")
enable_language(ASM)

option(BELLATRIX_ENABLE_EMU68_BOARDS "Enable Emu68 expansion boards in Bellatrix" ON)
option(BELLATRIX_ENABLE_SDCARD_BOARD "Include Emu68 SD card Z3 board" ON)
option(BELLATRIX_ENABLE_DEVICETREE_BOARD "Include Emu68 devicetree.resource Z3 board" OFF)
option(BELLATRIX_ENABLE_68040_BOARD "Include Emu68 68040 support Z3 board" OFF)
option(BELLATRIX_USE_MUSASHI_CPU "Use Musashi instead of Emu68 JIT as the Bellatrix CPU backend" OFF)
set(BELLATRIX_MUSASHI_CPU "68040" CACHE STRING "Musashi CPU model for Bellatrix bare-metal: 68000, 68010, 68ec020, 68020, 68030, 68040")
option(BELLATRIX_OSD "Show FPS/frame overlay on framebuffer" OFF)
option(BELLATRIX_ENABLE_MULTICORE "Enable unified runtime (Core0=CPU Core1=Aux Core2=Chipset Core3=Host)" OFF)
option(BELLATRIX_EMU68_CORE0_REBASELINE
    "Keep native Emu68 JIT/exception integration on the common CPU Core 0" ON)
option(BELLATRIX_CORE_LOG "Enable runtime-role log tags [HOST] [CPU] [CHIPSET]" OFF)
option(BELLATRIX_BT_TRACE "Enable low-level BT HAL byte/block tracing (floods the log during scan/inquiry retries)" OFF)
option(BELLATRIX_PROFILE "Enable MMIO profiling instrumentation (zero cost when OFF)" OFF)
option(BELLATRIX_COARSE_OBSERVABLE_DEADLINES
    "Experimental: let Rigel process observable deadlines internally in Core-2 drains" OFF)
set(BELLATRIX_TIMELINE_MODE "realtime" CACHE STRING
    "Timeline policy: cpu, realtime, or hybrid")
set_property(CACHE BELLATRIX_TIMELINE_MODE PROPERTY STRINGS cpu realtime hybrid)
if(BELLATRIX_TIMELINE_MODE STREQUAL "cpu")
    add_compile_definitions(BELLATRIX_TIMELINE_DEFAULT=0)
elseif(BELLATRIX_TIMELINE_MODE STREQUAL "realtime")
    add_compile_definitions(BELLATRIX_TIMELINE_DEFAULT=1)
elseif(BELLATRIX_TIMELINE_MODE STREQUAL "hybrid")
    add_compile_definitions(BELLATRIX_TIMELINE_DEFAULT=2)
else()
    message(FATAL_ERROR "Invalid BELLATRIX_TIMELINE_MODE=${BELLATRIX_TIMELINE_MODE}")
endif()
add_compile_definitions(BELLATRIX BELLATRIX_ENABLE_MINIUART_BACKEND BELLATRIX_ENABLE_PL011_BACKEND)
if(BELLATRIX_PROFILE)
    add_compile_definitions(BELLATRIX_PROFILE=1)
    message(STATUS "[BUILD] MMIO profiling: enabled (BELLATRIX_PROFILE=1)")
else()
    message(STATUS "[BUILD] MMIO profiling: disabled")
endif()
if(BELLATRIX_COARSE_OBSERVABLE_DEADLINES)
    add_compile_definitions(BELLATRIX_COARSE_OBSERVABLE_DEADLINES=1)
    message(STATUS "[BUILD] EXPERIMENTAL coarse observable deadlines: enabled")
endif()
if(BELLATRIX_ENABLE_MULTICORE)
    add_compile_definitions(BELLATRIX_ENABLE_MULTICORE)
    if(BELLATRIX_EMU68_CORE0_REBASELINE AND NOT BELLATRIX_USE_MUSASHI_CPU)
        add_compile_definitions(BELLATRIX_EMU68_CORE0_REBASELINE=1)
        message(STATUS "[BUILD] native Emu68/Core0 integration enabled")
    else()
        add_compile_definitions(BELLATRIX_EMU68_CORE0_REBASELINE=0)
        message(STATUS "[BUILD] native Emu68 integration disabled (selected CPU still owns Core0)")
    endif()
else()
    add_compile_definitions(BELLATRIX_EMU68_CORE0_REBASELINE=0)
    message(STATUS "[BUILD] Multicore: disabled (single-core mode)")
endif()
if(BELLATRIX_ENABLE_MULTICORE)
    message(STATUS "[BUILD] topology: irq=Core0 cpu=Core0 chipset=Core2 host=Core3 aux=Core1")
endif()
if(BELLATRIX_CORE_LOG)
    add_compile_definitions(BELLATRIX_CORE_LOG)
    message(STATUS "[BUILD] Role log: enabled ([HOST] [CPU] [CHIPSET] [XCORE-*])")
else()
    message(STATUS "[BUILD] Core log: disabled")
endif()
if(BELLATRIX_BT_TRACE)
    add_compile_definitions(BELLATRIX_BT_TRACE)
    message(STATUS "[BUILD] BT HAL trace: enabled ([BT-HAL] [BT-RX] [BT-TX] byte/block dumps)")
else()
    message(STATUS "[BUILD] BT HAL trace: disabled")
endif()
list(APPEND BELLATRIX_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../src/cpu
    ${CMAKE_SOURCE_DIR}/../src/host
    ${CMAKE_SOURCE_DIR}/src
)
list(APPEND BELLATRIX_SOURCES
    ${CMAKE_SOURCE_DIR}/../src/io/hid/hid_router.c
)
# Rigel is THE Bellatrix chipset backend — not an option.
# (BELLATRIX_USE_RIGEL_CHIPSET=1 stays defined because sources still #if on it.)
list(APPEND BELLATRIX_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/../external/rigel/include
    ${CMAKE_SOURCE_DIR}/../external/rigel/src
    ${CMAKE_SOURCE_DIR}/../external/rigel/src/chipset
)
set(RIGEL_BUILD_TESTS OFF CACHE BOOL "Build Rigel unit tests" FORCE)
set(RIGEL_BUILD_HARNESS OFF CACHE BOOL "Build Rigel Musashi harness" FORCE)
set(RIGEL_ENABLE_STDIO_LOG OFF CACHE BOOL "Use stderr for Rigel logs" FORCE)
set(RIGEL_ENABLE_STDLIB_ENV OFF CACHE BOOL "Use getenv() for Rigel trace knobs" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/../external/rigel ${CMAKE_BINARY_DIR}/rigel-build EXCLUDE_FROM_ALL)
# The rigel target is created BEFORE Emu68's global
# add_compile_options(-ffixed-x12 -ffixed-q28..q31), so it does NOT inherit
# the pinned-register set. Rigel code runs inside the Emu68 MainLoop/JIT
# context (chipset advance from the progress hook), where x12 caches the
# translation-unit entry point and v28-v31 hold live JIT state: without
# these flags any rigel function may clobber them and crash the LastPC==PC
# fast dispatch (ISSUE-0038 — AROS checksum-loop halt into wfe).
target_compile_options(rigel PRIVATE -mbig-endian -fno-stack-protector
    -ffixed-x12 -ffixed-q31 -ffixed-q30 -ffixed-q29 -ffixed-q28
    # Pi 3 (BCM2837) is Cortex-A53; Emu68's global -mtune=cortex-a72 targets
    # the Pi 4 and mis-schedules for the in-order A53 pipeline. Rigel is the
    # hottest code in the system (~1.08 us/CCK, third Pi gate).
    -mtune=cortex-a53)
add_compile_definitions(BELLATRIX_USE_RIGEL_CHIPSET=1)
set(BELLATRIX_MACHINE_SOURCE
    ${CMAKE_SOURCE_DIR}/../src/machine/machine_rigel.c
    ${CMAKE_SOURCE_DIR}/../src/machine/machine_rigel_trace.c
    ${CMAKE_SOURCE_DIR}/../src/machine/machine_rigel_bus.c
    ${CMAKE_SOURCE_DIR}/../src/machine/machine_rigel_step.c
)
if(BELLATRIX_USE_MUSASHI_CPU)
    if(BELLATRIX_MUSASHI_CPU STREQUAL "68000")
        set(BELLATRIX_MUSASHI_CPU_MODEL "M68K_CPU_TYPE_68000")
    elseif(BELLATRIX_MUSASHI_CPU STREQUAL "68010")
        set(BELLATRIX_MUSASHI_CPU_MODEL "M68K_CPU_TYPE_68010")
    elseif(BELLATRIX_MUSASHI_CPU STREQUAL "68ec020")
        set(BELLATRIX_MUSASHI_CPU_MODEL "M68K_CPU_TYPE_68EC020")
    elseif(BELLATRIX_MUSASHI_CPU STREQUAL "68020")
        set(BELLATRIX_MUSASHI_CPU_MODEL "M68K_CPU_TYPE_68020")
    elseif(BELLATRIX_MUSASHI_CPU STREQUAL "68030")
        set(BELLATRIX_MUSASHI_CPU_MODEL "M68K_CPU_TYPE_68030")
    elseif(BELLATRIX_MUSASHI_CPU STREQUAL "68040")
        set(BELLATRIX_MUSASHI_CPU_MODEL "M68K_CPU_TYPE_68040")
    else()
        message(FATAL_ERROR "Invalid BELLATRIX_MUSASHI_CPU='${BELLATRIX_MUSASHI_CPU}'. Valid values: 68000, 68010, 68ec020, 68020, 68030, 68040")
    endif()
    message(STATUS "[BUILD] Musashi CPU model: ${BELLATRIX_MUSASHI_CPU}")

    find_program(BELLATRIX_HOST_CC NAMES cc gcc clang)
    if(NOT BELLATRIX_HOST_CC)
        message(FATAL_ERROR "Bellatrix Musashi CPU backend requested but no host C compiler was found")
    endif()
    set(BELLATRIX_MUSASHI_GEN_DIR ${CMAKE_BINARY_DIR}/musashi-generated)
    file(MAKE_DIRECTORY ${BELLATRIX_MUSASHI_GEN_DIR})
    execute_process(
        COMMAND ${BELLATRIX_HOST_CC} -O2 -o ${BELLATRIX_MUSASHI_GEN_DIR}/m68kmake ${CMAKE_SOURCE_DIR}/../external/musashi/m68kmake.c
        RESULT_VARIABLE BELLATRIX_M68KMAKE_CC_RESULT
    )
    if(NOT BELLATRIX_M68KMAKE_CC_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to build Musashi code generator with host compiler")
    endif()
    execute_process(
        COMMAND ${BELLATRIX_MUSASHI_GEN_DIR}/m68kmake ${BELLATRIX_MUSASHI_GEN_DIR}/ ${CMAKE_SOURCE_DIR}/../external/musashi/m68k_in.c
        RESULT_VARIABLE BELLATRIX_M68KMAKE_RUN_RESULT
    )
    if(NOT BELLATRIX_M68KMAKE_RUN_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to generate Musashi opcode tables")
    endif()
    list(APPEND BELLATRIX_INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/../external/musashi
        ${BELLATRIX_MUSASHI_GEN_DIR}
    )
    add_compile_definitions(BELLATRIX_USE_MUSASHI_CPU=1)
    add_compile_definitions(BELLATRIX_MUSASHI_CPU_MODEL=${BELLATRIX_MUSASHI_CPU_MODEL})
    add_compile_definitions(MUSASHI_CNF="cpu/musashi/musashi_baremetal_config.h")
else()
    add_compile_definitions(BELLATRIX_USE_MUSASHI_CPU=0)
endif()

# lide.rom → embedded C array (reuse the already-built ROM if present)
set(BELLATRIX_LIDE_ROM     ${CMAKE_SOURCE_DIR}/../external/lide.device/lide.rom)
set(BELLATRIX_LIDE_ROM_C   ${CMAKE_BINARY_DIR}/lide_rom_data.c)
find_program(BELLATRIX_PYTHON3 NAMES python3 python)
if(BELLATRIX_PYTHON3 AND EXISTS ${BELLATRIX_LIDE_ROM})
    add_custom_command(
        OUTPUT  ${BELLATRIX_LIDE_ROM_C}
        COMMAND ${BELLATRIX_PYTHON3}
                ${CMAKE_SOURCE_DIR}/../scripts/rom_to_c.py
                ${BELLATRIX_LIDE_ROM}
                ${BELLATRIX_LIDE_ROM_C}
        DEPENDS ${BELLATRIX_LIDE_ROM}
        COMMENT "Embedding lide.rom for Bellatrix bare-metal"
    )
    list(APPEND BASE_FILES ${BELLATRIX_LIDE_ROM_C})
else()
    message(WARNING "lide.rom or python3 not found — lide_cdrom will be stub-only in Bellatrix build")
    list(APPEND BASE_FILES ${CMAKE_SOURCE_DIR}/../src/machine/expansions/lide_cdrom/lide_rom_stub.c)
endif()

list(APPEND BASE_FILES
    ${CMAKE_SOURCE_DIR}/../src/cpu/emu68/bellatrix.c
    ${CMAKE_SOURCE_DIR}/../src/cpu/emu68/bellatrix_profile.c
    ${CMAKE_SOURCE_DIR}/../src/cpu/cpu_backend.c
    ${CMAKE_SOURCE_DIR}/../src/cpu/cpu_bridge.c
    ${CMAKE_SOURCE_DIR}/../src/cpu/direct_region.c
    ${CMAKE_SOURCE_DIR}/../src/cpu/mmio_policy.c
    ${BELLATRIX_MACHINE_SOURCE}
    ${CMAKE_SOURCE_DIR}/../src/machine/expansion.c
    # Bus fabric and CD-ROM expansion
    ${CMAKE_SOURCE_DIR}/../src/machine/autoconfig/autoconfig.c
    ${CMAKE_SOURCE_DIR}/../src/machine/bus/board_registry.c
    ${CMAKE_SOURCE_DIR}/../src/machine/bus/zorro_autoconfig.c
    ${CMAKE_SOURCE_DIR}/../src/machine/bus/superbuster/superbuster.c
    ${CMAKE_SOURCE_DIR}/../src/machine/expansions/z2_fast_ram/z2_fast_ram.c
    ${CMAKE_SOURCE_DIR}/../src/machine/expansions/lide_cdrom/lide_cdrom.c
    ${CMAKE_SOURCE_DIR}/../src/machine/expansions/lide_cdrom/ata_ide.c
    ${CMAKE_SOURCE_DIR}/../src/machine/expansions/lide_cdrom/atapi_cdrom.c
    # Bare-metal calloc/free backed by TLSF heap
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/pal_alloc.c
    ${CMAKE_SOURCE_DIR}/../src/debug/btrace.c
    ${CMAKE_SOURCE_DIR}/../src/debug/probe.c
    ${CMAKE_SOURCE_DIR}/../src/debug/debug.c
    ${CMAKE_SOURCE_DIR}/../src/debug/emu_debug.c
    ${CMAKE_SOURCE_DIR}/../src/debug/os_debug.c
    ${CMAKE_SOURCE_DIR}/../src/storage/iso/iso_image.c
    ${CMAKE_SOURCE_DIR}/../src/machine/memory/memory.c
    ${CMAKE_SOURCE_DIR}/../src/machine/memory/memory_map.c
    ${CMAKE_SOURCE_DIR}/../src/machine/memory/chip_ram.c
    ${CMAKE_SOURCE_DIR}/../src/machine/memory/fast_ram.c
    ${CMAKE_SOURCE_DIR}/../src/machine/memory/slow_ram.c
    ${CMAKE_SOURCE_DIR}/../src/machine/memory/overlay.c
    ${CMAKE_SOURCE_DIR}/../src/io/serial/null_modem.c
    ${CMAKE_SOURCE_DIR}/../src/io/serial/uart_host.c
    ${CMAKE_SOURCE_DIR}/../src/machine/input/controller_port.c
    ${CMAKE_SOURCE_DIR}/../src/machine/input/keyboard.c
    ${CMAKE_SOURCE_DIR}/../src/audio/mixer.c
    ${CMAKE_SOURCE_DIR}/../src/audio/output.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/miniuart_backend.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/pl011_backend.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/physical_interrupts.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/vc_mailbox.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/hdmi_audio.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/console_log.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/pal_debug.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/pal_ipl.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/pal_core.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/time.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/posix_time.c
    ${CMAKE_SOURCE_DIR}/../src/runtime/bringup.c
    ${CMAKE_SOURCE_DIR}/../src/runtime/core_chipset.c
    ${CMAKE_SOURCE_DIR}/../src/runtime/timeline.c
    ${CMAKE_SOURCE_DIR}/../src/runtime/posted_writes.c
    # core_io.c provides the STRONG bellatrix_runtime_io_step (and
    # core_io_init/step).  Without it the weak no-op stub in pal_core.c
    # links instead and USB/BT silently stop after the launcher.
    ${CMAKE_SOURCE_DIR}/../src/runtime/core_io.c
    ${CMAKE_SOURCE_DIR}/../src/io/bluetooth/bt_host.c
    ${CMAKE_SOURCE_DIR}/../src/io/bluetooth/bt_session.c
    ${CMAKE_SOURCE_DIR}/../src/io/usb/usb_host.c
)
if(BELLATRIX_USE_MUSASHI_CPU)
    list(APPEND BASE_FILES
        ${CMAKE_SOURCE_DIR}/../src/cpu/musashi/musashi_backend.c
        ${CMAKE_SOURCE_DIR}/../src/cpu/musashi/musashi_baremetal_stubs.c
        ${CMAKE_SOURCE_DIR}/../external/musashi/m68kcpu.c
        ${CMAKE_SOURCE_DIR}/../external/musashi/m68kdasm.c
        ${CMAKE_SOURCE_DIR}/../external/musashi/softfloat/softfloat.c
        ${BELLATRIX_MUSASHI_GEN_DIR}/m68kops.c
    )
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/../external/musashi/m68kcpu.c
        ${CMAKE_SOURCE_DIR}/../external/musashi/m68kdasm.c
        ${CMAKE_SOURCE_DIR}/../external/musashi/softfloat/softfloat.c
        ${BELLATRIX_MUSASHI_GEN_DIR}/m68kops.c
        PROPERTIES COMPILE_OPTIONS "-w"
    )
else()
    list(APPEND BASE_FILES
        ${CMAKE_SOURCE_DIR}/../src/cpu/emu68/emu68_native_backend.c
        ${CMAKE_SOURCE_DIR}/../src/cpu/emu68/emu68_direct_region.c
    )
endif()
if(BELLATRIX_ENABLE_EMU68_BOARDS)
    add_compile_definitions(BELLATRIX_ENABLE_EMU68_BOARDS=1)
    list(APPEND BASE_FILES
        src/boards/z2ram.c
    )
else()
    add_compile_definitions(BELLATRIX_ENABLE_EMU68_BOARDS=0)
    # Non-boards mode: Emu68's native z2ram/etc. are not compiled; the Bellatrix
    # Z2 Fast RAM board (board_registry, in BASE_FILES unconditionally) provides
    # expansion RAM at this size instead.
    set(BELLATRIX_LEGACY_Z2_RAM_MB "8" CACHE STRING
        "Z2 Fast RAM size in MB for the board_registry Fast RAM (non-boards mode)")
    add_compile_definitions(BELLATRIX_LEGACY_Z2_RAM_MB=${BELLATRIX_LEGACY_Z2_RAM_MB})
endif()
if(BELLATRIX_ENABLE_SDCARD_BOARD)
    list(APPEND BASE_FILES src/boards/sdcard.c)
    message(STATUS "[BUILD] Emu68 SD card board: enabled")
else()
    message(FATAL_ERROR "Bellatrix requires BELLATRIX_ENABLE_SDCARD_BOARD=ON")
endif()
if(BELLATRIX_ENABLE_DEVICETREE_BOARD)
    list(APPEND BASE_FILES src/boards/devicetree.c)
    message(STATUS "[BUILD] Emu68 devicetree board: enabled")
else()
    message(STATUS "[BUILD] Emu68 devicetree board: disabled")
endif()
if(BELLATRIX_ENABLE_68040_BOARD)
    if(BELLATRIX_USE_MUSASHI_CPU)
        message(FATAL_ERROR "Emu68 68040 support board must not be enabled with Musashi")
    endif()
    list(APPEND BASE_FILES src/boards/68040.c)
    message(STATUS "[BUILD] Emu68 68040 support board: enabled")
else()
    message(STATUS "[BUILD] Emu68 68040 support board: disabled")
endif()
list(APPEND BELLATRIX_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/../src/io/bluetooth
    ${CMAKE_SOURCE_DIR}/../src/io/usb
)
option(BELLATRIX_ENABLE_BTSTACK "Enable BTStack support in Bellatrix" OFF)
option(BELLATRIX_ENABLE_USBSTACK "Enable CherryUSB host stack scaffold in Bellatrix" OFF)
option(BELLATRIX_ENABLE_USB_MSC "Enable CherryUSB USB mass-storage class in Bellatrix" ON)
# ISSUE-0011: direct-register HDMI audio touches MMIO (MAI/HDMI/Clock
# Manager) that QEMU's raspi3b machine does not model -- an unhandled
# access there hangs boot before Kickstart even runs. Off by default until
# validated on real Pi 3B hardware; only real firmware images meant to test
# HDMI audio should turn this on.
option(BELLATRIX_ENABLE_HDMI_AUDIO "Enable direct-register HDMI audio output (real Pi hardware only, not QEMU)" OFF)
add_compile_definitions(
    BELLATRIX_ENABLE_BTSTACK=$<BOOL:${BELLATRIX_ENABLE_BTSTACK}>
    BELLATRIX_ENABLE_USBSTACK=$<BOOL:${BELLATRIX_ENABLE_USBSTACK}>
    BELLATRIX_ENABLE_USB_MSC=$<BOOL:${BELLATRIX_ENABLE_USB_MSC}>
    BELLATRIX_ENABLE_HDMI_AUDIO=$<BOOL:${BELLATRIX_ENABLE_HDMI_AUDIO}>
)
if(BELLATRIX_ENABLE_BTSTACK)
    set(BELLATRIX_BTSTACK_PATCHRAM_SOURCE "" CACHE STRING "Path to generated BCM PatchRAM C source for Bellatrix BTStack")
    list(APPEND BELLATRIX_SOURCES
        ${CMAKE_SOURCE_DIR}/../src/io/bluetooth/bt_hal_raspi3.c
        ${CMAKE_SOURCE_DIR}/../src/io/bluetooth/bt_scan.c
        ${CMAKE_SOURCE_DIR}/../src/io/bluetooth/bt_pairs.c
        ${CMAKE_SOURCE_DIR}/../src/io/bluetooth/bt_hid.c
        ${CMAKE_SOURCE_DIR}/../src/io/bluetooth/bt_link_key_db_sd.c
        ${CMAKE_SOURCE_DIR}/../src/io/bluetooth/bt_diag.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/ad_parser.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/btstack_linked_list.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/btstack_memory.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/btstack_memory_pool.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/btstack_run_loop.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/btstack_slip.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/btstack_util.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/btstack_uart_slip_wrapper.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/btstack_ltv_builder.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/btstack_hid.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/btstack_hid_parser.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/hci.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/hci_cmd.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/hci_dump.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/hci_transport_h4.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/hci_transport_h5.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/l2cap.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/l2cap_signaling.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/platform/embedded/btstack_run_loop_embedded.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/platform/embedded/btstack_uart_block_embedded.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/chipset/bcm/btstack_chipset_bcm.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/chipset/bcm/btstack_chipset_bcm_download_firmware.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/classic/sdp_util.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/classic/sdp_server.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/classic/sdp_client.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/classic/btstack_link_key_db_memory.c
        ${CMAKE_SOURCE_DIR}/../external/btstack/src/classic/hid_host.c
    )
    if(BELLATRIX_BTSTACK_PATCHRAM_SOURCE)
        message(STATUS "Bellatrix BTStack PatchRAM source: ${BELLATRIX_BTSTACK_PATCHRAM_SOURCE}")
        list(APPEND BELLATRIX_SOURCES ${BELLATRIX_BTSTACK_PATCHRAM_SOURCE})
    else()
        message(WARNING "Bellatrix BTStack PatchRAM source not provided; building without embedded BCM firmware")
        list(APPEND BELLATRIX_SOURCES
            ${CMAKE_SOURCE_DIR}/../src/io/bluetooth/bt_firmware_stub.c
        )
    endif()
    list(APPEND BELLATRIX_INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/../external/btstack/src
        ${CMAKE_SOURCE_DIR}/../external/btstack/platform/embedded
        ${CMAKE_SOURCE_DIR}/../external/btstack/chipset/bcm
        ${CMAKE_SOURCE_DIR}/../external/btstack/3rd-party/bluedroid/encoder/include
        ${CMAKE_SOURCE_DIR}/../external/btstack/3rd-party/bluedroid/decoder/include
        ${CMAKE_SOURCE_DIR}/../external/btstack/3rd-party/micro-ecc
        ${CMAKE_SOURCE_DIR}/../external/btstack/3rd-party/rijndael
        ${CMAKE_SOURCE_DIR}/../external/btstack/3rd-party/yxml
    )
    add_compile_definitions(BTSTACK_CONFIG_H="btstack_config.h")
    # BTSCAN.TXT / BTPAIRS.TXT placeholders: the bare-metal FAT32 writer only
    # overwrites an existing file in place (never allocates clusters), so ship
    # pre-sized target files alongside Emu68.img for the user to copy to the SD root.
    string(REPEAT " " 16384 _bellatrix_btscan_fill)
    file(WRITE ${CMAKE_BINARY_DIR}/BTSCAN.TXT "${_bellatrix_btscan_fill}")
    install(FILES ${CMAKE_BINARY_DIR}/BTSCAN.TXT DESTINATION .)
    string(REPEAT " " 512 _bellatrix_btpairs_fill)
    file(WRITE ${CMAKE_BINARY_DIR}/BTPAIRS.TXT "${_bellatrix_btpairs_fill}")
    install(FILES ${CMAKE_BINARY_DIR}/BTPAIRS.TXT DESTINATION .)
    string(REPEAT " " 512 _bellatrix_btkeys_fill)
    file(WRITE ${CMAKE_BINARY_DIR}/BTKEYS.TXT "${_bellatrix_btkeys_fill}")
    install(FILES ${CMAKE_BINARY_DIR}/BTKEYS.TXT DESTINATION .)
endif()
if(BELLATRIX_ENABLE_USBSTACK)
    if(EXISTS "${CMAKE_SOURCE_DIR}/../external/cherryusb/CMakeLists.txt")
        list(APPEND BELLATRIX_SOURCES
            ${CMAKE_SOURCE_DIR}/../src/io/usb/usb_libc_bellatrix.c
            ${CMAKE_SOURCE_DIR}/../src/io/usb/usb_osal_bellatrix.c
            ${CMAKE_SOURCE_DIR}/../src/io/usb/usbh_class_info.c
            ${CMAKE_SOURCE_DIR}/../src/io/usb/usb_hid_bellatrix.c
            ${CMAKE_SOURCE_DIR}/../src/io/usb/usb_msc_bellatrix.c
            ${CMAKE_SOURCE_DIR}/../src/io/usb/usb_glue_dwc2_bellatrix.c
            ${CMAKE_SOURCE_DIR}/../src/io/usb/usb_hc_bellatrix.c
            ${CMAKE_SOURCE_DIR}/../external/cherryusb/core/usbh_core.c
            ${CMAKE_SOURCE_DIR}/../external/cherryusb/class/hub/usbh_hub.c
            ${CMAKE_SOURCE_DIR}/../external/cherryusb/class/hid/usbh_hid.c
        )
        list(APPEND BELLATRIX_INCLUDE_DIRS
            ${CMAKE_SOURCE_DIR}/../external/cherryusb/common
            ${CMAKE_SOURCE_DIR}/../external/cherryusb/core
            ${CMAKE_SOURCE_DIR}/../external/cherryusb/class/hub
            ${CMAKE_SOURCE_DIR}/../external/cherryusb/class/hid
            ${CMAKE_SOURCE_DIR}/../external/cherryusb/port/dwc2
        )
        if(BELLATRIX_ENABLE_USB_MSC)
            list(APPEND BELLATRIX_SOURCES
                ${CMAKE_SOURCE_DIR}/../external/cherryusb/class/msc/usbh_msc.c
            )
            list(APPEND BELLATRIX_INCLUDE_DIRS
                ${CMAKE_SOURCE_DIR}/../external/cherryusb/class/msc
            )
            if(NOT BELLATRIX_LAUNCHER)
                # fat32 is required by usb_msc_bellatrix.c; normally provided
                # by the launcher block, but must be added here when launcher=OFF
                list(APPEND BELLATRIX_SOURCES
                    ${CMAKE_SOURCE_DIR}/../src/storage/fat/fat32.c
                    ${CMAKE_SOURCE_DIR}/../src/storage/fat/fat32_lfn.c
                    ${CMAKE_SOURCE_DIR}/../src/storage/fat/fat32_unicode.c
                )
            endif()
        endif()
    else()
        message(FATAL_ERROR "Bellatrix USB stack requested but external/cherryusb is missing")
    endif()
endif()
if(BELLATRIX_OSD)
    add_compile_definitions(BELLATRIX_OSD=1)
    list(APPEND BELLATRIX_SOURCES
        ${CMAKE_SOURCE_DIR}/../src/host/osd.c
    )
endif()
option(BELLATRIX_LAUNCHER "ADF selector UI on framebuffer (SD card FAT32)" ON)
if(BELLATRIX_LAUNCHER)
    add_compile_definitions(BELLATRIX_LAUNCHER=1)
    list(APPEND BELLATRIX_SOURCES
        ${CMAKE_SOURCE_DIR}/../src/launcher/launcher.c
        ${CMAKE_SOURCE_DIR}/../src/launcher/launcher_ui.c
        ${CMAKE_SOURCE_DIR}/../src/launcher/media_selection.c
        ${CMAKE_SOURCE_DIR}/../src/launcher/btscan.c
        ${CMAKE_SOURCE_DIR}/../src/launcher/launcher_input.c
        ${CMAKE_SOURCE_DIR}/../src/storage/fat/fat32.c
        ${CMAKE_SOURCE_DIR}/../src/storage/fat/fat32_lfn.c
        ${CMAKE_SOURCE_DIR}/../src/storage/fat/fat32_unicode.c
        ${CMAKE_SOURCE_DIR}/../src/storage/sdcard/bcm_emmc.c
    )
    message(STATUS "[BUILD] Launcher: enabled (SD FAT32 ADF+ISO selector)")
else()
    message(STATUS "[BUILD] Launcher: disabled")
endif()

list(APPEND EMU68_FILES ${BELLATRIX_SOURCES})
