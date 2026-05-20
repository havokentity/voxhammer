// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

#include <memory>

namespace vox::render {

// Minimal DX12 presenter for the M0 window bring-up: device + flip-discard
// swapchain + per-frame clear to a color, then present. The full voxel path
// tracer (DXR brickmap AS, ReSTIR, DLSS/FSR) lands in M1/M4. DX12 types are
// PIMPL'd out of this header. When the window/DX12 feature is off, this is a
// no-op stub (Init returns false) and the engine runs headless.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Init(void* hwnd, int width, int height);
    void Shutdown();
    bool Valid() const { return valid_; }

    // Clear the backbuffer to (r,g,b) in 0..1 and present.
    void RenderFrame(float r, float g, float b);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool valid_ = false;
};

}  // namespace vox::render
