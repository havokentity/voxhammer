// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace vox::render {

// Per-frame inputs the renderer needs from the engine (filled from cvars in
// main): the clear/sky color and a free-fly camera.
struct FrameParams {
    float clear[3]        = {0.05f, 0.05f, 0.08f};
    float cam_pos[3]      = {32.0f, 40.0f, -24.0f};
    float cam_yaw         = 0.0f;    // radians, around +Y
    float cam_pitch       = -0.5f;   // radians
    float cam_fov         = 1.2f;    // radians (vertical)
    float time_sec        = 0.0f;
    float exposure        = 1.0f;
    int   hdr             = 0;       // 1 = ACES tonemap (stand-in for HDR10 output)
    float sun[3]          = {0.55f, 0.62f, 0.56f};  // normalized sun direction
    float ambient         = 0.28f;                  // ambient/fill light fraction
    // Lighting quality controls
    float shadow_softness = 0.08f;   // half-angle (rad) for penumbra jitter; 0 = hard shadow
    float ao_strength     = 0.55f;   // 0..1, how dark fully-occluded ambient gets
    float ao_radius       = 4.0f;    // world-space voxel radius for AO rays
    int   lighting_mode   = 0;       // 0 = PERFORMANCE (raymarch + AO), 1 = QUALITY (path-traced GI)
};

// DX12 presenter. M0+ slice: device + flip-discard swapchain + a full-screen
// fragment pass that DDA-raymarches a procedural voxel volume (StructuredBuffer
// SRV) with a free-fly camera. This is the foundation the DXR brickmap path
// tracer (ReSTIR, DLSS/FSR) replaces in M1/M4. DX12 types are PIMPL'd out.
// When the window/DX12 feature is off this is a no-op stub.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // voxels: optional flat kGrid^3 uint32 material grid (from VoxelWorld::BakeFlatGrid)
    // that overrides the built-in procedural scene. nullptr => procedural demo scene.
    bool Init(void* hwnd, int width, int height, const std::vector<std::uint32_t>* voxels = nullptr);
    void Shutdown();
    bool Valid() const { return valid_; }

    void RenderFrame(const FrameParams& fp);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool valid_ = false;
};

}  // namespace vox::render
