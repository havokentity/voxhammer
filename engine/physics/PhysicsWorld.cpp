// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "physics/PhysicsWorld.h"

#include "platform/Log.h"

namespace vox::physics {

void PhysicsWorld::Init() { vox::log::Trace("physics: PhysX stub init"); }
void PhysicsWorld::Shutdown() {}
void PhysicsWorld::Step(float dt) { (void)dt; }

}  // namespace vox::physics
