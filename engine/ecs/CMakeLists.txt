# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(vox_ecs STATIC
    EcsWorld.cpp
    EcsWorld.h
    components.h
)

target_link_libraries(vox_ecs PUBLIC vox_includes fmt::fmt)
vox_configure_target(vox_ecs)

if(VOX_ENABLE_ECS)
    find_package(EnTT CONFIG REQUIRED)
    target_link_libraries(vox_ecs PUBLIC EnTT::EnTT)
    target_compile_definitions(vox_ecs PUBLIC VOX_ENABLE_ECS=1)
endif()
