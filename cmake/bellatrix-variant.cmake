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

option(BELLATRIX_ENABLE_EMU68_BOARDS "Enable Emu68 expansion boards in Bellatrix" ON)
option(BELLATRIX_USE_MUSASHI_CPU "Use Musashi instead of Emu68 JIT as the Bellatrix CPU backend" OFF)
option(BELLATRIX_OSD "Show FPS/frame overlay on framebuffer" OFF)
option(BELLATRIX_ENABLE_MULTICORE "Enable multicore runtime (Core1=CPU Core2=Chipset Core3=IO)" OFF)
option(BELLATRIX_CORE_LOG "Enable per-core log tags [CORE0-HOST] [CORE1-CPU] [CORE2-CHIPSET] [CORE3-IO]" OFF)
option(BELLATRIX_PROFILE "Enable MMIO profiling instrumentation (zero cost when OFF)" OFF)
add_compile_definitions(BELLATRIX BELLATRIX_ENABLE_MINIUART_BACKEND BELLATRIX_ENABLE_PL011_BACKEND)
if(BELLATRIX_PROFILE)
    add_compile_definitions(BELLATRIX_PROFILE=1)
    message(STATUS "[BUILD] MMIO profiling: enabled (BELLATRIX_PROFILE=1)")
else()
    message(STATUS "[BUILD] MMIO profiling: disabled")
endif()
if(BELLATRIX_ENABLE_MULTICORE)
    add_compile_definitions(BELLATRIX_ENABLE_MULTICORE)
    message(STATUS "[BUILD] Multicore: enabled (Core1=CPU Core2=Chipset Core3=IO)")
else()
    message(STATUS "[BUILD] Multicore: disabled (single-core mode)")
endif()
if(BELLATRIX_CORE_LOG)
    add_compile_definitions(BELLATRIX_CORE_LOG)
    message(STATUS "[BUILD] Core log: enabled ([CORE0-HOST] [CORE1-CPU] [CORE2-CHIPSET] [CORE3-IO] [XCORE-*])")
else()
    message(STATUS "[BUILD] Core log: disabled")
endif()
list(APPEND BELLATRIX_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../src/cpu
    ${CMAKE_SOURCE_DIR}/../src/host
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
target_compile_options(rigel PRIVATE -mbig-endian -fno-stack-protector)
add_compile_definitions(BELLATRIX_USE_RIGEL_CHIPSET=1)
set(BELLATRIX_MACHINE_SOURCE
    ${CMAKE_SOURCE_DIR}/../src/machine/machine_rigel.c
    ${CMAKE_SOURCE_DIR}/../src/machine/machine_rigel_trace.c
    ${CMAKE_SOURCE_DIR}/../src/machine/machine_rigel_bus.c
    ${CMAKE_SOURCE_DIR}/../src/machine/machine_rigel_step.c
)
if(BELLATRIX_USE_MUSASHI_CPU)
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
    ${BELLATRIX_MACHINE_SOURCE}
    ${CMAKE_SOURCE_DIR}/../src/machine/expansion.c
    # Bus fabric and CD-ROM expansion
    ${CMAKE_SOURCE_DIR}/../src/machine/autoconfig/autoconfig.c
    ${CMAKE_SOURCE_DIR}/../src/machine/bus/zorro2/zorro2_bus.c
    ${CMAKE_SOURCE_DIR}/../src/machine/bus/zorro3/zorro3.c
    ${CMAKE_SOURCE_DIR}/../src/machine/bus/superbuster/superbuster.c
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
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/vc_mailbox.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/console_log.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/pal_debug.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/pal_ipl.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/pal_core.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/time.c
    ${CMAKE_SOURCE_DIR}/../src/host/raspi3/posix_time.c
    ${CMAKE_SOURCE_DIR}/../src/runtime/core_chipset.c
    # core_io.c provides the STRONG bellatrix_runtime_io_step (and
    # core_io_init/step).  Without it the weak no-op stub in pal_core.c
    # links instead and USB/BT silently stop after the launcher.
    ${CMAKE_SOURCE_DIR}/../src/runtime/core_io.c
    ${CMAKE_SOURCE_DIR}/../src/io/bluetooth/bt_host.c
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
endif()
if(BELLATRIX_ENABLE_EMU68_BOARDS)
    add_compile_definitions(BELLATRIX_ENABLE_EMU68_BOARDS=1)
    list(APPEND BASE_FILES
        src/boards/devicetree.c
        src/boards/z2ram.c
        src/boards/sdcard.c
        src/boards/68040.c
    )
else()
    add_compile_definitions(BELLATRIX_ENABLE_EMU68_BOARDS=0)
    set(BELLATRIX_LEGACY_Z2_RAM_MB "8" CACHE STRING
        "Z2 Fast RAM size in MB for the Bellatrix-owned autoconfig path (legacy non-boards mode)")
    add_compile_definitions(BELLATRIX_LEGACY_Z2_RAM_MB=${BELLATRIX_LEGACY_Z2_RAM_MB})
    list(APPEND BASE_FILES
        ${CMAKE_SOURCE_DIR}/../src/machine/bus/zorro2/zorro2_bus.c
    )
endif()
list(APPEND BELLATRIX_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/../src/io/bluetooth
    ${CMAKE_SOURCE_DIR}/../src/io/usb
)
option(BELLATRIX_ENABLE_BTSTACK "Enable BTStack support in Bellatrix" OFF)
option(BELLATRIX_ENABLE_USBSTACK "Enable CherryUSB host stack scaffold in Bellatrix" OFF)
option(BELLATRIX_ENABLE_USB_MSC "Enable CherryUSB USB mass-storage class in Bellatrix" ON)
add_compile_definitions(
    BELLATRIX_ENABLE_BTSTACK=$<BOOL:${BELLATRIX_ENABLE_BTSTACK}>
    BELLATRIX_ENABLE_USBSTACK=$<BOOL:${BELLATRIX_ENABLE_USBSTACK}>
    BELLATRIX_ENABLE_USB_MSC=$<BOOL:${BELLATRIX_ENABLE_USB_MSC}>
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
