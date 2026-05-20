# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
# doctest is found at the top level (find_package(doctest CONFIG REQUIRED)).

# vox_add_test(<name> SOURCES a.cpp ... LINK tgt ...)
function(vox_add_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LINK" ${ARGN})
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE doctest::doctest ${T_LINK})
    vox_configure_target(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

vox_add_test(cvar_roundtrip SOURCES cvar_roundtrip_test.cpp LINK vox_cvar)
vox_add_test(ispc_kernel    SOURCES ispc_kernel_test.cpp    LINK vox_ispc)
vox_add_test(command        SOURCES command_test.cpp        LINK vox_cvar)
vox_add_test(keybindings    SOURCES keybindings_test.cpp    LINK vox_platform)
vox_add_test(password       SOURCES password_test.cpp       LINK vox_console)
