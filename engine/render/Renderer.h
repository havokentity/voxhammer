// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

#include <cstdint>
#include <memory>
#include <string>
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
    // Runtime-tunable sample counts (mapped to HLSL cbuffer; changing any resets GI accumulation)
    int   dither          = 1;       // 1 = triangular dither before 8-bit output; 0 = off
    int   ao_samples      = 8;       // AO hemisphere rays (1..32)
    int   shadow_samples  = 12;      // soft-shadow cone rays (1..16)
    int   gi_samples      = 1;       // QUALITY indirect samples (paths) averaged per frame (1..8)
    int   gi_bounces      = 1;       // QUALITY path depth: bounces light walks per path (1..5; 1 = single bounce, the clean default)
    float gi_emissive     = 1.0f;    // global emissive multiplier (all emitters; keep ~1 = sane)
    float vox_emissive    = 1.0f;    // extra emissive boost for LOADED .vox maps only (host sets; 1 = demo/none)
    float emissive_surface = 1.0f;   // scales the emitter's OWN surface glow (1 = full; lower so strong emitters don't blow white). Not its GI contribution.
    float gi_intensity     = 1.0f;   // QUALITY indirect-GI strength (sky fill + inter-reflection); >1 brightens dim enclosed rooms
    int   gi_reproject_history = 128; // QUALITY+reproject: ceiling on accumulated GI samples DURING camera motion (the swim<->grain knob). Higher = smoother but more "swimming"/lag; lower = crisper/more responsive but grainier. Still frames always converge to the full cap.
    // ---- Multi-pass GI DENOISER (QUALITY/lighting_mode==1 only) -------------
    // 0 = NONE: the original SINGLE-PASS path (default, unchanged). 1 = ATROUS:
    // demodulated edge-aware a-trous wavelet spatial denoise. 2 = SVGF: same
    // a-trous filter but variance-driven (per-pixel luminance variance scales the
    // luminance edge-stop, plus a 3x3 variance pre-blur on the first iteration).
    // Modes 1 & 2 run the multi-pass pipeline (offscreen SHADE -> a-trous -> composite)
    // so temporal history can stay SHORT while the spatial filter removes per-frame
    // grain. Only consulted in QUALITY mode; in PERFORMANCE mode (no GI) the value is
    // ignored and the single pass runs. Switchable at runtime frame-to-frame.
    int   gi_denoiser          = 0;      // 0 NONE(single pass) / 1 ATROUS / 2 SVGF
    int   gi_atrous_iters      = 5;      // a-trous wavelet iterations (clamped 1..6); step doubles each (1,2,4,8,16,32)
    float gi_denoise_phi_normal= 64.0f;  // normal edge-stop exponent: higher = sharper preservation of normal discontinuities
    float gi_denoise_phi_depth = 1.0f;   // depth/position edge-stop scale: smaller = more sensitive to depth discontinuities
    float gi_denoise_phi_lum   = 4.0f;   // luminance edge-stop sigma (mode 1 fixed; mode 2 scaled by sqrt(variance))
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
    // palette256: optional pointer to 256 RGBA8 uints (VoxPalette::data()) from the imported .vox.
    //             nullptr => built-in default palette reproducing the procedural demo scene colors.
    bool Init(void* hwnd, int width, int height, const std::vector<std::uint32_t>* voxels = nullptr,
              const std::uint32_t* palette256 = nullptr);
    void Shutdown();
    bool Valid() const { return valid_; }

    void RenderFrame(const FrameParams& fp);

    // QUALITY-mode GI temporal denoise toggle (default ON). When on, GI history is
    // preserved across camera motion via a motion-adaptive temporal exponential
    // moving average instead of being discarded every frame, so path-traced GI
    // stays usable while the camera moves (at the cost of mild ghosting/lag).
    // Still-frame convergence and PERFORMANCE mode are unaffected. Off reproduces
    // the original hard-reset-on-view-change behaviour for A/B comparison.
    void SetGiDenoise(bool enabled);

    // Empty-space-skipping toggle (default ON). The voxel raymarcher consults a
    // coarse per-brick occupancy grid and jumps over empty bricks instead of
    // stepping one fine voxel at a time, so crossing large air gaps is nearly
    // free (the win grows with world size). The acceleration is CONSERVATIVE — a
    // brick is occupied if any voxel inside it is solid — so the rendered image
    // is identical with it on or off. Off forces the original per-voxel DDA for
    // A/B and correctness comparison.
    void SetEmptySpaceSkip(bool enabled);

    // GI-only debug view (default OFF). When ON in QUALITY mode, the renderer
    // outputs ONLY the accumulated indirect-bounce radiance (no direct light, no
    // albedo) so it is unmistakable that path-traced GI is contributing. Has no
    // effect in PERFORMANCE mode (no GI is computed there).
    void SetGiDebug(bool enabled);

    // QUALITY-mode GI temporal REPROJECTION toggle (default ON). When on, each
    // pixel's accumulated GI history is realigned across camera motion: the current
    // primary-hit world point is projected through the previous frame's
    // view-projection to find where it appeared last frame, and that pixel's history
    // is reused only when it is the same world point (no disocclusion). This lets a
    // long, clean history be trusted while the camera moves, so motion is far
    // sharper with little ghosting. Off falls back to the screen-space temporal EMA
    // (SetGiDenoise) that blends the SAME screen pixel regardless of motion, for A/B
    // comparison. No effect in PERFORMANCE mode. Independent of SetGiDenoise (which
    // governs the off path's motion-adaptive history cap).
    void SetGiReproject(bool enabled);

    // Hot-update the voxel grid and palette without restarting.
    // grid must contain exactly kGrid^3 (64^3 = 262144) uint32 entries.
    // palette256 must point to 256 RGBA8 uints (VoxPalette::data()).
    // Call from the main thread (WaitIdle is used internally to sync the GPU).
    void SetVoxels(const std::vector<std::uint32_t>& grid, const std::uint32_t* palette256);

    // Incremental edit: update only the voxels in the box [x0,x1) x [y0,y1) x
    // [z0,z1) (and the brick-occupancy cells they touch) from |region|, a flat
    // (x1-x0)(y1-y0)(z1-z0) array indexed local-z*dy*dx + local-y*dx + local-x.
    // No GPU stall -- writes the persistently-mapped buffer directly (uint32
    // stores are atomic, so at most a 1-frame old/new mix on the edited cells,
    // invisible for voxel edits). Use this for live carve/paint so the frame
    // never freezes; SetVoxels remains for full-grid swaps (load/clear).
    // palette256 may be null to leave the palette unchanged. Main thread.
    void EditVoxels(int x0, int y0, int z0, int x1, int y1, int z1,
                    const std::uint32_t* region, const std::uint32_t* palette256);

    // Capture the current backbuffer and save it to |path|.
    // png=true -> PNG (stb_image_write); png=false -> BMP.
    // Returns false if DX12 is not initialised or the write fails.
    bool CaptureScreenshot(const std::string& path, bool png);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool valid_ = false;
};

}  // namespace vox::render
