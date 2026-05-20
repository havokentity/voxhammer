// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "ecs/EcsWorld.h"

#include "platform/Log.h"

namespace vox::ecs {

void EcsWorld::Init() {
    // TODO(ecs): construct entt::registry, register the gameplay component
    // archetypes. Stub for the skeleton pass.
    vox::log::Trace("ecs: world stub init");
}

void EcsWorld::Shutdown() {}

}  // namespace vox::ecs
