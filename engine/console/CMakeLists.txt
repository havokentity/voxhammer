# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(vox_console STATIC
    ConsoleServer.cpp
    ConsoleServer.h
    Password.cpp
    Password.h
)

target_link_libraries(vox_console
    PUBLIC
        vox_includes
        vox_cvar
        fmt::fmt
)

vox_configure_target(vox_console)

# Embed the vanilla-JS web frontend into the binary (proves the embed -> link
# path). Symbols: web_<name>_data[] / web_<name>_size, referenced in
# ConsoleServer.cpp.
vox_embed_resource(vox_console SYMBOL web_index_html FILE engine/web/index.html)
vox_embed_resource(vox_console SYMBOL web_console_js  FILE engine/web/console.js)
vox_embed_resource(vox_console SYMBOL web_console_css FILE engine/web/console.css)

# Network + crypto deps activate only with the vcpkg manifest features
# 'console' / 'tls'. OFF in the skeleton pass.
if(VOX_ENABLE_CONSOLE_SERVER)
    find_package(civetweb CONFIG REQUIRED)
    find_package(unofficial-sodium CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    target_link_libraries(vox_console PRIVATE civetweb::civetweb unofficial-sodium::sodium nlohmann_json::nlohmann_json ws2_32)
    target_compile_definitions(vox_console PRIVATE VOX_HAVE_CONSOLE_SERVER=1)
    if(VOX_ENABLE_TLS)
        find_package(OpenSSL REQUIRED)
        target_link_libraries(vox_console PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(vox_console PRIVATE VOX_HAVE_TLS=1)
    endif()
endif()
