// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

namespace vox::ecs {

// Gameplay-entity registry (EnTT). Holds debris, projectiles, vehicles, tools,
// lights, props, audio emitters -- and references into the mass-data
// subsystems (voxel chunks, GPU particles, fluid grids, AS), never the mass
// data itself. EnTT wiring activates with VOX_ENABLE_ECS (vcpkg feature 'ecs').
class EcsWorld {
public:
    void Init();
    void Shutdown();
    unsigned EntityCount() const { return 0; }  // wired with EnTT later
};

}  // namespace vox::ecs
