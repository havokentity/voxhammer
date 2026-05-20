# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_executable(assetcook main.cpp)
target_link_libraries(assetcook PRIVATE fmt::fmt)
vox_configure_target(assetcook)
