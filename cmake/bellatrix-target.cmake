# cmake/bellatrix-target.cmake
#
# The half of the Bellatrix variant that needs the Emu68.elf target to exist:
# include directories, link libraries and target-level definitions. Included by
# the patched external/emu68/CMakeLists.txt immediately after the target is
# created.
#
# Kept separate from bellatrix-variant.cmake because CMake will not accept
# target_* commands before add_executable(), not because the two describe
# different things.

if(BELLATRIX_INCLUDE_DIRS)
    target_include_directories(Emu68.elf PRIVATE ${BELLATRIX_INCLUDE_DIRS})
endif()

target_compile_definitions(Emu68.elf PRIVATE BELLATRIX_EMU68=1)

if(CONFIG_RIGEL)
    # Emu68 defines this target with the plain target_link_libraries signature.
    target_link_libraries(Emu68.elf rigel)
    target_compile_definitions(Emu68.elf PRIVATE CONFIG_RIGEL=1)
    if(CONFIG_RIGEL_SELFTEST)
        target_compile_definitions(Emu68.elf PRIVATE CONFIG_RIGEL_SELFTEST=1)
    endif()
else()
    target_compile_definitions(Emu68.elf PRIVATE CONFIG_RIGEL=0)
endif()
