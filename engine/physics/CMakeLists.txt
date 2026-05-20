# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(vox_physics STATIC
    PhysicsWorld.cpp
    PhysicsWorld.h
)

target_link_libraries(vox_physics PUBLIC vox_includes fmt::fmt)
vox_configure_target(vox_physics)

# TODO(M3): link NVIDIA PhysX 5 from thirdparty/physx when present.
