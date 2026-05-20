// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

namespace vox::physics {

// NVIDIA PhysX 5 wrapper: rigid bodies, articulations, vehicles, FEM soft
// bodies, cloth, fluids. GPU rigid bodies on NVIDIA (10K+ islands), CPU path
// elsewhere. Dispatcher wired into the enkiTS job system. Stub this pass (M3).
class PhysicsWorld {
public:
    void Init();
    void Shutdown();
    void Step(float dt);
    unsigned ActiveIslands() const { return 0; }
};

}  // namespace vox::physics
