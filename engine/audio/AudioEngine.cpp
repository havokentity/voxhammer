// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "audio/AudioEngine.h"

#include "platform/Log.h"

namespace vox::audio {

void AudioEngine::Init() { vox::log::Trace("audio: FMOD + Steam Audio stub init"); }
void AudioEngine::Shutdown() {}
void AudioEngine::Update() {}

}  // namespace vox::audio
