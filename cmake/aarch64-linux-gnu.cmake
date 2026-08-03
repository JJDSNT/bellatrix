# Cross toolchain for the bare-metal AArch64 build.
#
# Emu68 ships toolchains/aarch64-linux-gnu.cmake, but it pins gcc-14 by name.
# This file is the same thing without the version pin, so the build follows
# whatever the distribution installed. Kept here rather than patched into the
# submodule: a toolchain choice is ours, not upstream's.
#
# Override the compiler explicitly with -DCROSS_COMPILE_SUFFIX=-13 (or any
# other suffix) when several versions are installed and the default is wrong.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CROSS_COMPILE aarch64-linux-gnu)
set(CROSS_COMPILE_SUFFIX "" CACHE STRING "Version suffix of the cross compiler, e.g. -13")

set(CMAKE_C_COMPILER   ${CROSS_COMPILE}-gcc${CROSS_COMPILE_SUFFIX})
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE}-g++${CROSS_COMPILE_SUFFIX})
set(CMAKE_AR      ${CROSS_COMPILE}-ar)
set(CMAKE_RANLIB  ${CROSS_COMPILE}-ranlib)
set(CMAKE_OBJCOPY ${CROSS_COMPILE}-objcopy)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
