# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_executable(voxhammer
    main.cpp
)

target_link_libraries(voxhammer PRIVATE vox_engine)
vox_configure_target(voxhammer)

# Console subsystem app (no WIN32). The DX12 window (with
# WS_EX_NOREDIRECTIONBITMAP) lands in the graphics bring-up pass.
