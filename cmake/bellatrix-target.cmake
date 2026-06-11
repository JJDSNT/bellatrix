# cmake/bellatrix-target.cmake
# Bellatrix hooks that need the Emu68.elf target to exist (link libraries,
# include dirs, target-level defines).  Included by the patched
# emu68/CMakeLists.txt right after the target is created.

if(TARGET rigel)
    target_link_libraries(Emu68.elf rigel)
endif()
if(BELLATRIX_INCLUDE_DIRS)
    target_include_directories(Emu68.elf PRIVATE ${BELLATRIX_INCLUDE_DIRS})
endif()
target_compile_definitions(Emu68.elf PRIVATE BELLATRIX_EMU68=1)
