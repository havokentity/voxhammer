# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_executable(voximport main.cpp)
target_link_libraries(voximport PRIVATE fmt::fmt)
vox_configure_target(voximport)
