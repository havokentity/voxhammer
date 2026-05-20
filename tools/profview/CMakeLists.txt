# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_executable(profview main.cpp)
target_link_libraries(profview PRIVATE fmt::fmt)
vox_configure_target(profview)
