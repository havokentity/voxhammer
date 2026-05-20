// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

namespace vox::audio {

// FMOD Studio + Steam Audio: geometric occlusion/reverb against the voxel
// world, HRTF, distance attenuation, air absorption, ambisonics, WASAPI
// exclusive output. Stub this pass (M6).
class AudioEngine {
public:
    void Init();
    void Shutdown();
    void Update();
};

}  // namespace vox::audio
