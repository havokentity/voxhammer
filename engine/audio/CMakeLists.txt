# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(vox_audio STATIC
    AudioEngine.cpp
    AudioEngine.h
)

target_link_libraries(vox_audio PUBLIC vox_includes fmt::fmt)
vox_configure_target(vox_audio)

# TODO(M6): link FMOD Studio API + Steam Audio FMOD plugin from thirdparty/.
