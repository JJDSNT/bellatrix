# cmake/bellatrix-variant.cmake
#
# The Bellatrix variant of the Emu68 build: source lists, include paths and
# compile definitions. Included by the patched external/emu68/CMakeLists.txt
# when VARIANT=bellatrix.
#
# The point of the split is that patching Emu68 is expensive and adding a
# source file is not. The patch is a four-line hook that names this file; every
# subsequent change to what Bellatrix compiles happens here, in our own tree,
# and never touches the patch again.
#
# Context: this runs inside external/emu68/CMakeLists.txt, so CMAKE_SOURCE_DIR
# is external/emu68 and the Bellatrix tree is at ${CMAKE_SOURCE_DIR}/../..

if(NOT ${TARGET} STREQUAL "raspi64")
    message(FATAL_ERROR "Bellatrix variant is supported on the raspi64 target only.")
endif()

set(BELLATRIX_ROOT ${CMAKE_SOURCE_DIR}/../..)

set(BELLATRIX_INCLUDE_DIRS
    ${BELLATRIX_ROOT}/src
)

list(APPEND BASE_FILES
    ${BELLATRIX_ROOT}/src/machine/machine.c
    ${BELLATRIX_ROOT}/src/machine/bus.c
)

message(STATUS "[BUILD] Bellatrix machine: low 24-bit address domain protected")
