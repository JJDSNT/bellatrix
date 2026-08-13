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

set(BELLATRIX_SOURCES
    ${BELLATRIX_ROOT}/src/machine/machine.c
    ${BELLATRIX_ROOT}/src/machine/bus.c
    ${BELLATRIX_ROOT}/src/machine/region.c
)

list(APPEND BASE_FILES ${BELLATRIX_SOURCES})

# Reserve the registers Emu68 reserves. This is not a precaution.
#
# Emu68 breaks the AArch64 C ABI on purpose: x13-x29 hold the m68k registers
# (M68k.h, REG_PC is 18), x12 holds the translation-unit entry point, and the
# m68k context pointer lives in a vector lane -- CTX_POINTER_ASM is "v20.d[1]".
# The files that participate in that convention are compiled with the registers
# pinned, one set per file, at the top of Emu68's CMakeLists.
#
# Our sources are called from src/aarch64/vectors.c and return into translated
# code, so they participate in exactly the same convention -- but a source
# listed in BASE_FILES inherits only the directory-level options, which pin
# x12 and nothing else. That gap is not theoretical and it is not survivable:
# under AAPCS64 the vector registers v16-v31 are caller-saved, so the compiler
# treats v20 as scratch, and a function of ours may clobber the m68k context
# pointer and never restore it. The legacy integration lost boots to precisely
# this -- a target created before Emu68's global flags, therefore compiled
# without them, clobbering pinned state inside the JIT context.
#
# So take the same set vectors.c takes, because that is who calls us.
# CONTEXT_RESERVE_FLAGS is Emu68's own variable, in scope here.
set_source_files_properties(${BELLATRIX_SOURCES} PROPERTIES COMPILE_FLAGS
    "-ffixed-x19 -ffixed-x20 -ffixed-x21 -ffixed-x22 -ffixed-x23 -ffixed-x24 \
     -ffixed-x25 -ffixed-x26 -ffixed-x27 -ffixed-x28 -ffixed-x29 \
     ${CONTEXT_RESERVE_FLAGS}")

message(STATUS "[BUILD] Bellatrix machine: low 24-bit address domain protected")
