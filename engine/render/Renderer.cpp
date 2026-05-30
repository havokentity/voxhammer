// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "render/Renderer.h"

#include "platform/Log.h"
#include "platform/Platform.h"
#include "voxel/VoxelWorld.h"

#if defined(VOX_HAVE_DX12)

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

using Microsoft::WRL::ComPtr;

namespace vox::render {
namespace {

constexpr UINT kGrid = vox::voxel::kWorldDim;  // 128^3 voxel volume

// Empty-space-skipping acceleration structure: the world is divided into
// kBrickDim^3 bricks. kBrickGrid = ceil(kGrid/kBrickDim) bricks per axis
// (16^3 for a 128^3 world). One uint per brick marks "any solid voxel inside".
constexpr UINT kBrickDim  = 8;
constexpr UINT kBrickGrid = (kGrid + kBrickDim - 1) / kBrickDim;
constexpr UINT kBrickCount = kBrickGrid * kBrickGrid * kBrickGrid;

// Full-screen-triangle VS + DDA voxel-raymarch PS. Compiled at runtime with
// D3DCompile (SM 5.1 DXBC -> accepted by DX12), so no DXC dependency.
const char* kShader = R"HLSL(
cbuffer Camera : register(b0) {
    float3 camPos;         float fov;
    float3 camFwd;         float timeSec;
    float3 camRight;       float aspect;
    float3 camUp;          int   gridDim;
    float3 clearCol;       float exposure;
    float2 viewport;       int   hdr;          float shadowSoftness;
    float3 sunDir;         float ambient;
    float  aoStrength;     float aoRadius;     int lightingMode; int accumFrame; // row 7
    int    dither;         int aoSamples;      int shadowSamples; int giSamples;  // row 8
    int    giDenoise;      float giHistMax;    int emptySkip;     int brickDim;   // row 9
    int    giDebug;        int   giBounces;    float giEmissive;  float voxEmissive; // row 10
    float  emissiveSurface; float giIntensity;  int reproject;     int ePad11c;    // row 11
    row_major float4x4 prevVP;                                                      // rows 12-15: previous-frame view-projection
};
StructuredBuffer<uint> Voxels : register(t0);
// QUALITY GI temporal buffers (PING-PONG, swapped each frame in RenderFrame):
//   Accum/HitPos        = CURRENT frame: written this frame (u0/u1).
//   PrevAccum/PrevHitPos= PREVIOUS frame: read for temporal reprojection (u2/u3).
// Accum.xyz = running-average indirect+direct radiance, Accum.w = sample count.
// HitPos.xyz = primary-hit WORLD position seen by this pixel, HitPos.w = valid flag
// (1 = solid hit, 0 = sky/miss). Reprojection maps the current hit through prevVP
// into the previous frame's screen pixel and reuses that pixel's history when the
// previous primary hit is the same world point (no disocclusion).
RWStructuredBuffer<float4> Accum       : register(u0);  // CURRENT GI accumulation: rgb + sample count
RWStructuredBuffer<float4> HitPos      : register(u1);  // CURRENT primary-hit world pos + valid flag
RWStructuredBuffer<float4> PrevAccum   : register(u2);  // PREVIOUS-frame GI accumulation (read-only this frame)
RWStructuredBuffer<float4> PrevHitPos  : register(u3);  // PREVIOUS-frame primary-hit world pos (read-only this frame)

// Coarse occupancy acceleration structure (empty-space skipping). One uint per
// BRICK_DIM^3 brick: nonzero iff ANY voxel in that brick is solid. The brick
// grid is ceil(gridDim/brickDim) per axis. Built on the CPU in Init/SetVoxels
// so it always matches Voxels. A brick is CONSERVATIVELY occupied (set if any
// solid voxel), so skipping an empty brick can never miss a hit.
StructuredBuffer<uint> BrickOccupancy : register(t3);

// Number of bricks per axis for the current grid (ceil division).
int brickGridDim() { return (gridDim + brickDim - 1) / brickDim; }

// True if the brick containing integer voxel cell |c| is entirely empty.
// |c| is assumed in-bounds (callers guard the grid bounds).
bool brickIsEmpty(int3 c) {
    int  bd = brickDim;
    int3 b  = c / bd;                      // brick coordinate (floor; c>=0 in-bounds)
    int  B  = brickGridDim();
    return BrickOccupancy[(uint)((b.z * B + b.y) * B + b.x)] == 0u;
}

// Coarse-DDA skip: given the current fine-DDA voxel |cell| (whose brick is
// empty), the ray (ro,rd,stepv,s=sign), advance to the entry of the NEXT brick
// along the ray. Returns the t at which the next brick is entered and writes the
// face normal of the crossed brick boundary into |nrm|. The caller rederives the
// fine cell/tMax from ro+rd*tHit, so there is no coarse/fine state to drift.
float brickSkipT(float3 ro, float3 rd, int3 cell, float3 s, out float3 nrm) {
    int   bd = brickDim;
    // Min corner (in voxel space) of the brick containing cell.
    float3 bmin = floor(float3(cell) / float(bd)) * float(bd);
    // Exit plane per axis: far face if stepping +, near face if stepping -.
    float3 plane = bmin + (s > 0.0 ? float(bd) : 0.0);
    float3 tNext = (plane - ro) / rd;      // rd already de-zeroed by caller
    // Nearest brick face along the ray.
    float tHit;
    if (tNext.x <= tNext.y && tNext.x <= tNext.z)      { tHit = tNext.x; nrm = float3(-s.x, 0, 0); }
    else if (tNext.y <= tNext.z)                       { tHit = tNext.y; nrm = float3(0, -s.y, 0); }
    else                                               { tHit = tNext.z; nrm = float3(0, 0, -s.z); }
    return tHit;
}

struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    float2 t = float2((vid << 1) & 2, vid & 2);   // (0,0)(2,0)(0,2)
    o.pos = float4(t * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}

// Narkowicz ACES filmic tonemap.
float3 tonemapACES(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Improved sky: horizon-to-zenith gradient + soft sun disk/glow, clearCol as tint.
float3 sky(float3 rd) {
    float k = saturate(rd.y);
    // horizon tint is warm/bright, zenith is deep blue-ish
    float3 horiz  = clearCol * 1.60;
    float3 zenith = clearCol * float3(0.40, 0.52, 0.80);
    float3 base   = lerp(horiz, zenith, k * k);

    // soft sun glow: broad halo + tight disk
    float cosTheta = saturate(dot(rd, sunDir));
    float halo     = pow(cosTheta, 8.0)  * 0.25;
    float disk     = pow(cosTheta, 256.0) * 3.0;
    float3 sunCol  = float3(1.10, 0.95, 0.75);
    base += sunCol * (halo + disk) * saturate(sunDir.y + 0.15);  // fade when sun below horizon

    return base;
}

StructuredBuffer<uint> Palette : register(t2);

float3 palette(uint m) {
    uint p = Palette[m & 255u];
    float3 c = float3(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u) / 255.0;
    return pow(c, 2.2);
}

// Emissive strength = (alpha/255) * 8 (the 8 is just the alpha-byte's HDR
// encoding range, like RGB bytes encode 0..1) * giEmissive (sane global cvar,
// default 1) * voxEmissive (LIVE boost the host sets only when a .vox map is
// loaded, else 1 -- so dim MagicaVoxel _emit materials can be cranked WAY up to
// flood a room while the demo/code emissive stays sane; uncapped, no alpha clip).
float emission(uint m) {
    uint p = Palette[m & 255u];
    return (float((p >> 24) & 255u) / 255.0) * 8.0 * giEmissive * voxEmissive;
}

// Blue-noise lookup: spatially uniform, spectrally high-frequency.
// px   = integer pixel coordinate (wraps mod 64 in each axis).
// k    = sample index; offsets by (k*13, k*7) so successive samples within
//        the same pixel draw decorrelated values from the tiled 64x64 texture.
StructuredBuffer<float> BlueNoise : register(t1);
float blue(int2 px, int k) {
    return BlueNoise[((px.x + k * 13) & 63) + (((px.y + k * 7) & 63) << 6)];
}

// Vogel-disk sample k of n on the unit disk, spun by phi (a golden-angle
// spiral). Spreads the handful of penumbra/AO taps evenly with no clumping.
float2 vogelDisk(int k, int n, float phi) {
    const float GOLDEN_ANGLE = 2.39996323;
    float r = sqrt((float(k) + 0.5) / float(n));
    float theta = float(k) * GOLDEN_ANGLE + phi;
    return float2(r * cos(theta), r * sin(theta));
}

// Build an orthonormal basis given a normal n (Frisvad / Duff et al.).
void buildBasis(float3 n, out float3 t, out float3 b) {
    if (abs(n.z) < 0.9) {
        t = normalize(cross(n, float3(0,0,1)));
    } else {
        t = normalize(cross(n, float3(0,1,0)));
    }
    b = cross(n, t);
}

// DDA ray: returns true if a solid voxel is hit within maxSteps.
bool occludedRay(float3 p, float3 d, int maxSteps) {
    if (abs(d.x) < 1e-5) d.x = 1e-5;
    if (abs(d.y) < 1e-5) d.y = 1e-5;
    if (abs(d.z) < 1e-5) d.z = 1e-5;
    float3 cell = floor(p);
    float3 s = sign(d);
    float3 tD = abs(1.0 / d);
    float3 tM = ((cell + (s * 0.5 + 0.5)) - p) / d;
    bool skip = (emptySkip != 0);
    [loop] for (int i = 0; i < maxSteps; ++i) {
        if (tM.x < tM.y && tM.x < tM.z) { cell.x += s.x; tM.x += tD.x; }
        else if (tM.y < tM.z)           { cell.y += s.y; tM.y += tD.y; }
        else                            { cell.z += s.z; tM.z += tD.z; }
        if (cell.x < 0 || cell.y < 0 || cell.z < 0 ||
            cell.x >= gridDim || cell.y >= gridDim || cell.z >= gridDim) return false;
        if (Voxels[(uint)(cell.z * gridDim * gridDim + cell.y * gridDim + cell.x)] != 0) return true;
        // Empty-space skip: empty brick -> jump to the next brick boundary. This
        // loop steps before testing, so after relocating we must TEST the first
        // voxel of the new brick here (the next iteration steps past it).
        [branch] if (skip && brickIsEmpty(int3(cell))) {
            float3 sn;
            float tBrick = brickSkipT(p, d, int3(cell), s, sn);
            float tNp = tBrick + 0.0005;
            float3 np = p + d * tNp;
            float3 nc = floor(np);
            [branch] if (any(nc != cell)) {
                cell = nc;
                tM   = ((cell + (s * 0.5 + 0.5)) - np) / d + tNp;
                if (cell.x < 0 || cell.y < 0 || cell.z < 0 ||
                    cell.x >= gridDim || cell.y >= gridDim || cell.z >= gridDim) return false;
                if (Voxels[(uint)(cell.z * gridDim * gridDim + cell.y * gridDim + cell.x)] != 0) return true;
            }
        }
    }
    return false;
}
)HLSL"
// Adjacent raw-string literal #2 (C++ concatenates all chunks at compile time).
// MSVC caps a single string-literal token at 16380 bytes; adding the temporal-
// reprojection cbuffer rows + ping-pong UAV declarations pushed the first chunk
// over, so the shader is now split into THREE adjacent literals to stay under it.
R"HLSL(
// Soft shadow: average N jittered shadow rays within a cone of half-angle shadowSoftness.
// Uses a deterministic screen-space seed so no temporal flicker.
//
// Anti-banding strategy: two decorrelated IGN values per pixel.
//   rot   — rotates the entire Vogel disk (breaks tile-repeat structure).
//   idxOff — fractionally offsets the tap index before feeding vogelDisk, so
//             neighbouring pixels sample different positions on the spiral rather
//             than the same N discrete levels.  This converts the 0/1-averaged
//             staircase into fine dithered grain that reads as a smooth gradient.
//   radJitter — per-tap radius perturbation derived from idxOff, breaking the
//               uniform ring spacing of the pure Vogel spiral.
float softShadow(float3 hp, float3 nrm, float3 sunD, float2 screenPos) {
    [branch] if (shadowSoftness < 1e-4) {
        // Hard shadow fast path.
        return occludedRay(hp + nrm * 0.02, sunD, 256) ? 0.0 : 1.0;
    }

    // Build a tangent frame around sunDir to jitter within the cone.
    float3 st, sb;
    buildBasis(sunD, st, sb);

    // Blue-noise rotation seeds — two decorrelated samples (k=0 and k=1).
    // These replace the old IGN calls; blue noise gives the same well-spread
    // rotation behaviour but without IGN's visible diagonal grid in still frames.
    int2 isp = int2(screenPos);
    float rot     = blue(isp, 0) * 6.2832;
    float idxOff  = blue(isp, 1);   // [0,1)

    float tanS = tan(shadowSoftness);
    float lit  = 0.0;
    int N = clamp(shadowSamples, 1, 16);
    [loop] for (int s = 0; s < N; ++s) {
        // Shift the effective tap index by a per-pixel fraction so neighbours
        // draw from a different part of the Vogel spiral — dithers the discrete
        // penumbra levels into smooth grain.
        float effIdx = frac((float(s) + idxOff) / float(N)) * float(N);

        // Slight radius jitter: perturb sqrt factor by ±idxOff/N so the
        // concentric ring spacing varies pixel-to-pixel, breaking moiré.
        float radMul = 1.0 + (idxOff - 0.5) * (0.5 / float(N));

        float2 d   = vogelDisk((int)effIdx, N, rot) * (tanS * radMul);
        float3 dir = normalize(sunD + st * d.x + sb * d.y);
        lit += occludedRay(hp + nrm * 0.02, dir, 256) ? 0.0 : 1.0;
    }
    return lit / float(N);
}

// Ambient occlusion: sample hemisphere oriented around nrm with distance-weighted falloff.
// Nearer occluders contribute more darkness; far ones are attenuated smoothly.
float ambientOcclusion(float3 hp, float3 nrm, float2 screenPos) {
    [branch] if (aoStrength < 1e-4) return 1.0;

    float3 t, b;
    buildBasis(nrm, t, b);

    float rot   = blue(int2(screenPos), 2) * 6.2832;  // k=2 decorrelated from shadow (k=0,1)
    int   steps = (int)clamp(aoRadius, 2.0, 24.0);
    const float GOLDEN_ANGLE = 2.39996323;
    float occ   = 0.0;
    float wSum  = 0.0;
    int M = clamp(aoSamples, 1, 32);
    [loop] for (int r = 0; r < M; ++r) {
        // Low-discrepancy cosine-weighted hemisphere sample, rotated per pixel:
        // radial strata -> elevation, golden-angle azimuth -> even spread.
        float u    = (float(r) + 0.5) / float(M);
        float phi  = float(r) * GOLDEN_ANGLE + rot;
        float cosT = sqrt(1.0 - u);           // cosine-weighted toward the normal
        float sinT = sqrt(u);
        float3 dir = normalize(nrm * cosT + (t * cos(phi) + b * sin(phi)) * sinT);

        // Distance-weighted DDA: trace and record hit distance for smooth falloff.
        // Nearer occluders (small hitDist) get higher weight -> removes blocky AO edge.
        float3 pp  = hp + nrm * 0.02;
        float3 ddir = dir;
        if (abs(ddir.x) < 1e-5) ddir.x = 1e-5;
        if (abs(ddir.y) < 1e-5) ddir.y = 1e-5;
        if (abs(ddir.z) < 1e-5) ddir.z = 1e-5;
        float3 cell  = floor(pp);
        float3 sv    = sign(ddir);
        float3 tD    = abs(1.0 / ddir);
        float3 tM2   = ((cell + (sv * 0.5 + 0.5)) - pp) / ddir;
        float  hitDist = aoRadius;  // default = miss (far distance)
        bool   gotHit  = false;
        [loop] for (int i2 = 0; i2 < steps; ++i2) {
            float curT;
            if (tM2.x < tM2.y && tM2.x < tM2.z) { curT = tM2.x; cell.x += sv.x; tM2.x += tD.x; }
            else if (tM2.y < tM2.z)              { curT = tM2.y; cell.y += sv.y; tM2.y += tD.y; }
            else                                 { curT = tM2.z; cell.z += sv.z; tM2.z += tD.z; }
            if (cell.x < 0 || cell.y < 0 || cell.z < 0 ||
                cell.x >= gridDim || cell.y >= gridDim || cell.z >= gridDim) break;
            if (Voxels[(uint)(cell.z * gridDim * gridDim + cell.y * gridDim + cell.x)] != 0) {
                hitDist = curT;
                gotHit  = true;
                break;
            }
        }
        // Distance falloff: weight = 1/(1 + d^2*k) so close occluders darken strongly.
        float w = 1.0 / (1.0 + hitDist * hitDist * 0.25);
        occ   += gotHit ? w : 0.0;
        wSum  += w;
    }
    float normOcc  = (wSum > 1e-5) ? (occ / wSum) : 0.0;
    float visibility = 1.0 - normOcc * aoStrength;
    return saturate(visibility);
}

// ---- Path-traced GI (QUALITY tier) ---------------------------------------

// Unified voxel DDA: trace from ro along rd; on hit fill hp/nrm/mat -> true.
// Shared by the primary ray and the GI bounce ray.
bool traceVoxel(float3 ro, float3 rd, int maxSteps, out float3 hp, out float3 nrm, out uint mat) {
    nrm = float3(0, 1, 0); mat = 0; hp = ro;
    if (abs(rd.x) < 1e-5) rd.x = 1e-5;
    if (abs(rd.y) < 1e-5) rd.y = 1e-5;
    if (abs(rd.z) < 1e-5) rd.z = 1e-5;
    float3 bmin = float3(0,0,0), bmax = float3(gridDim, gridDim, gridDim);
    float3 t0 = (bmin - ro) / rd, t1 = (bmax - ro) / rd;
    float3 ts = min(t0, t1), tb = max(t0, t1);
    float tBox  = max(max(ts.x, ts.y), ts.z);
    float tExit = min(min(tb.x, tb.y), tb.z);
    if (tExit < max(tBox, 0.0)) return false;
    float3 p = ro + rd * (max(tBox, 0.0) + 0.001);
    float3 cell = floor(p);
    float3 stepv = sign(rd);
    // Seed nrm with the box ENTRY face (the axis that produced tBox). If the FIRST
    // sampled cell is solid -- geometry sitting AT the world boundary, viewed from
    // outside (tBox>0) -- the loop below returns on iteration 0 BEFORE any DDA step
    // assigns a normal, so without this the hit keeps the bogus default-up nrm. That
    // wrong normal points the AO/GI ray offset (hp + nrm*eps) ALONG the wall instead
    // of off it -> the secondary rays self-occlude -> the whole boundary face goes BLACK.
    // Pick the ENTRY axis = whichever per-axis entry distance equals tBox. (Must
    // compare ts.x/ts.y/ts.z to EACH OTHER -- comparing tBox, which IS the max, to
    // ts.y/ts.z is always true and would wrongly pick X for every direction.)
    [branch] if (tBox > 0.0) {
        if      (ts.x >= ts.y && ts.x >= ts.z) nrm = float3(-stepv.x, 0, 0);  // entered through an X face
        else if (ts.y >= ts.z)                 nrm = float3(0, -stepv.y, 0);  // entered through a Y face
        else                                   nrm = float3(0, 0, -stepv.z);  // entered through a Z face
    }
    float3 tDelta = abs(1.0 / rd);
    float3 tMax = ((cell + (stepv * 0.5 + 0.5)) - p) / rd;
    float tEnter = 0.0;
    bool skip = (emptySkip != 0);
    [loop] for (int k = 0; k < maxSteps; ++k) {
        if (cell.x < 0 || cell.y < 0 || cell.z < 0 ||
            cell.x >= gridDim || cell.y >= gridDim || cell.z >= gridDim) return false;
        uint v = Voxels[(uint)(cell.z * gridDim * gridDim + cell.y * gridDim + cell.x)];
        if (v != 0) { mat = v; hp = p + rd * tEnter; return true; }
        // Empty-space skip: if this whole brick is empty, jump to the next brick
        // boundary (coarse DDA step) instead of a single fine voxel. Conservative:
        // an empty brick has no solid voxel, so we cannot skip a hit.
        [branch] if (skip && brickIsEmpty(int3(cell))) {
            float3 sn;
            float tBrick = brickSkipT(p, rd, int3(cell), stepv, sn);
            float tNp = tBrick + 0.0005;                // param (from p) just inside next brick
            float3 np = p + rd * tNp;                   // nudge just inside next brick
            float3 nc = floor(np);
            // Guard: if the nudge failed to advance the cell (ray ~parallel to a
            // face), fall through to the normal single fine step below.
            [branch] if (any(nc != cell)) {
                tEnter = tBrick;
                cell   = nc;
                nrm    = sn;
                tMax   = ((cell + (stepv * 0.5 + 0.5)) - np) / rd + tNp;
                continue;
            }
        }
        if (tMax.x < tMax.y && tMax.x < tMax.z) { tEnter = tMax.x; cell.x += stepv.x; tMax.x += tDelta.x; nrm = float3(-stepv.x, 0, 0); }
        else if (tMax.y < tMax.z)               { tEnter = tMax.y; cell.y += stepv.y; tMax.y += tDelta.y; nrm = float3(0, -stepv.y, 0); }
        else                                    { tEnter = tMax.z; cell.z += stepv.z; tMax.z += tDelta.z; nrm = float3(0, 0, -stepv.z); }
    }
    return false;
}
)HLSL"
// Adjacent raw-string literal #3 (C++ concatenates all chunks at compile time).
// MSVC caps a single string literal token at 16380 bytes, so the shader is split
// into three adjacent literals to keep each chunk comfortably under the limit.
R"HLSL(
// Per-frame-varying low-discrepancy 2D sample: an R2 sequence advanced by the
// accumulation frame, Cranley-Patterson rotated by a per-pixel blue-noise offset,
// so each accumulated frame draws a fresh well-spread sample that converges.
// saltK: base blue-noise sample index (even/odd pair used for the 2D offset).
float2 sampleXi(float2 screenPos, int frame, int saltK) {
    float2 r2 = frac(0.5 + float2(0.7548776662, 0.5698402909) * float(frame));
    int2 isp = int2(screenPos);
    float2 cp = float2(blue(isp, saltK), blue(isp, saltK + 1));
    return frac(r2 + cp);
}

// Direct sun radiance at a point (single hard shadow ray, no albedo factor).
float3 directSun(float3 hp, float3 nrm, float3 sd) {
    float ndl = saturate(dot(nrm, sd));
    if (ndl <= 0.0) return float3(0,0,0);
    float vis = occludedRay(hp + nrm * 0.02, sd, 256) ? 0.0 : 1.0;
    return float3(1.15, 1.06, 0.90) * (ndl * vis);
}

// Bright sky-dome irradiance used ONLY for GI indirect bounces. Decoupled from
// the background sky()/clearCol (kept dark by the user) so QUALITY GI gets real
// fill light instead of the near-black background -- otherwise shadowed/sky-
// facing surfaces (which PERFORMANCE fills with `ambient`) go black under GI.
// The `ambient` cvar is the fill-strength knob, same role as in PERFORMANCE.
float3 giSky(float3 rd) {
    float up = saturate(rd.y * 0.5 + 0.5);
    float3 dome = lerp(float3(0.85, 0.90, 1.00), float3(0.42, 0.60, 1.00), up);  // pale horizon -> blue zenith
    return dome * (ambient * 2.0);
}

// Core path-traced shading at the primary hit, returning the THREE terms the
// multi-pass denoiser needs SEPARATELY (the single-pass giRadiance below composes
// them exactly as before):
//   outAlb      = primary surface albedo (for demodulation / re-modulation)
//   outDirect   = direct sun (soft-shadowed, jittered) at the primary hit, NO albedo
//   returns       the DEMODULATED indirect irradiance (sky fill + inter-reflection),
//                 i.e. indirectAvg BEFORE the primary-albedo multiply, * giIntensity.
// Keeping the indirect demodulated lets the spatial filter blur it without bleeding
// across albedo/texture edges; albedo is re-applied at composite.
float3 giShadeSplit(float3 hp, float3 nrm, uint mat, float2 screenPos,
                    out float3 outAlb, out float3 outDirect) {
    float3 alb = palette(mat);
    outAlb = alb;

    // Direct sun, jittered inside the penumbra cone (accumulates -> soft shadow).
    // k=4,5 reserved for this pair (k=0..3 used by shadow/AO blue samples).
    float2 xs = sampleXi(screenPos, accumFrame, 4);
    float3 st, sb; buildBasis(sunDir, st, sb);
    float  ang = xs.y * 6.2832;
    float  rad = sqrt(xs.x) * tan(max(shadowSoftness, 0.0008));
    float3 sd  = normalize(sunDir + (st * cos(ang) + sb * sin(ang)) * rad);
    float3 direct = directSun(hp, nrm, sd);
    outDirect = direct;

    // Indirect: average giSamples hemisphere PATHS, each up to giBounces deep.
    // Light "walks" surface->surface (sun -> floor -> wall -> ...) so enclosed
    // rooms fill in, not just directly sun-lit faces. Lambertian + cosine-
    // weighted sampling => the per-bounce estimator weight is just the surface
    // albedo (the pi/pdf cancel). depth==1 reproduces the old single-bounce look;
    // depth>=2 is real multi-bounce GI. Temporal accumulation converges it.
    int    numSamples = clamp(giSamples, 1, 8);
    int    maxDepth   = clamp(giBounces, 0, 5);   // 0 = direct only (no indirect/sky fill); 1 = single bounce; 2-5 = multi
    float3 indirectSum = float3(0, 0, 0);
    int    kSalt = 6;
    [loop] for (int s = 0; s < numSamples; ++s) {
        float3 L    = float3(0, 0, 0);   // radiance gathered along this path
        float3 beta = float3(1, 1, 1);   // throughput (running albedo product)
        float3 ro   = hp + nrm * 0.02;
        float3 n    = nrm;
        [loop] for (int d = 0; d < maxDepth; ++d) {
            float3 t, b; buildBasis(n, t, b);
            float2 xb   = sampleXi(screenPos, accumFrame, kSalt); kSalt += 2;
            float  cosT = sqrt(1.0 - xb.x), sinT = sqrt(xb.x);
            float  phi  = xb.y * 6.2832;
            float3 bd   = normalize(n * cosT + (t * cos(phi) + b * sin(phi)) * sinT);
            float3 qhp, qn; uint qm;
            if (traceVoxel(ro, bd, 256, qhp, qn, qm)) {
                L    += beta * palette(qm) * directSun(qhp, qn, sunDir);  // sun light at this surface
                L    += beta * palette(qm) * emission(qm);               // emissive surface = light source
                beta *= palette(qm);                                     // carry albedo for the next bounce
                ro    = qhp + qn * 0.02;
                n     = qn;
            } else {
                L += beta * giSky(bd);                                    // escaped to the sky -> stop
                break;
            }
        }
        // Soft firefly suppression (replaces a hard min() clamp). A hard min() puts a
        // C0 KNEE in indirect radiance at a FIXED brightness (8); on a lit surface the
        // "L == 8" iso-contour then reads as a hard lighting SEAM -- and giIntensity,
        // which multiplies AFTER (below), scales that step up the more you boost. The
        // temporal accumulator faithfully converges TO the kinked value, so more frames
        // sharpen the seam instead of hiding it. This reciprocal luminance roll-off is
        // smooth everywhere (no knee -> no seam), leaves dim paths nearly untouched
        // (lum << kFire => factor ~1), and asymptotes a path's luminance to ~kFire so a
        // blown emissive bounce still lights the room (~kFire) without a spike OR a seam.
        const float kFire = 8.0;
        float plum = dot(L, float3(0.2126, 0.7152, 0.0722));   // path luminance (Rec.709)
        indirectSum += L * (1.0 / (1.0 + plum * (1.0 / kFire)));
    }
    float3 indirectAvg = (indirectSum / float(numSamples)) * giIntensity;   // artistic indirect-strength dial (>1 brightens dim rooms)
    return indirectAvg;   // DEMODULATED indirect (no primary albedo); composed by callers
}

// One-bounce diffuse path-traced radiance at the primary hit (SINGLE-PASS path).
// giSamples indirect bounces are averaged per frame; temporal accumulation converges
// the result. Composes giShadeSplit's three terms EXACTLY as the original did, so
// the gi_denoiser==0 path is bit-identical to before this refactor.
float3 giRadiance(float3 hp, float3 nrm, uint mat, float2 screenPos) {
    float3 alb, direct;
    float3 indirectAvg = giShadeSplit(hp, nrm, mat, screenPos, alb, direct);
    if (giDebug != 0) return indirectAvg;   // GI debug view: show ONLY the indirect bounce radiance
    // Self-emission (the surface's OWN glow when viewed) is scaled by
    // emissiveSurface so a strong room-light emitter need not blow to white;
    // the GI bounce contribution above stays at full emission (lights the room).
    return alb * (direct + indirectAvg) + alb * emission(mat) * emissiveSurface;
}

float4 PSMain(VSOut i) : SV_Target {
    float2 ndc = (i.pos.xy / viewport) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float tf = tan(fov * 0.5);
    float3 rd = normalize(camFwd + camRight * (ndc.x * tf * aspect) + camUp * (ndc.y * tf));
    float3 ro = camPos;

    float3 hp, nrm; uint mat;
    float3 col;
    bool primHit = traceVoxel(ro, rd, 768, hp, nrm, mat);   // primary-hit world pos in hp (for temporal reprojection)
    if (!primHit) {
        col = sky(rd);
    } else if (lightingMode == 1) {
        col = giRadiance(hp, nrm, mat, i.pos.xy);
    } else {
        // PERFORMANCE tier: direct soft shadow + short-range AO + flat ambient.
        float ndl = saturate(dot(nrm, sunDir));
        float shadow = softShadow(hp, nrm, sunDir, i.pos.xy);
        float ao = ambientOcclusion(hp, nrm, i.pos.xy);
        float3 alb = palette(mat);
        float3 ambientLight = float3(ambient, ambient, ambient) * ao;
        col = alb * (ambientLight + (1.0 - ambient) * ndl * shadow);
        col += alb * emission(mat) * emissiveSurface;   // self-glow (scaled so strong emitters don't blow white)
    }

    // QUALITY: progressive temporal accumulation. New-sample blend weight is
    // 1/(n+1), so the per-pixel sample cap (giHistMax) is the denoise knob:
    // still -> cap 2048 (converges clean); moving -> small cap => EMA that keeps
    // history (kills the 1-spp noise storm). accumFrame==0 forces a fresh start
    // on a genuine reset; giDenoise off keeps the cap at 2048 (original).
    //
    // TEMPORAL REPROJECTION (reproject != 0): instead of blending the SAME screen
    // pixel's history (which is a DIFFERENT world point once the camera moves ->
    // smearing), project this pixel's primary-hit world point P through the
    // PREVIOUS frame's view-projection (prevVP) to find where it was on screen
    // last frame, and reuse THAT pixel's history -- but only if it landed on
    // screen, the previous primary hit there was a valid solid hit, and it was the
    // SAME world point (no disocclusion). Aligned history can be trusted far
    // longer while moving (cap stays large), so motion is dramatically cleaner.
    // reproject == 0 reproduces the original screen-space EMA (history = same pixel
    // last frame) for A/B comparison. Ping-pong: read PrevAccum, write Accum.
    if (lightingMode == 1) {
        uint w = (uint)viewport.x;
        uint h = (uint)viewport.y;
        uint pix = (uint)i.pos.y * w + (uint)i.pos.x;

        // Locate the history source pixel and decide whether it is valid history.
        uint  srcPix    = pix;     // default: same screen pixel (screen-space EMA)
        bool  histValid = true;    // false => start a fresh average (n = 0)
        float cap       = (giDenoise != 0) ? max(giHistMax, 0.0) : 2048.0;

        [branch] if (reproject != 0) {
            // Reprojection only makes sense for a real surface hit; sky pixels have
            // no stable world point, so they always restart (and stay noise-free).
            [branch] if (primHit) {
                float4 clip = mul(prevVP, float4(hp, 1.0));   // P -> previous clip space
                // clip.w = depth along the previous camera forward; <=0 means the
                // point was behind the previous camera -> no valid history.
                [branch] if (clip.w > 1e-4) {
                    float2 pndc = clip.xy / clip.w;                 // previous-frame NDC (x right, y up)
                    float2 puv  = float2(pndc.x * 0.5 + 0.5, -pndc.y * 0.5 + 0.5);
                    [branch] if (all(puv >= 0.0) && all(puv < 1.0)) {
                        int px = (int)(puv.x * float(w));
                        int py = (int)(puv.y * float(h));
                        px = clamp(px, 0, (int)w - 1);
                        py = clamp(py, 0, (int)h - 1);
                        uint ppix = (uint)py * w + (uint)px;
                        float4 prevHP = PrevHitPos[ppix];
                        // Disocclusion test: the previous primary hit at that pixel
                        // must be a valid solid hit (prevHP.w > 0.5) AND the SAME
                        // world point as P. Tolerance scales with distance (clip.w)
                        // so far surfaces -- where one pixel spans more world space
                        // -- are not falsely rejected by sub-pixel parallax.
                        float tol = max(0.75, clip.w * 0.02);
                        float3 dlt = prevHP.xyz - hp;
                        [branch] if (prevHP.w > 0.5 && dot(dlt, dlt) <= tol * tol) {
                            srcPix = ppix;        // accept aligned history
                        } else {
                            histValid = false;    // disocclusion / different surface -> fresh
                        }
                    } else {
                        histValid = false;        // reprojected off-screen -> fresh
                    }
                } else {
                    histValid = false;            // behind previous camera -> fresh
                }
            } else {
                histValid = false;                // sky / miss -> fresh
            }
        }

        float4 hist = PrevAccum[srcPix];                       // ping-pong: read previous frame
        float n = (accumFrame == 0 || !histValid) ? 0.0 : min(hist.a, cap);
        float3 avg = (hist.rgb * n + col) / (n + 1.0);
        Accum[pix]  = float4(avg, n + 1.0);                    // ping-pong: write current frame
        HitPos[pix] = float4(hp, primHit ? 1.0 : 0.0);         // primary-hit world pos for next-frame reprojection
        col = avg;
    }

    col *= exposure;
    col = (hdr != 0) ? tonemapACES(col) : saturate(col);

    // Triangular dither: two decorrelated blue-noise samples -> triangular PDF
    // centered on 0 (range ±1/255).  Applied just before 8-bit quantisation
    // so smooth gradients never show banding on the R8G8B8A8 swapchain.
    // k=22,23 chosen well past the GI bounce range (k≤21 for 8 bounces).
    [branch] if (dither != 0) {
        float u1 = blue(int2(i.pos.xy), 22);
        float u2 = blue(int2(i.pos.xy), 23);
        float tri = (u1 + u2 - 1.0) / 255.0;   // triangular PDF, range (-1/255, +1/255)
        col = saturate(col + tri);
    }

    return float4(col, 1.0);
}
)HLSL"
// Adjacent raw-string literal #4 (C++ concatenates all chunks at compile time).
// Holds the MULTI-PASS GI-DENOISER offscreen SHADE entry point (PSShade) and its
// extra moment ping-pong UAVs. PSMain (single pass, mode 0) does not reference u4/u5
// or PSShade, so the unused declarations are harmless to its PSO/root-sig (a shader's
// USED resources need only be a subset of its root signature). Kept under 16380 bytes.
R"HLSL(
// SVGF luminance-moment temporal buffers (PING-PONG, same indexing as Accum):
//   Moments     = CURRENT frame moments written this frame (u4).
//   PrevMoments = PREVIOUS frame moments read for temporal reprojection (u5).
// .x = E[luminance] (1st moment), .y = E[luminance^2] (2nd moment), .z = sample count.
RWStructuredBuffer<float4> Moments     : register(u4);  // CURRENT luminance moments + count
RWStructuredBuffer<float4> PrevMoments : register(u5);  // PREVIOUS-frame moments (read-only this frame)

// DIRECT-term temporal buffers (PING-PONG, same indexing/reproject as Accum):
//   DirectAccum     = CURRENT frame's accumulated direct (sun*albedo + emissive) written this frame (u6).
//   PrevDirectAccum = PREVIOUS frame's accumulated direct read for temporal reprojection (u7).
// .rgb = running-average direct radiance; .a unused (count tracked by Accum/Moments).
// The DIRECT sun term is a single jittered soft-shadow ray per frame (penumbra jitter
// via sampleXi), so in QUALITY denoiser modes it is NOISY without accumulation. Mirror
// the indirect's temporal EMA -- REUSING the SAME reproject srcPix + sample count n --
// so the frac-advanced soft-shadow samples converge. The direct is NOT a-trous filtered
// (sharp shadow edges must not blur); temporal accumulation alone converges it.
RWStructuredBuffer<float4> DirectAccum     : register(u6);  // CURRENT direct accumulation: rgb (.a unused)
RWStructuredBuffer<float4> PrevDirectAccum : register(u7);  // PREVIOUS-frame direct accumulation (read-only this frame)

// MRT outputs of the offscreen SHADE pass = the G-BUFFER (Milestone A0). All RGBA16F
// full-res, consumed by the DEFERRED-LIGHTING passes (a-trous + composite). INDIRECT is
// DEMODULATED (no primary albedo) so the spatial filter never bleeds across albedo edges;
// albedo is re-applied at composite. (A1+ extend this G-buffer with explicit material +
// motion-vector channels once OBB raster / per-object grids land; A0 keeps it as-is so
// the deferred output stays pixel-identical to the fused single pass.)
struct ShadeOut {
    float4 indirect : SV_Target0;  // temporally-accumulated DEMODULATED indirect irradiance (.a = sample count)
    float4 direct   : SV_Target1;  // direct sun*albedo + self-emission (NOT to be blurred)
    float4 albedo   : SV_Target2;  // primary surface albedo (for re-modulation at composite)
    float4 gbuffer  : SV_Target3;  // world-space normal (.xyz) + linear camera depth (.w) for edge-stopping
    float4 variance : SV_Target4;  // .x = temporal luminance variance, .y = history sample count (SVGF)
};

// Shared G-buffer shading core (Milestone A0/A1). Given a primary ray (ro,rd) IN THE
// SAME SPACE the voxel grid is marched (world==voxel for the single global object) and
// the integer screen pixel |screenPos|, run the voxel DDA, compute the demodulated
// indirect / direct / albedo via giShadeSplit, temporally accumulate the indirect EXACTLY
// like the single pass (same reproject + Accum/PrevAccum ping-pong), accumulate luminance
// moments for SVGF, write the Accum/HitPos/Moments/DirectAccum ping-pong buffers, and
// return the five G-buffer MRT values. NO tonemap/dither here -- that is the composite
// (deferred-lighting) pass's job. Identical math whether invoked by the full-screen
// PSShade (A0) or the OBB-raster PSObb (A1), so the deferred output stays pixel-identical.
ShadeOut shadeGBuffer(float3 ro, float3 rd, float2 screenPos, out float3 outHitLocal) {
    ShadeOut o;
    float3 hp, nrm; uint mat;
    bool primHit = traceVoxel(ro, rd, 768, hp, nrm, mat);
    outHitLocal = hp;   // the primary-hit position in the SAME space the ray was given (object-local
                        // for an OBB draw); used by PSObb to write per-voxel SV_Depth for compositing.

    float3 alb, direct, indirect;
    if (primHit) {
        indirect = giShadeSplit(hp, nrm, mat, screenPos, alb, direct);
    } else {
        // Sky/miss: the demodulated-indirect channel carries the background sky so the
        // composite (DIRECT + ALBEDO*indirect) reproduces the sky where albedo=1,direct=0.
        alb      = float3(1, 1, 1);
        direct   = float3(0, 0, 0);
        indirect = sky(rd);
        nrm      = float3(0, 0, 0);
    }

    // Temporal accumulation of the DEMODULATED indirect -- identical scheme to PSMain
    // (reproject this pixel's primary hit through prevVP, reuse aligned history only on
    // the same world point, else start fresh). Ping-pong: read PrevAccum, write Accum.
    uint w   = (uint)viewport.x;
    uint h   = (uint)viewport.y;
    uint pix = (uint)screenPos.y * w + (uint)screenPos.x;

    uint  srcPix    = pix;
    bool  histValid = true;
    float cap       = (giDenoise != 0) ? max(giHistMax, 0.0) : 2048.0;

    [branch] if (reproject != 0) {
        [branch] if (primHit) {
            float4 clip = mul(prevVP, float4(hp, 1.0));
            [branch] if (clip.w > 1e-4) {
                float2 pndc = clip.xy / clip.w;
                float2 puv  = float2(pndc.x * 0.5 + 0.5, -pndc.y * 0.5 + 0.5);
                [branch] if (all(puv >= 0.0) && all(puv < 1.0)) {
                    int px = clamp((int)(puv.x * float(w)), 0, (int)w - 1);
                    int py = clamp((int)(puv.y * float(h)), 0, (int)h - 1);
                    uint ppix = (uint)py * w + (uint)px;
                    float4 prevHP = PrevHitPos[ppix];
                    float tol = max(0.75, clip.w * 0.02);
                    float3 dlt = prevHP.xyz - hp;
                    [branch] if (prevHP.w > 0.5 && dot(dlt, dlt) <= tol * tol) {
                        srcPix = ppix;
                    } else {
                        histValid = false;
                    }
                } else {
                    histValid = false;
                }
            } else {
                histValid = false;
            }
        } else {
            histValid = false;
        }
    }

    float4 hist  = PrevAccum[srcPix];
    float  n     = (accumFrame == 0 || !histValid) ? 0.0 : min(hist.a, cap);
    float3 indAvg = (hist.rgb * n + indirect) / (n + 1.0);

    // SVGF temporal luminance moments (1st & 2nd), accumulated like the colour above.
    float  lum   = dot(indirect, float3(0.2126, 0.7152, 0.0722));
    float4 pm    = PrevMoments[srcPix];
    float  m1    = (pm.x * n + lum)        / (n + 1.0);
    float  m2    = (pm.y * n + lum * lum)  / (n + 1.0);
    float  tvar  = max(m2 - m1 * m1, 0.0);   // temporal luminance variance (>=0)

    // Temporal accumulation of the DIRECT term (sun*albedo + self-emission). The
    // soft-shadow sun ray is jittered fresh each frame (sampleXi penumbra), so a single
    // frame is noisy in penumbrae; accumulate it EXACTLY like the indirect, REUSING the
    // SAME reproject srcPix + sample count n (same primary world point, same disocclusion
    // test -- do NOT recompute a second reproject). The emissive self-glow is constant
    // per pixel, so folding it into the accumulated value is harmless. NOT a-trous
    // filtered downstream (sharp shadow edges); temporal EMA alone converges the penumbra.
    float3 emit = primHit ? (alb * emission(mat) * emissiveSurface) : float3(0, 0, 0);
    float3 dcur   = direct * alb + emit;                          // this frame's direct (albedo-modulated) + self-emission
    float3 dirAvg = (PrevDirectAccum[srcPix].rgb * n + dcur) / (n + 1.0);  // n==0 (fresh/disocclusion) => dirAvg == dcur (no stale read)

    Accum[pix]       = float4(indAvg, n + 1.0);
    HitPos[pix]      = float4(hp, primHit ? 1.0 : 0.0);
    Moments[pix]     = float4(m1, m2, n + 1.0, 0.0);
    DirectAccum[pix] = float4(dirAvg, n + 1.0);                   // ping-pong: write current frame's accumulated direct

    // Write the G-buffer targets. The composite reconstructs colour as
    // DIRECT + ALBEDO * filtered(INDIRECT) + ALBEDO*self-emission. The accumulated
    // (albedo-modulated, soft-shadow-converged) direct + folded self-emission goes to
    // SV_Target1; it is not blurred and survives demodulation.
    o.indirect = float4(indAvg, n + 1.0);
    o.direct   = float4(dirAvg, 1.0);                             // temporally-accumulated direct (soft shadow converges)
    o.albedo   = float4(alb, 1.0);
    float linDepth = primHit ? length(hp - camPos) : 1e6;
    o.gbuffer  = float4(nrm, linDepth);
    o.variance = float4(tvar, n + 1.0, 0.0, 0.0);
    return o;
}

// G-BUFFER PASS (Milestone A0). Full-screen-triangle variant: reconstruct the primary
// world ray from the pixel exactly as PSMain does, then run the shared shading core. The
// ray space IS the world/voxel space for the single global grid, so this is byte-identical
// to the pre-A1 PSShade. (A1's OBB-raster PSObb produces the same G-buffer for the single
// world object; this entry point is retained for A/B and as the no-OBB fallback.)
ShadeOut PSShade(VSOut i) {
    float2 ndc = (i.pos.xy / viewport) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float tf = tan(fov * 0.5);
    float3 rd = normalize(camFwd + camRight * (ndc.x * tf * aspect) + camUp * (ndc.y * tf));
    float3 ro = camPos;
    float3 hitLocal;   // unused here (A0 full-screen has no depth buffer)
    return shadeGBuffer(ro, rd, i.pos.xy, hitLocal);
}
)HLSL"
// Adjacent raw-string literal #5 (C++ concatenates all chunks at compile time). Holds the
// Milestone A1 OBB-raster pipeline (cube VS + voxel-DDA PS) for the deferred G-buffer pass.
// Kept a separate literal so each chunk stays comfortably under MSVC's 16380-byte cap.
R"HLSL(
// ---- Milestone A1: OBB raster of a single VoxelObject ---------------------
// The deferred G-buffer pass rasterizes the object's oriented bounding box (a unit cube
// scaled to the voxel dims by the object's model matrix), BACKFACES ONLY (front-face
// cull, so a camera inside the volume still covers the screen), depth-test + write. The
// fragment shader reconstructs the world ray (identically to PSShade), transforms it into
// object-LOCAL voxel space via invModel, and runs the SAME voxel DDA, writing the SAME 5
// G-buffer MRT targets. For the single world object model/invModel are identity and the
// cube fills the view, so the output is pixel-identical to the full-screen A0 path.
cbuffer ObbObject : register(b1) {
    row_major float4x4 obbMVP;     // world->clip with a REAL perspective-z (model premultiplied): clip = mul(obbMVP, float4(unitCube*dims,1))
    row_major float4x4 obbInvModel;// world->object-local (voxel) space for the ray: identity for object #0
    float3             obbDims;    // object voxel dimensions (the unit cube is scaled by this)
    int                obbSkyOnMiss;// 1 = write the background sky on a DDA miss (the enclosing world object); 0 = discard (objects composited over it, A2+)
};

// 36-vertex unit cube ([0,1]^3) generated from SV_VertexID -- no vertex buffer. Wound
// OUTWARD CCW (each face's vertices are CCW seen from outside, built via U x V = N). The
// PSO uses FrontCounterClockwise=TRUE + CULL_FRONT, so the camera-facing (near) faces are
// culled and only the far faces are rasterized -- coverage survives the camera being
// inside the box. The far-face depth is what the hardware depth buffer records.
struct ObbVSOut { float4 pos : SV_Position; };
ObbVSOut VSObb(uint vid : SV_VertexID) {
    const float3 C[8] = {
        float3(0,0,0), float3(1,0,0), float3(1,1,0), float3(0,1,0),
        float3(0,0,1), float3(1,0,1), float3(1,1,1), float3(0,1,1),
    };
    const uint IDX[36] = {
        1,2,6, 1,6,5,   // +X
        0,4,7, 0,7,3,   // -X
        3,7,6, 3,6,2,   // +Y
        0,1,5, 0,5,4,   // -Y
        4,5,6, 4,6,7,   // +Z
        0,3,2, 0,2,1,   // -Z
    };
    float3 unit = C[IDX[vid]];
    float3 local = unit * obbDims;          // unit cube -> [0,dims] in object space
    ObbVSOut o;
    o.pos = mul(obbMVP, float4(local, 1.0));// obbMVP already folds the object model matrix
    return o;
}

// OBB fragment shader. Reconstruct the SAME world ray PSShade uses (from the rasterized
// pixel, not the interpolated vertex -- so coverage is the ONLY thing the cube changes),
// transform ray origin + direction into object-local voxel space via invModel, then run
// the shared G-buffer shading core. On a DDA miss: the enclosing world object writes the
// background sky (obbSkyOnMiss=1, pixel-identical to A0); a non-enclosing object discards
// (obbSkyOnMiss=0) so the depth buffer reveals whatever is behind it (A2+).
// PSObb output = the 5 G-buffer MRT targets PLUS an explicit SV_Depth so the hardware
// depth buffer records the PER-VOXEL hit depth (not the cube's far face). This is what
// lets MULTIPLE objects depth-composite correctly: object #0's solid voxels write their
// true depth, and a second object (A2) is occluded by / occludes them per-voxel. (For the
// single A1 world object the depth value never affects the COLOR output -- nothing reads
// it back and only one object draws -- so A1 stays pixel-identical to A0.)
struct ObbPSOut {
    float4 indirect : SV_Target0;
    float4 direct   : SV_Target1;
    float4 albedo   : SV_Target2;
    float4 gbuffer  : SV_Target3;
    float4 variance : SV_Target4;
    float  depth    : SV_Depth;
};
ObbPSOut PSObb(ObbVSOut i) {
    float2 ndc = (i.pos.xy / viewport) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float tf = tan(fov * 0.5);
    float3 rdW = normalize(camFwd + camRight * (ndc.x * tf * aspect) + camUp * (ndc.y * tf));
    float3 roW = camPos;
    // World -> object-local voxel space. invModel is identity for the single world object,
    // so roL/rdL == roW/rdW and the DDA marches the global grid exactly as before. (rd is a
    // direction => the translation row is dropped; it is NOT renormalized so the marched t
    // stays in world units, matching PSShade's linear-depth and reprojection math.)
    float3 roL = mul(obbInvModel, float4(roW, 1.0)).xyz;
    float3 rdL = mul(obbInvModel, float4(rdW, 0.0)).xyz;

    // Single shared trace (local-space AABB clip is built into traceVoxel). For the
    // enclosing world object (obbSkyOnMiss=1) a miss writes the sky -> pixel-identical to
    // the full-screen A0 path. A non-enclosing object (obbSkyOnMiss=0, A2+) discards on a
    // miss so the depth buffer reveals what is behind it; shadeGBuffer flags a miss by
    // leaving gbuffer.w at the 1e6 far-sentinel and nrm at zero.
    float3 hitLocal;
    ShadeOut o = shadeGBuffer(roL, rdL, i.pos.xy, hitLocal);
    bool miss = (o.gbuffer.w >= 1e6);
    [branch] if (obbSkyOnMiss == 0 && miss) discard;

    ObbPSOut po;
    po.indirect = o.indirect;
    po.direct   = o.direct;
    po.albedo   = o.albedo;
    po.gbuffer  = o.gbuffer;
    po.variance = o.variance;
    // PER-VOXEL depth: project the LOCAL hit position through obbMVP (= worldClip * model),
    // which lands it in the SAME clip space VSObb used, then NDC-z = clip.z/clip.w. On a
    // miss (sky, world object only) keep the rasterized far-face depth so sky stays at the
    // back and a nearer object still composites in front of it.
    [branch] if (miss) {
        po.depth = i.pos.z;
    } else {
        float4 clip = mul(obbMVP, float4(hitLocal, 1.0));
        po.depth = saturate(clip.z / max(clip.w, 1e-6));   // [0,1] hardware depth (LESS test)
    }
    return po;
}
)HLSL";

struct CamCB {
    float camPos[3];       float fov;             // row 0
    float camFwd[3];       float timeSec;         // row 1
    float camRight[3];     float aspect;           // row 2
    float camUp[3];        int   gridDim;          // row 3
    float clearCol[3];     float exposure;         // row 4
    float viewport[2];     int   hdr;              float shadowSoftness; // row 5 (pad repurposed)
    float sunDir[3];       float ambient;          // row 6
    float aoStrength;      float aoRadius;         int lightingMode; int accumFrame; // row 7
    int   dither;          int   aoSamples;        int shadowSamples; int giSamples; // row 8
    int   giDenoise;       float giHistMax;        int emptySkip; int brickDim;      // row 9
    int   giDebug;         int   giBounces;        float giEmissive; float voxEmissive;  // row 10
    float emissiveSurface; float giIntensity;      int reproject;    int ePad11c;        // row 11
    float prevVP[16];                                                                    // rows 12-15: previous-frame view-projection (row-major)
};
// The CBV is uploaded into a 256-byte buffer (MakeUpload(256)); keep the mirror at
// exactly 16 float4 rows so prevVP lands on rows 12-15 and nothing overruns.
static_assert(sizeof(CamCB) == 256, "CamCB must stay 256 bytes (16 x 16-byte rows) to match cbuffer + camBuf size");

// Milestone A1: per-OBB-object constants (HLSL cbuffer ObbObject : register(b1)). One
// per drawn VoxelObject; for the single world object model/invModel are identity and dims
// = the global grid. obbMVP folds the object model INTO the world->clip matrix (with a
// real perspective-z for hardware depth). Uploaded into a 256-byte CBV. Rows: 0-3 obbMVP,
// 4-7 obbInvModel, 8 = dims + skyOnMiss flag. Mirror must stay <= 256 bytes.
struct ObbCB {
    float mvp[16];        // rows 0-3: world->clip (model premultiplied), row-major
    float invModel[16];   // rows 4-7: world->object-local (voxel) space, row-major
    float dims[3];        // row 8.xyz: object voxel dimensions
    int   skyOnMiss;      // row 8.w: 1 = enclosing world object (write sky on miss); 0 = discard (A2+)
};
static_assert(sizeof(ObbCB) == 144, "ObbCB mirror must match the HLSL ObbObject cbuffer layout (fits in a 256-byte CBV)");

// ===========================================================================
// DEFERRED-LIGHTING shaders (Milestone A0; GI-denoiser modes 1 & 2). These read the
// G-buffer that PSShade wrote and produce the final image -- the deferred half of the
// G-buffer-pass/deferred-lighting-pass split. A SEPARATE compilation unit from kShader
// because these are pure image-space passes that need their OWN b0 cbuffer (kShader's
// b0 is the 256-byte Camera). They are full-res 1:1, so all texture reads use integer
// Load() at SV_Position -- NO sampler/static-sampler is required.
//
//   VSFsq      full-screen triangle (shared by a-trous + composite)
//   PSAtrous   one edge-aware 5x5 a-trous wavelet iteration on the DEMODULATED
//              indirect (ping-ponged by RenderFrame, step doubles each iteration).
//              svgf=0: fixed phiLum.  svgf=1: phiLum scaled by sqrt(variance) and a
//              3x3 variance handling on iteration 0 (spatial estimate when history
//              is short, gaussian pre-blur otherwise).
//   PSComposite  final = DIRECT + ALBEDO*filtered(INDIRECT); then exposure + ACES +
//              blue-noise dither (identical math to the single pass) -> backbuffer.
//
// The per-iteration a-trous params are supplied as ROOT 32-BIT CONSTANTS (not a CBV)
// so each recorded draw can carry its own stepSize/iter without juggling N upload
// buffers within one command list. Composite params are likewise root constants.
// ===========================================================================
const char* kDenoiseShader = R"HLSL(
struct VSOut { float4 pos : SV_Position; };
VSOut VSFsq(uint vid : SV_VertexID) {
    VSOut o;
    float2 t = float2((vid << 1) & 2, vid & 2);   // (0,0)(2,0)(0,2) full-screen triangle
    o.pos = float4(t * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}

float3 tonemapACES(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// ---- a-trous wavelet iteration -------------------------------------------
cbuffer Atrous : register(b0) {
    int   stepSize;     // 1,2,4,8,16,32 -- doubles each iteration
    float phiNormal;    // normal edge-stop exponent
    float phiDepth;     // depth edge-stop scale
    float phiLum;       // luminance edge-stop sigma (svgf scales this by sqrt(variance))
    int   svgf;         // 0 = ATROUS (fixed phiLum), 1 = SVGF (variance-driven)
    int   atrousIter;   // 0-based iteration index (0 = first)
    int   awidth;
    int   aheight;
};
Texture2D<float4> Indirect : register(t0);   // DEMODULATED indirect (source ping)
Texture2D<float4> GBuf     : register(t1);   // normal.xyz + linear depth.w
Texture2D<float4> VarianceT: register(t2);   // .x = temporal luminance variance, .y = history count

// SVGF temporal COLOUR feedback (classic 1st-iteration recirculation). On a-trous
// iteration 0 ONLY we overwrite the temporal history that PSShade reprojects next
// frame (the SAME physical buffer SHADE wrote this frame as Accum/u0) with the
// FILTERED indirect instead of the raw pre-filter accumulator. This keeps the
// reprojected history LOW-VARIANCE so a noisy new sample shifts the running mean far
// less -> dark/shadow flicker is removed, while the mean still converges correctly.
// The per-pixel sample count (n+1) is preserved verbatim from VarianceT.y (which
// SHADE wrote alongside the colour) so PSShade's (hist*n + new)/(n+1) is unchanged.
// Same indexing as Accum in kShader: pix = row * awidth + col. Iterations > 0 never
// touch this buffer. Mode 0 (single pass) never runs this pass, so it is unaffected.
RWStructuredBuffer<float4> Accum : register(u0);  // temporal indirect history (write iter-0 feedback)

float lumOf(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// 3x3 luminance variance of the demodulated indirect around pixel p (used as the
// SVGF spatial fallback when temporal history is too short to be reliable).
float spatialLumVar(int2 p) {
    float s = 0.0, s2 = 0.0; int cnt = 0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx) {
        int2 q = clamp(p + int2(dx, dy), int2(0,0), int2(awidth - 1, aheight - 1));
        float l = lumOf(Indirect.Load(int3(q, 0)).rgb);
        s += l; s2 += l * l; ++cnt;
    }
    float inv = 1.0 / float(cnt);
    return max(s2 * inv - (s * inv) * (s * inv), 0.0);
}

// 3x3 gaussian pre-blur of the temporal variance (SVGF iteration-0 step).
float gaussianVar(int2 p) {
    const float k[3] = { 1.0, 2.0, 1.0 };  // [1 2 1]/16 separable
    float vsum = 0.0, wsum = 0.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx) {
        int2 q = clamp(p + int2(dx, dy), int2(0,0), int2(awidth - 1, aheight - 1));
        float wv = k[dx + 1] * k[dy + 1];
        vsum += VarianceT.Load(int3(q, 0)).x * wv;
        wsum += wv;
    }
    return (wsum > 0.0) ? (vsum / wsum) : 0.0;
}

float4 PSAtrous(VSOut i) : SV_Target {
    int2 p = int2(i.pos.xy);

    float4 cInd = Indirect.Load(int3(p, 0));
    float4 cG   = GBuf.Load(int3(p, 0));
    float3 nc   = cG.xyz;
    float  zc   = cG.w;
    float  lc   = lumOf(cInd.rgb);

    // SVGF luminance edge-stop sigma. Mode 1 (svgf==0) uses a fixed phiLum. Mode 2
    // drives it by the local variance: low variance (converged) -> small sigma ->
    // sharp; high variance (noisy) -> large sigma -> aggressive blur. On iteration 0,
    // fall back to a 3x3 spatial variance estimate when temporal history is short
    // (< 4 samples), else use a 3x3 gaussian pre-blur of the temporal variance.
    float lumSigma = max(phiLum, 1e-3);
    [branch] if (svgf != 0) {
        float vEst;
        [branch] if (atrousIter == 0) {
            float histLen = VarianceT.Load(int3(p, 0)).y;
            vEst = (histLen < 4.0) ? spatialLumVar(p) : gaussianVar(p);
        } else {
            vEst = VarianceT.Load(int3(p, 0)).x;
        }
        lumSigma = phiLum * sqrt(max(vEst, 0.0)) + 1e-3;
    }

    const float kern[3] = { 1.0, 0.66666667, 0.16666667 };  // B3-spline 5-tap

    float3 sum  = float3(0, 0, 0);
    float  wsum = 0.0;
    [loop] for (int dy = -2; dy <= 2; ++dy) {
        [loop] for (int dx = -2; dx <= 2; ++dx) {
            int2 q = p + int2(dx, dy) * stepSize;
            // Clamp to screen; off-screen taps fold back to the centre (weight via clamp).
            q = clamp(q, int2(0, 0), int2(awidth - 1, aheight - 1));

            float h = kern[abs(dx)] * kern[abs(dy)];
            [branch] if (dx == 0 && dy == 0) {
                // Centre tap always keeps full kernel weight (no edge-stop), so sky /
                // zero-normal / short-history pixels never reject themselves.
                sum  += cInd.rgb * h;
                wsum += h;
                continue;
            }

            float4 tInd = Indirect.Load(int3(q, 0));
            float4 tG   = GBuf.Load(int3(q, 0));
            float3 nt   = tG.xyz;
            float  zt   = tG.w;
            float  lt   = lumOf(tInd.rgb);

            float wN = pow(max(0.0, dot(nc, nt)), phiNormal);
            float wZ = exp(-abs(zc - zt) / max(phiDepth * (abs(zc) * 0.1 + 1.0), 1e-3));
            float wL = exp(-abs(lc - lt) / lumSigma);
            float wgt = h * wN * wZ * wL;

            sum  += tInd.rgb * wgt;
            wsum += wgt;
        }
    }
    float3 outc = (wsum > 1e-8) ? (sum / wsum) : cInd.rgb;

    // SVGF colour feedback: on the FIRST a-trous iteration recirculate the filtered
    // result into the temporal history PSShade reprojects next frame (NOT the final,
    // wide-blur output -- the 1st iteration is denoised enough to stabilise temporally
    // without leaking spatial blur into the temporal signal). Preserve the count from
    // VarianceT.y (the n+1 SHADE stored), so the running mean keeps converging. Both
    // ATROUS (svgf==0) and SVGF (svgf==1) feed back identically. A UAV barrier in
    // RenderFrame orders this write after SHADE's Accum write to the same buffer.
    [branch] if (atrousIter == 0) {
        uint pix = (uint)p.y * (uint)awidth + (uint)p.x;
        float count = VarianceT.Load(int3(p, 0)).y;   // history sample count (n+1) from SHADE
        Accum[pix] = float4(outc, count);
    }

    return float4(outc, cInd.a);   // carry the centre sample count in .a (unused downstream)
}
)HLSL"
R"HLSL(
// ---- composite ------------------------------------------------------------
cbuffer Comp : register(b0) {
    float cExposure;
    int   cHdr;
    int   cDither;
    int   cwidth;
    int   cheight;
};
Texture2D<float4>      DirectT : register(t0);   // direct*albedo + emissive (not blurred)
Texture2D<float4>      AlbedoT : register(t1);   // primary albedo (re-modulation)
Texture2D<float4>      IndirT  : register(t2);   // filtered DEMODULATED indirect
StructuredBuffer<float> BlueNoise : register(t3);

float blueC(int2 px, int k) {
    return BlueNoise[((px.x + k * 13) & 63) + (((px.y + k * 7) & 63) << 6)];
}

float4 PSComposite(VSOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float3 direct = DirectT.Load(int3(p, 0)).rgb;
    float3 albedo = AlbedoT.Load(int3(p, 0)).rgb;
    float3 indir  = IndirT.Load(int3(p, 0)).rgb;

    // Re-modulate: re-apply albedo to the (now denoised) indirect, add the unblurred
    // direct+emissive. Identity-filter equivalence to the single pass:
    //   direct*alb + emit + alb*indirectAvg  ==  alb*(direct+indirectAvg)+emit.
    float3 col = direct + albedo * indir;

    col *= cExposure;
    col = (cHdr != 0) ? tonemapACES(col) : saturate(col);

    [branch] if (cDither != 0) {
        float u1 = blueC(p, 22);
        float u2 = blueC(p, 23);
        float tri = (u1 + u2 - 1.0) / 255.0;   // triangular PDF, range (-1/255, +1/255)
        col = saturate(col + tri);
    }
    return float4(col, 1.0);
}
)HLSL";

std::vector<std::uint32_t> GenerateScene(UINT g) {
    std::vector<std::uint32_t> v(static_cast<size_t>(g) * g * g, 0);
    // Scale all geometry proportionally with grid size so the demo fills the
    // volume like it did at 64^3: terrain height ~g*0.34, sphere at
    // (g*0.69, g*0.66, g*0.625), radius ~g*0.125.
    const float fg = float(g);
    const float baseH  = fg * 0.34f;  // terrain base height
    const float ampH   = fg * 0.125f; // terrain amplitude
    const float sphCx  = fg * 0.69f;
    const float sphCy  = fg * 0.66f;
    const float sphCz  = fg * 0.625f;
    const float sphR2  = (fg * 0.125f) * (fg * 0.125f);  // radius^2
    for (UINT z = 0; z < g; ++z)
        for (UINT x = 0; x < g; ++x) {
            float h = baseH + ampH * std::sin(x * 0.20f) * std::cos(z * 0.18f);
            for (UINT y = 0; y < g; ++y) {
                std::uint32_t m = 0;
                if (float(y) < h) m = (float(y) > h - 1.5f) ? 1u : (float(y) > h - 5.0f ? 2u : 3u);
                float dx = float(x) - sphCx, dy = float(y) - sphCy, dz = float(z) - sphCz;
                if (dx * dx + dy * dy + dz * dz < sphR2) m = 4;  // floating sphere (casts a clear shadow)
                v[size_t(z) * g * g + size_t(y) * g + x] = m;
            }
        }
    return v;
}

// Milestone A2: a small procedural TEST voxel cube (g^3, intended g=16). A fully
// SOLID cube split into colored regions (material indices 1..4) so its arbitrary
// rotation reads unmistakably: each axis-octant gets a distinct face/quadrant color,
// proving multi-object + arbitrary transforms via the shared OBB-raster path. Indexed
// z*g*g + y*g + x to match the shader's cell.z*gridDim*gridDim + cell.y*gridDim + cell.x.
std::vector<std::uint32_t> GenerateTestCube(UINT g) {
    std::vector<std::uint32_t> v(static_cast<size_t>(g) * g * g, 0);
    const UINT half = g / 2;
    for (UINT z = 0; z < g; ++z)
        for (UINT y = 0; y < g; ++y)
            for (UINT x = 0; x < g; ++x) {
                // Solid cube; color by octant so rotation is obvious (1..4 cycle).
                std::uint32_t m = 1u + (((x >= half) ? 1u : 0u) +
                                        ((y >= half) ? 2u : 0u) +
                                        ((z >= half) ? 1u : 0u)) % 4u;
                v[size_t(z) * g * g + size_t(y) * g + x] = m;
            }
    return v;
}

// Build the coarse brick-occupancy grid from a dense flat voxel grid.
// Each brick cell is 1 iff ANY voxel inside the BRICK_DIM^3 brick is nonzero,
// so the structure is conservative (an empty brick is provably solid-free).
// |grid| is a flat g^3 array (z*g*g + y*g + x). Output is kBrickCount uints,
// flat-indexed (bz*kBrickGrid + by)*kBrickGrid + bx to match the shader.
std::vector<std::uint32_t> BuildBrickOccupancy(const std::vector<std::uint32_t>& grid, UINT g) {
    std::vector<std::uint32_t> occ(kBrickCount, 0u);
    const std::size_t need = static_cast<std::size_t>(g) * g * g;
    if (grid.size() < need) return occ;  // defensive: leave empty (skips nothing extra)
    for (UINT z = 0; z < g; ++z) {
        const UINT bz = z / kBrickDim;
        for (UINT y = 0; y < g; ++y) {
            const UINT by = y / kBrickDim;
            const std::size_t rowBase = (static_cast<std::size_t>(z) * g + y) * g;
            for (UINT x = 0; x < g; ++x) {
                if (grid[rowBase + x] != 0u) {
                    const UINT bx = x / kBrickDim;
                    occ[(static_cast<std::size_t>(bz) * kBrickGrid + by) * kBrickGrid + bx] = 1u;
                }
            }
        }
    }
    return occ;
}

void Cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
void Norm(float v[3]) {
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-8f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

}  // namespace

struct Renderer::Impl {
    static constexpr UINT kFrames = 3;
    ComPtr<ID3D12Device>              device;
    ComPtr<ID3D12CommandQueue>        queue;
    ComPtr<IDXGISwapChain3>           swap;
    ComPtr<ID3D12DescriptorHeap>      rtvHeap;
    UINT                              rtvSize = 0;
    ComPtr<ID3D12Resource>            targets[kFrames];
    ComPtr<ID3D12CommandAllocator>    alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence>               fence;
    UINT64                            fenceVal = 0;
    HANDLE                            fenceEvent = nullptr;
    bool                              tearing = false;
    UINT                              width = 0, height = 0;

    ComPtr<ID3D12RootSignature>       rootSig;
    ComPtr<ID3D12PipelineState>       pso;
    ComPtr<ID3D12Resource>            voxelBuf;
    std::uint32_t*                    voxelPtr   = nullptr;      // persistent map into voxelBuf
    ComPtr<ID3D12Resource>            camBuf;
    std::uint8_t*                     camPtr = nullptr;
    // Milestone A1 OBB-raster G-buffer pass (deferred path only): per-object constants
    // (b1), a unit-cube PSO drawn backfaces-only with depth test+write, and a full-res
    // depth buffer + DSV. obbBuf is a persistently-mapped 256-byte CBV updated each frame.
    ComPtr<ID3D12Resource>            obbBuf;
    std::uint8_t*                     obbPtr = nullptr;          // persistent map into obbBuf
    // Milestone A2 (FrameParams::test_object, default OFF): a SECOND VoxelObject -- a small
    // procedural test cube drawn through the SAME A1 OBB-raster path at a Y-rotating transform,
    // depth-composited against the world object. Its own little grid + palette + a second
    // ObbObject (b1) CB + a second Camera (b0) CB (so the DDA's gridDim is the test-cube size
    // and empty-skip is off). All persistently mapped; the test camera CB is rebuilt each frame
    // to mirror the live camera. INERT unless test_object != 0 AND the deferred OBB path is on.
    static constexpr UINT             kTestDim = 16;             // test cube voxel dimensions (16^3)
    ComPtr<ID3D12Resource>            testVoxelBuf;              // 16^3 uint material grid (rebinds t0 for the test draw)
    ComPtr<ID3D12Resource>            testPaletteBuf;            // test-cube RGBA8 palette (rebinds t2 for the test draw)
    ComPtr<ID3D12Resource>            testObbBuf;                // 2nd ObbObject CB (b1): rotating model/invModel/dims/skyOnMiss=0
    std::uint8_t*                     testObbPtr = nullptr;      // persistent map into testObbBuf
    ComPtr<ID3D12Resource>            testCamBuf;                // 2nd Camera CB (b0): same view, gridDim=kTestDim, emptySkip=0
    std::uint8_t*                     testCamPtr = nullptr;      // persistent map into testCamBuf
    ComPtr<ID3D12Resource>            depthTex;                  // full-res D32 depth for the OBB pass
    ComPtr<ID3D12DescriptorHeap>      dsvHeap;                   // single DSV
    ComPtr<ID3D12RootSignature>       obbRootSig;                // SHADE root sig + the b1 OBB CBV
    ComPtr<ID3D12PipelineState>       obbPso;                    // VSObb + PSObb, front-cull, depth+5 MRT
    // GI temporal accumulation -- PING-PONG pair (swapped each frame): one set is
    // CURRENT (written this frame: u0 accum / u1 hitpos), the other is PREVIOUS
    // (read this frame for reprojection: u2 accum / u3 hitpos). accumPair selects
    // which physical buffer is "current". Both default-heap RWStructuredBuffer<float4>.
    ComPtr<ID3D12Resource>            accumBuf[2];    // [i]=GI running avg+count
    ComPtr<ID3D12Resource>            hitPosBuf[2];   // [i]=primary-hit world pos + valid flag
    // DIRECT-term temporal accumulation -- PING-PONG pair mirroring accumBuf (multi-pass
    // denoiser modes 1/2 only; SHADE's u6/u7). curr=write (u6 DirectAccum), prev=read
    // (u7 PrevDirectAccum); swapped by accumPair each frame. RWStructuredBuffer<float4>.
    ComPtr<ID3D12Resource>            directAccumBuf[2]; // [i]=accumulated direct (sun*alb + emissive)
    UINT                              accumPair = 0;  // index of the CURRENT buffer this frame
    ComPtr<ID3D12Resource>            blueNoiseBuf;   // 64x64 blue-noise tile (StructuredBuffer<float> t1)
    ComPtr<ID3D12Resource>            paletteBuf;     // 256-entry RGBA8 palette (StructuredBuffer<uint> t2)
    std::uint32_t*                    palettePtr = nullptr;      // persistent map into paletteBuf
    ComPtr<ID3D12Resource>            brickBuf;       // coarse occupancy (StructuredBuffer<uint> t3) for empty-space skip
    std::uint32_t*                    brickPtr = nullptr;        // persistent map into brickBuf
    float*                            bnPtr      = nullptr;      // persistent map into blueNoiseBuf
    std::vector<float>                bnTile;                   // async-baked real tile (worker writes here)
    std::thread                       bnThread;                 // background void-and-cluster bake
    std::atomic<bool>                 bnReady{false};           // worker sets true when bnTile is complete
    bool                              bnUploaded = false;       // set true once we've swapped real tile in
    // GI temporal accumulation state. Split into a CAMERA key (pos/yaw/pitch/fov)
    // and a SCENE key (sun/exposure/lighting/sample counts/...). A scene change
    // invalidates history -> hard reset (accumFrame=0). A pure camera change keeps
    // history and only shortens the accumulation cap (motion-adaptive EMA) when
    // denoise is enabled; with denoise off it still hard-resets like the original.
    // MUST equal the element count of the camKey / sceneKey arrays built in
    // RenderFrame. (A mismatch makes memcmp/memcpy over/under-run -> "scene
    // changed" fires every frame -> accumFrame resets every frame -> GI never
    // converges. The sceneKey literal below uses these via static_assert.)
    static constexpr int kCamKeyLen   = 6;
    static constexpr int kSceneKeyLen = 23;  // +1 for gi_denoiser (mode switch -> reset; see RenderFrame)
    float                             accCamKey[kCamKeyLen]     = {};
    float                             accSceneKey[kSceneKeyLen] = {};
    bool                              accHave = false;
    UINT                              accumFrame = 0;
    bool                              giDenoise = true;   // QUALITY temporal denoise (Renderer::SetGiDenoise)
    bool                              emptySkip = true;   // empty-space skipping (Renderer::SetEmptySpaceSkip)
    bool                              giDebug   = false;  // GI-only debug view (Renderer::SetGiDebug)
    bool                              giReproject = true; // QUALITY temporal reprojection (Renderer::SetGiReproject)
    float                             prevVP[16] = {};    // previous frame's view-projection (row-major) for reprojection
    bool                              havePrevVP = false; // false until the first frame's VP has been stored

    // ---- Multi-pass GI DENOISER (gi_denoiser 1=ATROUS / 2=SVGF) -------------
    // All resources are allocated up front in Init so switching modes at runtime is
    // pure branching in RenderFrame; the extra passes only execute when multipass is
    // active (gi_denoiser != 0 AND lighting_mode == 1 / QUALITY). Mode 0 ignores ALL
    // of these and runs the original single pass via rootSig/pso/PSMain unchanged.
    bool                              denoiseReady = false;        // true once all denoiser objects built
    // Offscreen full-res RGBA16F targets (RTV written, SRV read). indirectTex is a
    // ping-pong pair the a-trous filter bounces between (SHADE writes [0]).
    ComPtr<ID3D12Resource>            indirectTex[2];              // demodulated indirect (a-trous ping-pong)
    ComPtr<ID3D12Resource>            directTex;                   // direct*albedo + emissive
    ComPtr<ID3D12Resource>            albedoTex;                   // primary albedo
    ComPtr<ID3D12Resource>            gbufTex;                     // normal.xyz + linear depth.w
    ComPtr<ID3D12Resource>            varianceTex;                 // .x variance, .y history count (SVGF)
    D3D12_RESOURCE_STATES             texState[6] = {};            // tracked state of the 6 offscreen RTs
    // Moments ping-pong for SVGF temporal accumulation (mirrors accumBuf indexing).
    ComPtr<ID3D12Resource>            momentBuf[2];                // float4: m1, m2, count, pad
    // RTV heap for the offscreen targets (separate from the swapchain rtvHeap).
    ComPtr<ID3D12DescriptorHeap>      offRtvHeap;
    UINT                              offRtvSize = 0;
    // Shader-visible CBV_SRV_UAV heap holding the 6 offscreen-texture SRVs, in fixed
    // slots: 0 indirect[0], 1 indirect[1], 2 direct, 3 albedo, 4 gbuf, 5 variance.
    ComPtr<ID3D12DescriptorHeap>      srvHeap;
    UINT                              srvInc = 0;
    // SHADE pass: own root sig + PSO (adds moment UAVs u4/u5 over the mode-0 rootSig),
    // MRT to the 5 offscreen targets. Reuses the Camera CBV + voxel/blue/palette/brick
    // SRVs + Accum/HitPos ping-pong UAVs exactly like the single pass.
    ComPtr<ID3D12RootSignature>       shadeRootSig;
    ComPtr<ID3D12PipelineState>       shadePso;
    // a-trous + composite root sigs and PSOs.
    ComPtr<ID3D12RootSignature>       atrousRootSig;
    ComPtr<ID3D12PipelineState>       atrousPso;
    ComPtr<ID3D12RootSignature>       compositeRootSig;
    ComPtr<ID3D12PipelineState>       compositePso;

    // GPU descriptor handle for SRV heap slot |slot| (0..5).
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(UINT slot) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(slot) * srvInc;
        return h;
    }
    // CPU descriptor handle for offscreen RTV |idx| (0..5).
    D3D12_CPU_DESCRIPTOR_HANDLE OffRtv(UINT idx) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = offRtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(idx) * offRtvSize;
        return h;
    }

    void WaitIdle() {
        if (!queue || !fence) return;
        const UINT64 v = ++fenceVal;
        queue->Signal(fence.Get(), v);
        if (fence->GetCompletedValue() < v) {
            fence->SetEventOnCompletion(v, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }
    ComPtr<ID3D12Resource> MakeUpload(UINT64 bytes) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                        IID_PPV_ARGS(&r));
        return r;
    }
    ComPtr<ID3D12Resource> MakeDefaultUAV(UINT64 bytes) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                        IID_PPV_ARGS(&r));
        return r;
    }
    // Full-res RGBA16F render-target texture (RTV-writable + SRV-readable) for the
    // multi-pass denoiser offscreen targets. Created in RENDER_TARGET state so every
    // frame can start from a known state (RenderFrame restores all to RT at frame end).
    ComPtr<ID3D12Resource> MakeRenderTex(UINT w, UINT h) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                        IID_PPV_ARGS(&r));
        return r;
    }
    // Build every multi-pass-denoiser object (offscreen RTs + RTV/SRV heaps + moment
    // buffers + SHADE/a-trous/composite root sigs & PSOs). Returns true on success
    // (sets denoiseReady). Defined out-of-line below Init. |kVox|/|kDenoise| are the
    // two HLSL source blobs (kShader / kDenoiseShader).
    bool InitDenoiser(const char* kVox, const char* kDenoise);
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() { Shutdown(); }

bool Renderer::Impl::InitDenoiser(const char* kVox, const char* kDenoise) {
    Impl& d = *this;
    const UINT W = d.width, H = d.height;

    // --- 6 offscreen RGBA16F render targets (all start RENDER_TARGET) ---
    d.indirectTex[0] = d.MakeRenderTex(W, H);
    d.indirectTex[1] = d.MakeRenderTex(W, H);
    d.directTex      = d.MakeRenderTex(W, H);
    d.albedoTex      = d.MakeRenderTex(W, H);
    d.gbufTex        = d.MakeRenderTex(W, H);
    d.varianceTex    = d.MakeRenderTex(W, H);
    if (!d.indirectTex[0] || !d.indirectTex[1] || !d.directTex || !d.albedoTex ||
        !d.gbufTex || !d.varianceTex) return false;
    for (int i = 0; i < 6; ++i) d.texState[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // --- Moment ping-pong buffers (float4 per pixel: m1, m2, count, pad) ---
    {
        const UINT64 pixBytes = static_cast<UINT64>(W) * H * 16;
        for (int i = 0; i < 2; ++i) {
            d.momentBuf[i] = d.MakeDefaultUAV(pixBytes);
            if (!d.momentBuf[i]) return false;
        }
    }

    // --- RTV heap for the 6 offscreen targets (slots match texState[] order) ---
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 6;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(d.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&d.offRtvHeap)))) return false;
        d.offRtvSize = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        ID3D12Resource* rts[6] = { d.indirectTex[0].Get(), d.indirectTex[1].Get(), d.directTex.Get(),
                                   d.albedoTex.Get(), d.gbufTex.Get(), d.varianceTex.Get() };
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (UINT i = 0; i < 6; ++i)
            d.device->CreateRenderTargetView(rts[i], &rv, d.OffRtv(i));
    }

    // --- Shader-visible SRV heap: 6 SRVs in fixed slots ---
    //   0 indirect[0], 1 indirect[1], 2 direct, 3 albedo, 4 gbuf, 5 variance
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 6;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(d.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&d.srvHeap)))) return false;
        d.srvInc = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ID3D12Resource* srcs[6] = { d.indirectTex[0].Get(), d.indirectTex[1].Get(), d.directTex.Get(),
                                    d.albedoTex.Get(), d.gbufTex.Get(), d.varianceTex.Get() };
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE ch = d.srvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < 6; ++i) {
            d.device->CreateShaderResourceView(srcs[i], &sv, ch);
            ch.ptr += d.srvInc;
        }
    }

    UINT cf = 0;
#if defined(_DEBUG)
    cf = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    // ===================== SHADE pass (MRT) =====================
    // Root sig = the mode-0 layout (b0; t0,t1,t2,t3; u0,u1,u2,u3) PLUS moment UAVs
    // u4 (Moments) and u5 (PrevMoments) PLUS the DIRECT-term ping-pong u6 (DirectAccum)
    // and u7 (PrevDirectAccum). 13 root descriptors, all buffers.
    {
        D3D12_ROOT_PARAMETER rp[13]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[0].Descriptor.ShaderRegister = 0;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[1].Descriptor.ShaderRegister = 0;  // t0 Voxels
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[2].Descriptor.ShaderRegister = 0;  // u0 Accum
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[3].Descriptor.ShaderRegister = 1;  // t1 BlueNoise
        rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[4].Descriptor.ShaderRegister = 2;  // t2 Palette
        rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[5].Descriptor.ShaderRegister = 3;  // t3 Brick
        rp[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[6].Descriptor.ShaderRegister = 1;  // u1 HitPos
        rp[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[7].Descriptor.ShaderRegister = 2;  // u2 PrevAccum
        rp[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[8].Descriptor.ShaderRegister = 3;  // u3 PrevHitPos
        rp[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[9].Descriptor.ShaderRegister = 4;  // u4 Moments
        rp[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[10].Descriptor.ShaderRegister = 5; // u5 PrevMoments
        rp[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[11].Descriptor.ShaderRegister = 6; // u6 DirectAccum
        rp[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[12].Descriptor.ShaderRegister = 7; // u7 PrevDirectAccum
        for (auto& p : rp) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 13; rs.pParameters = rp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return false;
        if (FAILED(d.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&d.shadeRootSig)))) return false;

        ComPtr<ID3DBlob> vs, ps, err2;
        if (FAILED(D3DCompile(kVox, std::strlen(kVox), "voxel", nullptr, nullptr, "VSMain", "vs_5_1", cf, 0, &vs, &err2))) {
            vox::log::Error("DX12: SHADE VS compile: {}", err2 ? static_cast<const char*>(err2->GetBufferPointer()) : "?");
            return false;
        }
        if (FAILED(D3DCompile(kVox, std::strlen(kVox), "voxel", nullptr, nullptr, "PSShade", "ps_5_1", cf, 0, &ps, &err2))) {
            vox::log::Error("DX12: PSShade compile: {}", err2 ? static_cast<const char*>(err2->GetBufferPointer()) : "?");
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psod{};
        psod.pRootSignature = d.shadeRootSig.Get();
        psod.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        psod.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        for (int i = 0; i < 5; ++i) psod.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psod.SampleMask = UINT_MAX;
        psod.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psod.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psod.RasterizerState.DepthClipEnable = TRUE;
        psod.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psod.NumRenderTargets = 5;
        for (int i = 0; i < 5; ++i) psod.RTVFormats[i] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psod.SampleDesc.Count = 1;
        if (FAILED(d.device->CreateGraphicsPipelineState(&psod, IID_PPV_ARGS(&d.shadePso)))) {
            vox::log::Error("DX12: SHADE PSO"); return false;
        }
    }

    // Common full-screen-triangle VS for a-trous + composite (from kDenoise).
    ComPtr<ID3DBlob> fsqVs, fsqErr;
    if (FAILED(D3DCompile(kDenoise, std::strlen(kDenoise), "denoise", nullptr, nullptr, "VSFsq", "vs_5_1", cf, 0, &fsqVs, &fsqErr))) {
        vox::log::Error("DX12: VSFsq compile: {}", fsqErr ? static_cast<const char*>(fsqErr->GetBufferPointer()) : "?");
        return false;
    }

    // ===================== a-trous pass =====================
    // Root sig: b0 = 8 root 32-bit constants (per-iteration params), three single-SRV
    // descriptor tables for t0 (indirect src), t1 (gbuf), t2 (variance), and a root UAV
    // u0 (Accum) for the SVGF 1st-iteration COLOUR FEEDBACK. PSAtrous writes Accum on
    // iteration 0 only (recirculating the filtered indirect into the temporal history
    // PSShade reprojects next frame); other iterations leave it untouched. Bound to the
    // SAME accumBuf SHADE wrote this frame, so the filtered result becomes next frame's
    // PrevAccum. A root UAV needs no descriptor-heap slot (raw GPU VA, like SHADE's u0).
    {
        D3D12_DESCRIPTOR_RANGE r0{}, r1{}, r2{};
        r0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r0.NumDescriptors = 1; r0.BaseShaderRegister = 0;
        r1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r1.NumDescriptors = 1; r1.BaseShaderRegister = 1;
        r2.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r2.NumDescriptors = 1; r2.BaseShaderRegister = 2;
        D3D12_ROOT_PARAMETER rp[5]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants.ShaderRegister = 0; rp[0].Constants.Num32BitValues = 8;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &r0;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[2].DescriptorTable.NumDescriptorRanges = 1; rp[2].DescriptorTable.pDescriptorRanges = &r1;
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[3].DescriptorTable.NumDescriptorRanges = 1; rp[3].DescriptorTable.pDescriptorRanges = &r2;
        rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp[4].Descriptor.ShaderRegister = 0;  // u0 Accum (iter-0 colour feedback)
        for (auto& p : rp) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 5; rs.pParameters = rp;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
            vox::log::Error("DX12: a-trous root sig: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
        if (FAILED(d.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&d.atrousRootSig)))) return false;

        ComPtr<ID3DBlob> ps, err2;
        if (FAILED(D3DCompile(kDenoise, std::strlen(kDenoise), "denoise", nullptr, nullptr, "PSAtrous", "ps_5_1", cf, 0, &ps, &err2))) {
            vox::log::Error("DX12: PSAtrous compile: {}", err2 ? static_cast<const char*>(err2->GetBufferPointer()) : "?");
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psod{};
        psod.pRootSignature = d.atrousRootSig.Get();
        psod.VS = {fsqVs->GetBufferPointer(), fsqVs->GetBufferSize()};
        psod.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        psod.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psod.SampleMask = UINT_MAX;
        psod.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psod.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psod.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psod.NumRenderTargets = 1;
        psod.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psod.SampleDesc.Count = 1;
        if (FAILED(d.device->CreateGraphicsPipelineState(&psod, IID_PPV_ARGS(&d.atrousPso)))) {
            vox::log::Error("DX12: a-trous PSO"); return false;
        }
    }

    // ===================== composite pass =====================
    // Root sig: b0 = 5 root constants, three single-SRV tables (t0 direct, t1 albedo,
    // t2 filtered indirect), plus a root SRV t3 for the blue-noise buffer.
    {
        D3D12_DESCRIPTOR_RANGE r0{}, r1{}, r2{};
        r0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r0.NumDescriptors = 1; r0.BaseShaderRegister = 0;
        r1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r1.NumDescriptors = 1; r1.BaseShaderRegister = 1;
        r2.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r2.NumDescriptors = 1; r2.BaseShaderRegister = 2;
        D3D12_ROOT_PARAMETER rp[5]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants.ShaderRegister = 0; rp[0].Constants.Num32BitValues = 5;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &r0;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[2].DescriptorTable.NumDescriptorRanges = 1; rp[2].DescriptorTable.pDescriptorRanges = &r1;
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[3].DescriptorTable.NumDescriptorRanges = 1; rp[3].DescriptorTable.pDescriptorRanges = &r2;
        rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[4].Descriptor.ShaderRegister = 3;  // t3 BlueNoise
        for (auto& p : rp) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 5; rs.pParameters = rp;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
            vox::log::Error("DX12: composite root sig: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
        if (FAILED(d.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&d.compositeRootSig)))) return false;

        ComPtr<ID3DBlob> ps, err2;
        if (FAILED(D3DCompile(kDenoise, std::strlen(kDenoise), "denoise", nullptr, nullptr, "PSComposite", "ps_5_1", cf, 0, &ps, &err2))) {
            vox::log::Error("DX12: PSComposite compile: {}", err2 ? static_cast<const char*>(err2->GetBufferPointer()) : "?");
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psod{};
        psod.pRootSignature = d.compositeRootSig.Get();
        psod.VS = {fsqVs->GetBufferPointer(), fsqVs->GetBufferSize()};
        psod.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        psod.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psod.SampleMask = UINT_MAX;
        psod.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psod.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psod.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psod.NumRenderTargets = 1;
        psod.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;   // writes the backbuffer
        psod.SampleDesc.Count = 1;
        if (FAILED(d.device->CreateGraphicsPipelineState(&psod, IID_PPV_ARGS(&d.compositePso)))) {
            vox::log::Error("DX12: composite PSO"); return false;
        }
    }

    // ===================== A1: OBB-raster G-buffer pass =====================
    // A full-res depth buffer (+ DSV) and a cube PSO (VSObb + PSObb) that draws the world
    // object's OBB backfaces-only with depth test+write into the SAME 5 MRT G-buffer the
    // full-screen SHADE pass writes. Replaces the full-screen SHADE draw in the deferred
    // path; the a-trous + composite passes downstream are unchanged.

    // --- full-res D32 depth buffer + a one-entry DSV heap ---
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = W;
        rd.Height = H;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_D32_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1.0f;
        if (FAILED(d.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                     IID_PPV_ARGS(&d.depthTex)))) {
            vox::log::Error("DX12: OBB depth buffer"); return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(d.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&d.dsvHeap)))) return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        d.device->CreateDepthStencilView(d.depthTex.Get(), &dv,
                                         d.dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // --- OBB root signature = the SHADE layout (b0; t0-t3; u0-u7) + a b1 OBB CBV ---
    {
        D3D12_ROOT_PARAMETER rp[14]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[0].Descriptor.ShaderRegister = 0;  // b0 Camera
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[1].Descriptor.ShaderRegister = 0;  // t0 Voxels
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[2].Descriptor.ShaderRegister = 0;  // u0 Accum
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[3].Descriptor.ShaderRegister = 1;  // t1 BlueNoise
        rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[4].Descriptor.ShaderRegister = 2;  // t2 Palette
        rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[5].Descriptor.ShaderRegister = 3;  // t3 Brick
        rp[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[6].Descriptor.ShaderRegister = 1;  // u1 HitPos
        rp[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[7].Descriptor.ShaderRegister = 2;  // u2 PrevAccum
        rp[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[8].Descriptor.ShaderRegister = 3;  // u3 PrevHitPos
        rp[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[9].Descriptor.ShaderRegister = 4;  // u4 Moments
        rp[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[10].Descriptor.ShaderRegister = 5; // u5 PrevMoments
        rp[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[11].Descriptor.ShaderRegister = 6; // u6 DirectAccum
        rp[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[12].Descriptor.ShaderRegister = 7; // u7 PrevDirectAccum
        rp[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[13].Descriptor.ShaderRegister = 1; // b1 ObbObject
        for (auto& p : rp) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 14; rs.pParameters = rp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
            vox::log::Error("DX12: OBB root sig: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
        if (FAILED(d.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&d.obbRootSig)))) return false;

        ComPtr<ID3DBlob> vs, ps, err2;
        if (FAILED(D3DCompile(kVox, std::strlen(kVox), "voxel", nullptr, nullptr, "VSObb", "vs_5_1", cf, 0, &vs, &err2))) {
            vox::log::Error("DX12: VSObb compile: {}", err2 ? static_cast<const char*>(err2->GetBufferPointer()) : "?");
            return false;
        }
        if (FAILED(D3DCompile(kVox, std::strlen(kVox), "voxel", nullptr, nullptr, "PSObb", "ps_5_1", cf, 0, &ps, &err2))) {
            vox::log::Error("DX12: PSObb compile: {}", err2 ? static_cast<const char*>(err2->GetBufferPointer()) : "?");
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psod{};
        psod.pRootSignature = d.obbRootSig.Get();
        psod.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        psod.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        for (int i = 0; i < 5; ++i) psod.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psod.SampleMask = UINT_MAX;
        psod.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        // BACKFACES ONLY: cull the camera-facing (near) faces so the far faces always
        // cover the screen, even with the camera inside the box. The cube is wound OUTWARD
        // CCW, so FrontCounterClockwise=TRUE makes the near (outside-visible) faces "front"
        // and CULL_FRONT removes them, leaving the far faces.
        psod.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        psod.RasterizerState.FrontCounterClockwise = TRUE;
        psod.RasterizerState.DepthClipEnable = TRUE;
        psod.DepthStencilState.DepthEnable = TRUE;
        psod.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psod.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psod.DepthStencilState.StencilEnable = FALSE;
        psod.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psod.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psod.NumRenderTargets = 5;
        for (int i = 0; i < 5; ++i) psod.RTVFormats[i] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psod.SampleDesc.Count = 1;
        if (FAILED(d.device->CreateGraphicsPipelineState(&psod, IID_PPV_ARGS(&d.obbPso)))) {
            vox::log::Error("DX12: OBB PSO"); return false;
        }
    }

    d.denoiseReady = true;
    vox::log::Info("DX12: GI denoiser ready (a-trous + SVGF, {}x{})", W, H);
    return true;
}

bool Renderer::Init(void* hwndPtr, int width, int height, const std::vector<std::uint32_t>* voxels,
                    const std::uint32_t* palette256) {
    HWND hwnd = static_cast<HWND>(hwndPtr);
    if (!hwnd) return false;
    Impl& d = *impl_;
    d.width = static_cast<UINT>(width);
    d.height = static_cast<UINT>(height);

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { vox::log::Error("DX12: factory"); return false; }
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d.device))) &&
        FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.device)))) {
        vox::log::Error("DX12: no device (RT-capable GPU required)");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(d.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&d.queue)))) return false;

    BOOL allowTearing = FALSE;
    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(factory.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
    d.tearing = allowTearing != 0;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = d.width;
    sd.Height = d.height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = Impl::kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = d.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(d.queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) { vox::log::Error("DX12: swapchain"); return false; }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(sc1.As(&d.swap))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = Impl::kFrames;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(d.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&d.rtvHeap)))) return false;
    d.rtvSize = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < Impl::kFrames; ++i) {
        if (FAILED(d.swap->GetBuffer(i, IID_PPV_ARGS(&d.targets[i])))) return false;
        d.device->CreateRenderTargetView(d.targets[i].Get(), nullptr, h);
        h.ptr += d.rtvSize;
    }
    if (FAILED(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d.alloc)))) return false;
    if (FAILED(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.alloc.Get(), nullptr, IID_PPV_ARGS(&d.list)))) return false;
    d.list->Close();
    if (FAILED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence)))) return false;
    d.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // --- root signature ---
    // CBV(b0) + SRV(t0 Voxels) + UAV(u0 Accum) + SRV(t1 BlueNoise) + SRV(t2 Palette)
    // + SRV(t3 BrickOccupancy) + UAV(u1 HitPos) + UAV(u2 PrevAccum) + UAV(u3 PrevHitPos).
    // u0/u1 are the CURRENT ping-pong buffers (written), u2/u3 the PREVIOUS (read for
    // temporal reprojection). The binding addresses are swapped each frame in RenderFrame.
    D3D12_ROOT_PARAMETER rp[9]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[0].Descriptor.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[1].Descriptor.ShaderRegister = 0;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[2].Descriptor.ShaderRegister = 0;   // u0 Accum (current)
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[3].Descriptor.ShaderRegister = 1;   // t1
    rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[4].Descriptor.ShaderRegister = 2;   // t2
    rp[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[5].Descriptor.ShaderRegister = 3;   // t3 BrickOccupancy
    rp[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[6].Descriptor.ShaderRegister = 1;   // u1 HitPos (current)
    rp[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[7].Descriptor.ShaderRegister = 2;   // u2 PrevAccum (previous)
    rp[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[8].Descriptor.ShaderRegister = 3;   // u3 PrevHitPos (previous)
    rp[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 9;
    rs.pParameters = rp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rsBlob, rsErr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr))) {
        vox::log::Error("DX12: root sig serialize");
        return false;
    }
    if (FAILED(d.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&d.rootSig)))) return false;

    // --- shaders ---
    UINT cf = 0;
#if defined(_DEBUG)
    cf = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> vs, ps, err;
    if (FAILED(D3DCompile(kShader, std::strlen(kShader), "voxel", nullptr, nullptr, "VSMain", "vs_5_1", cf, 0, &vs, &err))) {
        vox::log::Error("DX12: VS compile: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    if (FAILED(D3DCompile(kShader, std::strlen(kShader), "voxel", nullptr, nullptr, "PSMain", "ps_5_1", cf, 0, &ps, &err))) {
        vox::log::Error("DX12: PS compile: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }

    // --- PSO ---
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = d.rootSig.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    if (FAILED(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&d.pso)))) {
        vox::log::Error("DX12: PSO");
        return false;
    }

    // --- voxel buffer (upload heap StructuredBuffer) ---
    // Use an externally supplied grid (imported .vox) when it matches kGrid^3;
    // otherwise fall back to the built-in procedural demo scene.
    const std::size_t voxNeed = static_cast<std::size_t>(kGrid) * kGrid * kGrid;
    std::vector<std::uint32_t> scene = (voxels && voxels->size() == voxNeed) ? *voxels : GenerateScene(kGrid);
    d.voxelBuf = d.MakeUpload(scene.size() * sizeof(std::uint32_t));
    if (!d.voxelBuf) return false;
    D3D12_RANGE none{0, 0};  // read range "nothing" — used for all persistently-mapped upload buffers
    {
        void* p = nullptr;
        d.voxelBuf->Map(0, &none, &p);
        d.voxelPtr = static_cast<std::uint32_t*>(p);  // keep persistently mapped
        std::memcpy(d.voxelPtr, scene.data(), scene.size() * sizeof(std::uint32_t));
        // Do NOT Unmap — kept open for hot SetVoxels updates.
    }

    // --- brick occupancy buffer (empty-space-skip acceleration, StructuredBuffer<uint> t3) ---
    // Built from the same dense grid; persistently mapped and rebuilt in SetVoxels.
    {
        std::vector<std::uint32_t> occ = BuildBrickOccupancy(scene, kGrid);
        d.brickBuf = d.MakeUpload(static_cast<UINT64>(kBrickCount) * sizeof(std::uint32_t));
        if (!d.brickBuf) return false;
        void* bp = nullptr;
        d.brickBuf->Map(0, &none, &bp);
        d.brickPtr = static_cast<std::uint32_t*>(bp);  // keep persistently mapped
        std::memcpy(d.brickPtr, occ.data(), static_cast<std::size_t>(kBrickCount) * sizeof(std::uint32_t));
        // Do NOT Unmap — kept open for hot SetVoxels updates.
    }

    // --- camera CBV (persistently mapped) ---
    d.camBuf = d.MakeUpload(256);
    if (!d.camBuf) return false;
    d.camBuf->Map(0, &none, reinterpret_cast<void**>(&d.camPtr));

    // --- A1 OBB-object CBV (b1, persistently mapped, 256-byte aligned) ---
    d.obbBuf = d.MakeUpload(256);
    if (!d.obbBuf) return false;
    d.obbBuf->Map(0, &none, reinterpret_cast<void**>(&d.obbPtr));

    // --- A2 TEST OBJECT: a small procedural voxel cube + its own palette + a 2nd
    // ObbObject (b1) CB + a 2nd Camera (b0) CB. All built up front and INERT until
    // FrameParams::test_object is set (and the deferred OBB path is active). The test
    // voxel/palette buffers REBIND t0/t2 for the test draw; the 2nd camera CB carries
    // gridDim = kTestDim and emptySkip = 0 so the shared DDA marches the 16^3 test grid
    // (and never touches the world's brick-occupancy buffer left bound on t3). ---
    {
        std::vector<std::uint32_t> cube = GenerateTestCube(Impl::kTestDim);
        d.testVoxelBuf = d.MakeUpload(cube.size() * sizeof(std::uint32_t));
        if (!d.testVoxelBuf) return false;
        void* tp = nullptr;
        d.testVoxelBuf->Map(0, &none, &tp);
        std::memcpy(tp, cube.data(), cube.size() * sizeof(std::uint32_t));
        d.testVoxelBuf->Unmap(0, nullptr);  // static grid: no live edits needed

        // Test-cube palette (256 RGBA8, linear-encoded like the world palette). Indices
        // 1..4 are vivid primaries so the rotating cube's octants are clearly distinct.
        auto packL = [](float r, float g, float b) -> std::uint32_t {
            auto enc = [](float c) -> std::uint32_t {
                float s = std::pow(std::max(0.0f, std::min(1.0f, c)), 1.0f / 2.2f);
                return static_cast<std::uint32_t>(s * 255.0f + 0.5f) & 255u;
            };
            return enc(r) | (enc(g) << 8) | (enc(b) << 16);  // alpha 0 = non-emissive
        };
        std::uint32_t tpal[256] = {};
        tpal[1] = packL(0.90f, 0.18f, 0.16f);  // red
        tpal[2] = packL(0.16f, 0.75f, 0.25f);  // green
        tpal[3] = packL(0.18f, 0.42f, 0.95f);  // blue
        tpal[4] = packL(0.95f, 0.82f, 0.16f);  // yellow
        d.testPaletteBuf = d.MakeUpload(256 * sizeof(std::uint32_t));
        if (!d.testPaletteBuf) return false;
        void* pp = nullptr;
        d.testPaletteBuf->Map(0, &none, &pp);
        std::memcpy(pp, tpal, sizeof(tpal));
        d.testPaletteBuf->Unmap(0, nullptr);
    }
    d.testObbBuf = d.MakeUpload(256);
    if (!d.testObbBuf) return false;
    d.testObbBuf->Map(0, &none, reinterpret_cast<void**>(&d.testObbPtr));
    d.testCamBuf = d.MakeUpload(256);
    if (!d.testCamBuf) return false;
    d.testCamBuf->Map(0, &none, reinterpret_cast<void**>(&d.testCamPtr));

    // --- GI temporal buffers: PING-PONG accumulation + hit-position (default-heap
    // UAVs, float4 per pixel each). Two of each so we can read the previous frame's
    // Accum/HitPos (for temporal reprojection) while writing the current frame's. ---
    {
        const UINT64 pixBytes = static_cast<UINT64>(d.width) * d.height * 16;  // float4 per pixel
        for (int i = 0; i < 2; ++i) {
            d.accumBuf[i]       = d.MakeDefaultUAV(pixBytes);
            d.hitPosBuf[i]      = d.MakeDefaultUAV(pixBytes);
            d.directAccumBuf[i] = d.MakeDefaultUAV(pixBytes);  // DIRECT-term ping-pong (multi-pass SHADE u6/u7)
            if (!d.accumBuf[i] || !d.hitPosBuf[i] || !d.directAccumBuf[i]) return false;
        }
    }

    // --- Multi-pass GI DENOISER resources (allocated up front; only USED when
    // gi_denoiser != 0 in QUALITY mode). Builds: 6 offscreen RGBA16F targets + their
    // RTVs and SRVs, the moment ping-pong buffers, and three root-sig/PSO triples
    // (SHADE / a-trous / composite). On any failure we leave denoiseReady=false so the
    // renderer still runs the single pass; the multi-pass branch checks the flag. ---
    if (!d.InitDenoiser(kShader, kDenoiseShader)) {
        vox::log::Error("DX12: GI denoiser init failed; multi-pass modes disabled (single pass still works)");
        d.denoiseReady = false;
    }

    // --- Blue-noise tile: 64x64 floats generated via void-and-cluster on the CPU ---
    // Reference: Ulichney 1993, "The void-and-cluster method for dither array generation."
    // We use the classic iterative swap-based approximation:
    //   1. Seed random binary pattern at target density ~10%.
    //   2. Repeatedly find the tightest cluster and the largest void; swap them.
    //      Repeat until stable.
    //   3. Rank the 4096 entries by insertion order -> uniform float in [0,1).
    //
    // Cache HIT:  load from disk, upload synchronously, no thread.
    // Cache MISS: fill blueNoiseBuf with a cheap hash-based fallback so rendering works
    //             from frame 1, then spawn a background thread that runs the full
    //             void-and-cluster bake, writes the disk cache, and sets bnReady.
    //             RenderFrame swaps the real tile in once the thread is done.
    {
        constexpr int BN = 64;
        constexpr int BN2 = BN * BN;

        // Allocate blueNoiseBuf as a persistently-mapped upload buffer so we can
        // re-upload the real tile later without re-mapping.
        d.blueNoiseBuf = d.MakeUpload(static_cast<UINT64>(BN2) * sizeof(float));
        if (!d.blueNoiseBuf) return false;
        d.blueNoiseBuf->Map(0, &none, reinterpret_cast<void**>(&d.bnPtr));

        // Bake the void-and-cluster tile at most once per machine; cache it in the
        // user data dir so later boots load instantly (generation costs a few seconds).
        const std::filesystem::path cachePath =
            std::filesystem::path(vox::platform::UserDataDir()) / "bluenoise64.bin";
        bool loaded = false;
        {
            std::vector<float> bn(BN2);
            std::ifstream cacheIn(cachePath, std::ios::binary);
            if (cacheIn) {
                cacheIn.read(reinterpret_cast<char*>(bn.data()), std::streamsize(BN2 * sizeof(float)));
                if (cacheIn.gcount() == std::streamsize(BN2 * sizeof(float))) {
                    loaded = true;
                    vox::log::Info("DX12: loaded cached blue-noise tile");
                    std::memcpy(d.bnPtr, bn.data(), static_cast<UINT64>(BN2) * sizeof(float));
                }
            }
        }

        if (!loaded) {
            // Cache MISS: write a cheap hash-based white-noise fallback into the
            // buffer so the GPU has valid data from the very first frame.
            // Using a simple integer hash (Wang hash variant) to produce floats in [0,1).
            {
                float* dst = d.bnPtr;
                for (int i = 0; i < BN2; ++i) {
                    std::uint32_t hv = static_cast<std::uint32_t>(i);
                    hv = (hv ^ 61u) ^ (hv >> 16u);
                    hv *= 9u;
                    hv ^= hv >> 4u;
                    hv *= 0x27D4EB2Du;
                    hv ^= hv >> 15u;
                    dst[i] = (hv & 0xFFFFu) / 65536.0f;
                }
            }

            vox::log::Info("DX12: baking blue-noise tile in background (one-time)...");

            // Spawn background thread: runs the full void-and-cluster bake, writes
            // disk cache, then signals bnReady so RenderFrame can swap the real tile in.
            d.bnTile.resize(BN2);
            d.bnThread = std::thread([&impl = *impl_, cachePath]() {
                constexpr int BN  = 64;
                constexpr int BN2 = BN * BN;

                // Gaussian kernel energy (wrapping toroidal distance, sigma≈1.58).
                auto energy = [&](const std::vector<int>& pat, int cx, int cy) -> float {
                    float e = 0.0f;
                    for (int dy = -7; dy <= 7; ++dy) {
                        for (int dx = -7; dx <= 7; ++dx) {
                            int nx = (cx + dx + BN) & (BN - 1);
                            int ny = (cy + dy + BN) & (BN - 1);
                            if (pat[ny * BN + nx]) {
                                float d2 = float(dx * dx + dy * dy);
                                e += std::exp(-d2 * 0.4f);
                            }
                        }
                    }
                    return e;
                };

                // Seed: deterministic LCG to avoid rand() platform drift.
                std::vector<int> pat(BN2, 0);
                std::uint32_t lcg = 0xA5F1C3B7u;
                for (int i = 0; i < BN2; ++i) {
                    lcg = lcg * 1664525u + 1013904223u;
                    if ((lcg >> 24) < 26u) pat[i] = 1;   // ~10 % density
                }

                // Iterate void-and-cluster swaps until convergence (max 200 rounds).
                for (int iter = 0; iter < 200; ++iter) {
                    int clusterIdx = -1; float maxE = -1.0f;
                    for (int i = 0; i < BN2; ++i) {
                        if (pat[i]) { float e = energy(pat, i % BN, i / BN); if (e > maxE) { maxE = e; clusterIdx = i; } }
                    }
                    int voidIdx = -1; float minE = 1e30f;
                    for (int i = 0; i < BN2; ++i) {
                        if (!pat[i]) { float e = energy(pat, i % BN, i / BN); if (e < minE) { minE = e; voidIdx = i; } }
                    }
                    if (voidIdx < 0 || clusterIdx < 0 || clusterIdx == voidIdx) break;
                    pat[clusterIdx] = 0;
                    pat[voidIdx]    = 1;
                }

                // Rank to produce a uniform float tile.
                // Phase 1: remove ones in cluster order.
                std::vector<int> rank(BN2, -1);
                std::vector<int> cur = pat;
                int ones = 0; for (auto v : cur) ones += v;
                for (int r = ones - 1; r >= 0; --r) {
                    int ci = -1; float maxE = -1.0f;
                    for (int i = 0; i < BN2; ++i) {
                        if (cur[i]) { float e = energy(cur, i % BN, i / BN); if (e > maxE) { maxE = e; ci = i; } }
                    }
                    if (ci < 0) break;
                    rank[ci] = r; cur[ci] = 0;
                }
                // Phase 2: fill zeros in void order.
                cur = pat;
                for (int r = ones; r < BN2; ++r) {
                    int vi = -1; float minE = 1e30f;
                    for (int i = 0; i < BN2; ++i) {
                        if (!cur[i]) { float e = energy(cur, i % BN, i / BN); if (e < minE) { minE = e; vi = i; } }
                    }
                    if (vi < 0) break;
                    rank[vi] = r; cur[vi] = 1;
                }

                for (int i = 0; i < BN2; ++i)
                    impl.bnTile[i] = (rank[i] >= 0) ? (float(rank[i]) + 0.5f) / float(BN2) : 0.0f;

                // Write disk cache.
                std::error_code mkec;
                std::filesystem::create_directories(cachePath.parent_path(), mkec);
                std::ofstream of(cachePath, std::ios::binary);
                if (of) of.write(reinterpret_cast<const char*>(impl.bnTile.data()),
                                 std::streamsize(BN2 * sizeof(float)));

                // Signal main thread that the tile is ready.
                impl.bnReady.store(true, std::memory_order_release);
            });
        }
    }

    // --- Palette buffer (256 RGBA8 uints, StructuredBuffer<uint> t2) ---
    // If no external palette supplied, build the default that reproduces the
    // old hardcoded linear colors: encode linear->sRGB so the shader's pow(c,2.2)
    // round-trips back to the original linear value.
    {
        std::uint32_t pal[256]{};

        // Helper: pack linear float3 as RGBA8 sRGB (R|G<<8|B<<16|A<<24).
        auto packLinear = [](float r, float g, float b) -> std::uint32_t {
            auto to8 = [](float v) -> std::uint32_t {
                float s = std::pow(std::max(v, 0.0f), 1.0f / 2.2f);
                return static_cast<std::uint32_t>(std::round(s * 255.0f));
            };
            return to8(r) | (to8(g) << 8) | (to8(b) << 16) | (0u << 24);  // alpha byte = emission (0 = none)
        };

        if (palette256) {
            std::memcpy(pal, palette256, 256 * sizeof(std::uint32_t));
        } else {
            // Default palette: mirror old hardcoded palette() colors at indices 1-4.
            std::uint32_t gray = packLinear(0.7f, 0.7f, 0.7f);
            for (int i = 0; i < 256; ++i) pal[i] = gray;
            pal[1] = packLinear(0.42f, 0.66f, 0.30f);  // grass
            pal[2] = packLinear(0.48f, 0.36f, 0.26f);  // dirt
            pal[3] = packLinear(0.55f, 0.56f, 0.60f);  // stone
            pal[4] = packLinear(0.82f, 0.30f, 0.24f);  // sphere
        }

        d.paletteBuf = d.MakeUpload(256 * sizeof(std::uint32_t));
        if (!d.paletteBuf) return false;
        {
            void* pp = nullptr;
            d.paletteBuf->Map(0, &none, &pp);
            d.palettePtr = static_cast<std::uint32_t*>(pp);  // keep persistently mapped
            std::memcpy(d.palettePtr, pal, 256 * sizeof(std::uint32_t));
            // Do NOT Unmap — kept open for hot SetVoxels updates.
        }
    }

    valid_ = true;
    vox::log::Info("DX12: voxel raymarcher ready ({}^3 grid, {}x{}, tearing={})", kGrid, width, height, d.tearing);
    return true;
}

void Renderer::RenderFrame(const FrameParams& fp) {
    if (!valid_) return;
    Impl& d = *impl_;

    // Async blue-noise swap: if the background bake finished and we haven't yet
    // copied the real tile, do a one-time WaitIdle + memcpy + thread join.
    if (!d.bnUploaded && d.bnReady.load(std::memory_order_acquire)) {
        constexpr int BN2 = 64 * 64;
        d.WaitIdle();   // ensure GPU is not mid-read of blueNoiseBuf
        std::memcpy(d.bnPtr, d.bnTile.data(), static_cast<UINT64>(BN2) * sizeof(float));
        d.bnUploaded = true;
        if (d.bnThread.joinable()) d.bnThread.join();
        d.bnTile.clear();
        d.bnTile.shrink_to_fit();
        vox::log::Info("DX12: blue-noise tile ready (async)");
    }

    // camera basis from yaw/pitch
    float cp = std::cos(fp.cam_pitch), sp = std::sin(fp.cam_pitch);
    float cy = std::cos(fp.cam_yaw), sy = std::sin(fp.cam_yaw);
    float fwd[3] = {cp * sy, sp, cp * cy};
    float wup[3] = {0, 1, 0};
    float right[3];
    Cross(fwd, wup, right);
    Norm(right);
    float up[3];
    Cross(right, fwd, up);

    CamCB cb{};
    for (int i = 0; i < 3; ++i) {
        cb.camPos[i] = fp.cam_pos[i];
        cb.camFwd[i] = fwd[i];
        cb.camRight[i] = right[i];
        cb.camUp[i] = up[i];
        cb.clearCol[i] = fp.clear[i];
        cb.sunDir[i] = fp.sun[i];
    }
    cb.fov = fp.cam_fov;
    cb.timeSec = fp.time_sec;
    cb.aspect = d.height ? float(d.width) / float(d.height) : 1.0f;
    cb.gridDim = static_cast<int>(kGrid);
    cb.exposure = fp.exposure;
    cb.hdr = fp.hdr;
    cb.ambient = fp.ambient;
    cb.viewport[0] = float(d.width);
    cb.viewport[1] = float(d.height);
    cb.shadowSoftness = fp.shadow_softness;
    cb.aoStrength     = fp.ao_strength;
    cb.aoRadius       = fp.ao_radius;
    cb.lightingMode   = fp.lighting_mode;
    cb.dither         = fp.dither;
    cb.aoSamples      = std::max(1, std::min(fp.ao_samples,     32));
    cb.shadowSamples  = std::max(1, std::min(fp.shadow_samples, 16));
    cb.giSamples      = std::max(1, std::min(fp.gi_samples,      8));
    cb.giBounces      = std::max(0, std::min(fp.gi_bounces,      5));
    cb.giEmissive     = std::max(0.0f, std::min(fp.gi_emissive, 8.0f));
    cb.voxEmissive    = std::max(0.0f, fp.vox_emissive);   // host sets >1 only when a .vox map is loaded
    cb.emissiveSurface = std::max(0.0f, fp.emissive_surface);  // dims the emitter's OWN surface glow (not its GI contribution)
    cb.giIntensity    = std::max(0.0f, fp.gi_intensity);   // indirect-strength dial (>1 brightens dim rooms)
    cb.reproject      = d.giReproject ? 1 : 0;

    // --- Build this frame's view-projection (row-major) for temporal reprojection.
    // It is the exact algebraic inverse of the shader's primary-ray generation, so
    // projecting a world point P gives the screen pixel whose ray hits P:
    //   xv = (P-cam).right, yv = (P-cam).up, zv = (P-cam).fwd  (orthonormal basis)
    //   clip = (xv/(tf*aspect), yv/tf, zv, zv);  ndc = clip.xy/clip.w
    //   uv = (ndc.x*0.5+0.5, -ndc.y*0.5+0.5)   (matches PSMain's ndc.y = -ndc.y flip)
    // Rows 0/1 are the right/up basis scaled by the inverse FOV terms, rows 2/3 are
    // the forward basis (depth + perspective w). The shader does mul(prevVP, P).
    {
        const float tf      = std::tan(fp.cam_fov * 0.5f);
        const float aspect  = cb.aspect;
        const float invX    = 1.0f / std::max(tf * aspect, 1e-6f);
        const float invY    = 1.0f / std::max(tf, 1e-6f);
        const float* e      = fp.cam_pos;
        const float dotR    = right[0] * e[0] + right[1] * e[1] + right[2] * e[2];
        const float dotU    = up[0]    * e[0] + up[1]    * e[1] + up[2]    * e[2];
        const float dotF    = fwd[0]   * e[0] + fwd[1]   * e[1] + fwd[2]   * e[2];
        float curVP[16] = {
            right[0] * invX, right[1] * invX, right[2] * invX, -dotR * invX,  // row 0 -> clip.x
            up[0]    * invY, up[1]    * invY, up[2]    * invY, -dotU * invY,  // row 1 -> clip.y
            fwd[0],          fwd[1],          fwd[2],          -dotF,         // row 2 -> clip.z (depth)
            fwd[0],          fwd[1],          fwd[2],          -dotF,         // row 3 -> clip.w (== zv)
        };
        // Upload the PREVIOUS frame's VP (first frame: use current; accumFrame==0
        // gates reprojection anyway, so the self-projection is harmless).
        const float* vpSrc = d.havePrevVP ? d.prevVP : curVP;
        std::memcpy(cb.prevVP, vpSrc, sizeof(cb.prevVP));
        // Remember this frame's VP for next frame's reprojection.
        std::memcpy(d.prevVP, curVP, sizeof(d.prevVP));
        d.havePrevVP = true;

        // --- A1: per-OBB-object constants for the deferred G-buffer pass ---
        // The OBB cube needs a world->clip matrix with a REAL perspective-z (curVP's z is
        // degenerate -- z==w -> NDC z==1 -- which is fine for reprojection but useless as a
        // depth buffer). Reuse curVP's rows 0/1/3 (so the cube projects to EXACTLY the
        // screen positions the PS reconstructs its rays for) and replace row 2 with a
        // standard [0,1] perspective-depth row: clip.z = A*zv + B, zv = fwd.P - dotF.
        // For the single world object the model matrix is identity, so obbMVP == this
        // world->clip matrix and obbInvModel == identity (world space == voxel space).
        {
            const float zn = 0.1f, zf = 2048.0f;     // near/far bracketing the [0,128]^3 scene
            const float A = zf / (zf - zn);
            const float B = -zn * zf / (zf - zn);
            ObbCB ob{};
            const float mvp[16] = {
                right[0] * invX, right[1] * invX, right[2] * invX, -dotR * invX,        // clip.x
                up[0]    * invY, up[1]    * invY, up[2]    * invY, -dotU * invY,        // clip.y
                A * fwd[0],      A * fwd[1],      A * fwd[2],       A * (-dotF) + B,     // clip.z (perspective depth)
                fwd[0],          fwd[1],          fwd[2],          -dotF,               // clip.w (== zv)
            };
            std::memcpy(ob.mvp, mvp, sizeof(mvp));
            // invModel = identity (object #0: object-local voxel space IS world space).
            ob.invModel[0] = ob.invModel[5] = ob.invModel[10] = ob.invModel[15] = 1.0f;
            ob.dims[0] = ob.dims[1] = ob.dims[2] = static_cast<float>(kGrid);
            ob.skyOnMiss = 1;   // enclosing world object: a DDA miss writes the sky (pixel-identical to A0)
            std::memcpy(d.obbPtr, &ob, sizeof(ob));

            // --- A2: SECOND object (rotating test cube) per-OBB constants ---------------
            // Same world->clip rows as the world object (so the cube projects to exactly the
            // screen positions PSObb reconstructs rays for + a real perspective-depth row),
            // but with a NON-identity model: rotate about +Y by time_sec around the cube's
            // own center, then place it in the world. The hardware depth buffer (written via
            // obbMVP*localCube) composites it against the world object (skyOnMiss=0 => DISCARD
            // on a DDA miss, so only the cube's solid voxels draw). Built every frame so the
            // rotation animates; uploaded into testObbBuf. Consumed only when test_object is on.
            {
                const float worldClip[16] = {
                    right[0] * invX, right[1] * invX, right[2] * invX, -dotR * invX,    // clip.x
                    up[0]    * invY, up[1]    * invY, up[2]    * invY, -dotU * invY,    // clip.y
                    A * fwd[0],      A * fwd[1],      A * fwd[2],       A * (-dotF) + B, // clip.z (perspective depth)
                    fwd[0],          fwd[1],          fwd[2],          -dotF,           // clip.w (== zv)
                };
                const float td = static_cast<float>(Impl::kTestDim);   // cube spans local [0,td]^3
                const float c  = td * 0.5f;                            // cube center (local)
                const float th = fp.time_sec;                          // Y-rotation angle (rad)
                const float ct = std::cos(th), st = std::sin(th);
                // Place the cube center near the world center, floating clearly above terrain.
                const float Px = static_cast<float>(kGrid) * 0.5f;
                const float Py = static_cast<float>(kGrid) * 0.40f;
                const float Pz = static_cast<float>(kGrid) * 0.5f;
                // model M (row-major): linear = R_y(th); translation column = P - R_y*c.
                //   M*p = R_y*(p - c) + P  (rotate about the cube's own center, then place)
                const float M[16] = {
                    ct,   0.0f, st,   Px - (ct * c + st * c),
                    0.0f, 1.0f, 0.0f, Py - c,
                   -st,   0.0f, ct,   Pz - (-st * c + ct * c),
                    0.0f, 0.0f, 0.0f, 1.0f,
                };
                // invModel = M^-1 (row-major): R_y orthonormal => linear = R_y^T = R_y(-th);
                //   translation column = c - R_y^T*P.  invM*p' = R_y^T*(p' - P) + c
                const float invM[16] = {
                    ct,   0.0f, -st,  c - ( ct * Px - st * Pz),
                    0.0f, 1.0f, 0.0f, c - Py,
                    st,   0.0f, ct,   c - ( st * Px + ct * Pz),
                    0.0f, 0.0f, 0.0f, 1.0f,
                };
                // obbMVP = worldClip * M (row-major 4x4 multiply).
                auto mul4 = [](const float a[16], const float b[16], float o[16]) {
                    for (int r = 0; r < 4; ++r)
                        for (int col = 0; col < 4; ++col)
                            o[r * 4 + col] = a[r * 4 + 0] * b[0 * 4 + col] +
                                             a[r * 4 + 1] * b[1 * 4 + col] +
                                             a[r * 4 + 2] * b[2 * 4 + col] +
                                             a[r * 4 + 3] * b[3 * 4 + col];
                };
                ObbCB tb{};
                mul4(worldClip, M, tb.mvp);
                std::memcpy(tb.invModel, invM, sizeof(invM));
                tb.dims[0] = tb.dims[1] = tb.dims[2] = td;
                tb.skyOnMiss = 0;   // non-enclosing: DISCARD on a DDA miss so depth composites it over the world
                std::memcpy(d.testObbPtr, &tb, sizeof(tb));
                // The test object's 2nd Camera CB (b0) is uploaded LATER, once cb has its
                // accumFrame/giHistMax/emptySkip/etc. populated -- see after d.camPtr write.
            }
        }
    }

    // Temporal accumulation with motion-adaptive denoise.
    //
    // CAMERA key (pos/orientation/fov) vs SCENE key (sun/exposure/lighting/sample
    // counts/...). A SCENE change makes existing GI history wrong, so it forces a
    // hard reset (accumFrame=0 -> shader restarts the running average). A pure
    // CAMERA change keeps history: with denoise on we only SHORTEN the per-pixel
    // accumulation cap (giHistMax) so the shader's running average degrades into a
    // motion-adaptive exponential moving average (alpha ~= 1/(cap+1)) -- this is
    // what removes the 1-spp noise storm in motion. With denoise off, a camera
    // change hard-resets like the original (full noise storm, for A/B testing).
    const float camKey[Impl::kCamKeyLen] = {
        fp.cam_pos[0], fp.cam_pos[1], fp.cam_pos[2], fp.cam_yaw, fp.cam_pitch, fp.cam_fov,
    };
    const float sceneKey[Impl::kSceneKeyLen] = {
        fp.sun[0], fp.sun[1], fp.sun[2], fp.exposure, fp.ambient,
        fp.clear[0], fp.clear[1], fp.clear[2], fp.shadow_softness, fp.ao_strength, fp.ao_radius,
        static_cast<float>(fp.lighting_mode), static_cast<float>(fp.hdr),
        static_cast<float>(fp.dither), static_cast<float>(fp.ao_samples),
        static_cast<float>(fp.shadow_samples), static_cast<float>(fp.gi_samples),
        static_cast<float>(fp.gi_bounces), fp.gi_emissive, fp.vox_emissive, fp.emissive_surface, fp.gi_intensity,
        // gi_denoiser is part of the SCENE key because mode 0 accumulates FULL radiance
        // into Accum while modes 1/2 accumulate the DEMODULATED indirect -- different
        // quantities, so switching modes must hard-reset the running average (else the
        // EMA blends the two for a few frames). The atrous iters/phi knobs are NOT in
        // the key: they only affect the post-accumulation spatial filter, not what is
        // stored, so changing them needs no reset.
        static_cast<float>(fp.gi_denoiser),
    };
    // Compile-time guard: the stored copies (Impl::accCamKey/accSceneKey) MUST be
    // the same length as these keys, or the memcmp/memcpy below over-run.
    static_assert(sizeof(camKey)   == sizeof(d.accCamKey),   "camKey vs accCamKey length mismatch");
    static_assert(sizeof(sceneKey) == sizeof(d.accSceneKey), "sceneKey vs accSceneKey length mismatch");
    const bool sceneSame = d.accHave && std::memcmp(sceneKey, d.accSceneKey, sizeof(sceneKey)) == 0;
    const bool camSame   = d.accHave && std::memcmp(camKey,   d.accCamKey,   sizeof(camKey))   == 0;

    // Per-pixel sample-count cap fed to the shader. Still frames converge to a
    // clean image (large cap); moving frames clamp to a short window so history
    // is trusted just enough to kill noise without excessive lag/ghosting.
    constexpr float kHistMaxStill = 2048.0f;
    float histMax = kHistMaxStill;

    if (fp.lighting_mode != 1) {
        d.accumFrame = 0;                 // not QUALITY: nothing accumulates
    } else if (!sceneSame) {
        d.accumFrame = 0;                 // scene/lighting changed: history invalid -> hard reset
    } else if (camSame) {
        ++d.accumFrame;                   // fully still: keep converging (cap stays 2048)
    } else if (d.giReproject) {
        // Pure camera motion with REPROJECTION ON: history is realigned per-pixel in
        // the shader (current hit projected through the previous VP, matched to the
        // previous hit), so we KEEP history and keep advancing the sample sequence --
        // but we must NOT freeze the cap at 2048. Reprojection is NEAREST-pixel (a
        // sub-pixel snap error) and the disocclusion match isn't perfect, so a frozen
        // 2048-deep history lets that error ACCUMULATE frame-over-frame into visible
        // "swimming"/smearing that drags with the camera. Scale the cap by motion so
        // realignment error decays over a handful-to-dozens of frames (still FAR
        // cleaner than the reproject-OFF EMA below, which trusts ~6), then snap back to
        // the full still cap the instant motion stops (the camSame branch above).
        ++d.accumFrame;
        float dp = 0.0f;
        for (int i = 0; i < 3; ++i) { float e = fp.cam_pos[i] - d.accCamKey[i]; dp += e * e; }
        const float posDelta = std::sqrt(dp);                              // world voxels moved
        const float angDelta = std::fabs(fp.cam_yaw   - d.accCamKey[3]) +
                               std::fabs(fp.cam_pitch - d.accCamKey[4]) +
                               std::fabs(fp.cam_fov   - d.accCamKey[5]);   // radians
        const float motion = posDelta * 0.20f + angDelta * 2.0f;
        // Ceiling (how long realigned history is trusted) is user-tunable via
        // renderer.gi.reproject_history -- the swim<->grain knob: a gentle pan keeps
        // most of it, a fast swing decays toward a floor of 8. cap = ceil/(1+motion*4).
        // Higher = smoother but more swim/lag; lower = crisper but grainier. Still
        // frames ignore this entirely (the camSame branch keeps the full 2048 cap).
        const float histCeil = static_cast<float>(std::max(8, fp.gi_reproject_history));
        histMax = std::max(8.0f, histCeil / (1.0f + motion * 4.0f));
    } else if (d.giDenoise) {
        // Pure camera motion, reprojection OFF but denoise ON: screen-space EMA.
        // KEEP history, advance the sample sequence, and shorten the cap based on
        // motion magnitude. Larger motion -> smaller cap -> higher EMA alpha ->
        // less ghosting (but a touch noisier). This is the original behaviour.
        ++d.accumFrame;
        float dp = 0.0f;
        for (int i = 0; i < 3; ++i) { float e = fp.cam_pos[i] - d.accCamKey[i]; dp += e * e; }
        const float posDelta = std::sqrt(dp);                              // world voxels moved this frame
        const float angDelta = std::fabs(fp.cam_yaw   - d.accCamKey[3]) +
                               std::fabs(fp.cam_pitch - d.accCamKey[4]) +
                               std::fabs(fp.cam_fov   - d.accCamKey[5]);   // radians
        // Map motion -> blend alpha in [0.15, 0.45]; convert to a sample cap.
        // Scales chosen so a gentle nudge sits near 0.15 (heavy history, very
        // clean) and a fast swing approaches 0.45 (light history, low ghosting).
        const float motion = posDelta * 0.20f + angDelta * 2.0f;
        const float alpha  = std::min(0.45f, 0.15f + motion);
        histMax = (1.0f / alpha) - 1.0f;  // cap s.t. shader's 1/(n+1) == alpha
    } else {
        d.accumFrame = 0;                 // camera moved, reproject + denoise OFF: original hard reset
    }

    std::memcpy(d.accCamKey,   camKey,   sizeof(camKey));
    std::memcpy(d.accSceneKey, sceneKey, sizeof(sceneKey));
    d.accHave = true;
    cb.accumFrame = static_cast<int>(d.accumFrame);
    cb.giDenoise  = d.giDenoise ? 1 : 0;
    cb.giHistMax  = histMax;
    cb.giDebug    = d.giDebug ? 1 : 0;
    cb.emptySkip  = d.emptySkip ? 1 : 0;
    cb.brickDim   = static_cast<int>(kBrickDim);
    std::memcpy(d.camPtr, &cb, sizeof(cb));

    // A2 test object's 2nd Camera CB (b0): a full copy of the now-complete frame camera,
    // but with gridDim = the test-cube size and empty-skip OFF, so the SHARED voxel DDA
    // marches the rebound 16^3 test grid (t0/t2 rebound to the test buffers for the test
    // draw) and never reads the world's brick-occupancy buffer (left bound on t3). All
    // other fields (camera basis, lighting, accumFrame, prevVP, ...) match the world draw.
    {
        CamCB tc = cb;
        tc.gridDim   = static_cast<int>(Impl::kTestDim);
        tc.emptySkip = 0;
        std::memcpy(d.testCamPtr, &tc, sizeof(tc));
    }

    // Ping-pong selection: this frame WRITES the "cur" Accum/HitPos (u0/u1) and
    // READS last frame's "prev" Accum/HitPos (u2/u3). accumPair flips at the end so
    // the buffers written this frame become next frame's "prev". WaitIdle() at the
    // end of every frame guarantees the GPU finished the previous write before the
    // swap reuses it, so no explicit UAV barrier is needed across frames.
    const UINT curBuf  = d.accumPair;
    const UINT prevBuf = d.accumPair ^ 1u;

    // The multi-pass GI denoiser only applies in QUALITY mode (lighting_mode==1, the
    // only tier that computes path-traced indirect) AND when its resources built OK.
    // In every other case (mode 0, PERFORMANCE, or a denoiser-init failure) the
    // ORIGINAL single pass runs, completely unchanged -- this is the default.
    const bool multipass = d.denoiseReady && fp.gi_denoiser != 0 && fp.lighting_mode == 1;
    const int  svgf      = (fp.gi_denoiser == 2) ? 1 : 0;

    d.alloc->Reset();
    d.list->Reset(d.alloc.Get(), multipass ? d.shadePso.Get() : d.pso.Get());

    const UINT idx = d.swap->GetCurrentBackBufferIndex();
    D3D12_VIEWPORT vp{0, 0, float(d.width), float(d.height), 0, 1};
    D3D12_RECT sc{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
    d.list->RSSetViewports(1, &vp);
    d.list->RSSetScissorRects(1, &sc);

    if (!multipass) {
        // ============================ SINGLE PASS (mode 0 / PERFORMANCE) ============================
        // Unchanged from the original: one full-screen draw -> tonemapped backbuffer.
        D3D12_RESOURCE_BARRIER bar{};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource = d.targets[idx].Get();
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        d.list->ResourceBarrier(1, &bar);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(idx) * d.rtvSize;
        d.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        const float clear[4] = {fp.clear[0], fp.clear[1], fp.clear[2], 1.0f};
        d.list->ClearRenderTargetView(rtv, clear, 0, nullptr);

        d.list->SetGraphicsRootSignature(d.rootSig.Get());
        d.list->SetGraphicsRootConstantBufferView(0, d.camBuf->GetGPUVirtualAddress());
        d.list->SetGraphicsRootShaderResourceView(1, d.voxelBuf->GetGPUVirtualAddress());
        d.list->SetGraphicsRootUnorderedAccessView(2, d.accumBuf[curBuf]->GetGPUVirtualAddress());    // u0 Accum (current, write)
        d.list->SetGraphicsRootShaderResourceView(3, d.blueNoiseBuf->GetGPUVirtualAddress());
        d.list->SetGraphicsRootShaderResourceView(4, d.paletteBuf->GetGPUVirtualAddress());
        d.list->SetGraphicsRootShaderResourceView(5, d.brickBuf->GetGPUVirtualAddress());
        d.list->SetGraphicsRootUnorderedAccessView(6, d.hitPosBuf[curBuf]->GetGPUVirtualAddress());   // u1 HitPos (current, write)
        d.list->SetGraphicsRootUnorderedAccessView(7, d.accumBuf[prevBuf]->GetGPUVirtualAddress());   // u2 PrevAccum (previous, read)
        d.list->SetGraphicsRootUnorderedAccessView(8, d.hitPosBuf[prevBuf]->GetGPUVirtualAddress());  // u3 PrevHitPos (previous, read)
        d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d.list->DrawInstanced(3, 1, 0, 0);

        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        d.list->ResourceBarrier(1, &bar);
    } else {
        // ===================== DEFERRED PIPELINE (mode 1 ATROUS / 2 SVGF) =====================
        // The explicit two-pass split (Milestone A0): a G-BUFFER PASS that full-screen
        // raymarches the one global voxel grid and writes its results into offscreen
        // G-buffer targets (albedo, normal+linear-depth, demodulated indirect, direct,
        // SVGF variance), followed by a DEFERRED-LIGHTING PASS that reads that G-buffer
        // and produces the final image. Concretely:
        //   1) G-BUFFER PASS  : PSShade -> 5 MRT offscreen G-buffer targets.
        //   2) DEFERRED LIGHT : N edge-aware a-trous iterations on the demodulated
        //                       indirect (ping-pong) + COMPOSITE (re-modulate the
        //                       G-buffer albedo onto the filtered indirect, add the
        //                       direct term, tonemap + dither -> backbuffer).
        // (Mode 0 / PERFORMANCE still runs the original fused single pass above, kept
        // unchanged so its output is byte-identical; A1+ migrate it onto this path.)
        d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d.list->SetDescriptorHeaps(1, d.srvHeap.GetAddressOf());

        // Transition helper for the 6 offscreen targets (tracks d.texState[]).
        auto toState = [&](UINT slot, D3D12_RESOURCE_STATES after) {
            if (d.texState[slot] == after) return;
            ID3D12Resource* res[6] = { d.indirectTex[0].Get(), d.indirectTex[1].Get(), d.directTex.Get(),
                                       d.albedoTex.Get(), d.gbufTex.Get(), d.varianceTex.Get() };
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = res[slot];
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = d.texState[slot];
            b.Transition.StateAfter = after;
            d.list->ResourceBarrier(1, &b);
            d.texState[slot] = after;
        };
        constexpr D3D12_RESOURCE_STATES kRT  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        constexpr D3D12_RESOURCE_STATES kPSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // ---- 1) G-BUFFER PASS (A1 OBB raster): draw the world object's OBB backfaces-only
        //        with depth test+write, fragment-DDA the LOCAL grid, write the 5 MRT G-buffer
        //        targets (indirect[0], direct, albedo, gbuf=normal+depth, variance) ----
        // All 6 RTs start RENDER_TARGET (from the previous frame's restore / Init). The
        // depth buffer stays permanently in DEPTH_WRITE (never read as an SRV).
        for (int i = 0; i < 6; ++i) toState((UINT)i, kRT);
        D3D12_CPU_DESCRIPTOR_HANDLE shadeRtvs[5] = {
            d.OffRtv(0),  // SV_Target0 indirect[0]
            d.OffRtv(2),  // SV_Target1 direct
            d.OffRtv(3),  // SV_Target2 albedo
            d.OffRtv(4),  // SV_Target3 gbuf
            d.OffRtv(5),  // SV_Target4 variance
        };
        // A1 OBB-raster is GATED behind renderer.gbuffer.obb (fp.gbuffer_obb, default OFF).
        // OFF -> the PROVEN A0 full-screen PSShade G-buffer pass (shadePso is already bound by the
        // Reset that opened this list; shadeRootSig; NO depth; full-screen triangle). ON -> A1's
        // OBB cube raster with a depth buffer. Both write the identical 5 MRT G-buffer and share
        // roots 0-12 below; only this head (RT/depth/rootsig/PSO) and the b1+draw tail differ.
        if (fp.gbuffer_obb) {
            D3D12_CPU_DESCRIPTOR_HANDLE dsv = d.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            d.list->OMSetRenderTargets(5, shadeRtvs, FALSE, &dsv);
            d.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            d.list->SetGraphicsRootSignature(d.obbRootSig.Get());
            d.list->SetPipelineState(d.obbPso.Get());
        } else {
            d.list->OMSetRenderTargets(5, shadeRtvs, FALSE, nullptr);   // no DSV
            d.list->SetGraphicsRootSignature(d.shadeRootSig.Get());     // shadePso already bound via Reset()
        }
        d.list->SetGraphicsRootConstantBufferView(0, d.camBuf->GetGPUVirtualAddress());
        d.list->SetGraphicsRootShaderResourceView(1, d.voxelBuf->GetGPUVirtualAddress());
        d.list->SetGraphicsRootUnorderedAccessView(2, d.accumBuf[curBuf]->GetGPUVirtualAddress());    // u0 Accum
        d.list->SetGraphicsRootShaderResourceView(3, d.blueNoiseBuf->GetGPUVirtualAddress());
        d.list->SetGraphicsRootShaderResourceView(4, d.paletteBuf->GetGPUVirtualAddress());
        d.list->SetGraphicsRootShaderResourceView(5, d.brickBuf->GetGPUVirtualAddress());
        d.list->SetGraphicsRootUnorderedAccessView(6, d.hitPosBuf[curBuf]->GetGPUVirtualAddress());   // u1 HitPos
        d.list->SetGraphicsRootUnorderedAccessView(7, d.accumBuf[prevBuf]->GetGPUVirtualAddress());   // u2 PrevAccum
        d.list->SetGraphicsRootUnorderedAccessView(8, d.hitPosBuf[prevBuf]->GetGPUVirtualAddress());  // u3 PrevHitPos
        d.list->SetGraphicsRootUnorderedAccessView(9, d.momentBuf[curBuf]->GetGPUVirtualAddress());   // u4 Moments
        d.list->SetGraphicsRootUnorderedAccessView(10, d.momentBuf[prevBuf]->GetGPUVirtualAddress()); // u5 PrevMoments
        d.list->SetGraphicsRootUnorderedAccessView(11, d.directAccumBuf[curBuf]->GetGPUVirtualAddress());  // u6 DirectAccum (current, write)
        d.list->SetGraphicsRootUnorderedAccessView(12, d.directAccumBuf[prevBuf]->GetGPUVirtualAddress()); // u7 PrevDirectAccum (previous, read)
        if (fp.gbuffer_obb) {
            d.list->SetGraphicsRootConstantBufferView(13, d.obbBuf->GetGPUVirtualAddress());          // b1 ObbObject (model/invModel/dims/skyOnMiss)
            d.list->DrawInstanced(36, 1, 0, 0);   // A1: 36 verts = the unit cube (generated in VSObb)

            // ---- A2: SECOND object (rotating test cube), depth-composited over the world ----
            // Only when test_object is set (and we are in the deferred OBB path, the enclosing
            // branch). Reuse obbPso/obbRootSig/VSObb/PSObb verbatim; the DSV is NOT cleared, so
            // the cube's far-face depth test (LESS) composites it against the world object's
            // depth -- it correctly occludes / is occluded by world geometry. Rebind only what
            // differs from the world draw: b0 -> the test camera CB (gridDim=16, empty-skip off),
            // t0 -> the test voxel grid, t2 -> the test palette, b1 -> the rotating test ObbObject
            // CB (skyOnMiss=0 => DISCARD on a DDA miss so only the cube's solid voxels draw).
            // The world bindings are re-established at the top of the next frame's G-buffer pass.
            if (fp.test_object) {
                // Order the test draw's G-buffer UAV writes (u0..u7) after the world draw's so
                // overlapping pixels don't race. (On occluded pixels the cube's shader may still
                // run and write history before failing the MRT depth test -- an accepted transient
                // temporal artifact on this moving proof object; the composited image is correct
                // via the hardware depth buffer.)
                ID3D12Resource* uavs[4] = { d.accumBuf[curBuf].Get(), d.hitPosBuf[curBuf].Get(),
                                            d.momentBuf[curBuf].Get(), d.directAccumBuf[curBuf].Get() };
                D3D12_RESOURCE_BARRIER ubs[4]{};
                for (int b = 0; b < 4; ++b) {
                    ubs[b].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    ubs[b].UAV.pResource = uavs[b];
                }
                d.list->ResourceBarrier(4, ubs);   // accum/hitpos/moment/direct (the 4 G-buffer UAV buffers)
                d.list->SetGraphicsRootConstantBufferView(0, d.testCamBuf->GetGPUVirtualAddress());        // b0 test camera (gridDim=16)
                d.list->SetGraphicsRootShaderResourceView(1, d.testVoxelBuf->GetGPUVirtualAddress());      // t0 test grid
                d.list->SetGraphicsRootShaderResourceView(4, d.testPaletteBuf->GetGPUVirtualAddress());    // t2 test palette
                d.list->SetGraphicsRootConstantBufferView(13, d.testObbBuf->GetGPUVirtualAddress());       // b1 rotating test ObbObject
                d.list->DrawInstanced(36, 1, 0, 0);   // the test cube's 36-vertex OBB
            }
        } else {
            d.list->DrawInstanced(3, 1, 0, 0);    // A0: full-screen triangle (PSShade)
        }

        // ---- 2) DEFERRED LIGHTING (a-trous): edge-aware wavelet over the G-buffer's
        //        demodulated indirect, ping-pong indirect[src]->indirect[dst] ----
        // gbuf + variance are read every iteration; transition them to PSR once.
        toState(4, kPSR);  // gbuf
        toState(5, kPSR);  // variance
        // SVGF colour feedback ordering: SHADE wrote accumBuf[curBuf] (u0) above and
        // a-trous iteration 0 OVERWRITES the same buffer (the filtered indirect) via its
        // own u0. A UAV barrier makes iter-0's write strictly ordered AFTER SHADE's so
        // the filtered value -- not a late SHADE write -- becomes next frame's PrevAccum.
        // Iterations > 0 don't touch Accum, so one barrier here suffices. The buffer
        // stays in UNORDERED_ACCESS the whole time (no state transition needed).
        {
            D3D12_RESOURCE_BARRIER ub{};
            ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            ub.UAV.pResource = d.accumBuf[curBuf].Get();
            d.list->ResourceBarrier(1, &ub);
        }
        const int iters = std::max(1, std::min(fp.gi_atrous_iters, 6));
        UINT srcT = 0;     // SHADE wrote indirect[0]
        for (int it = 0; it < iters; ++it) {
            const UINT dstT = srcT ^ 1u;
            const UINT srcSlot = (srcT == 0) ? 0u : 1u;   // SRV heap slot for indirect[src]
            const UINT dstSlot = (dstT == 0) ? 0u : 1u;   // RTV index for indirect[dst]
            toState(srcSlot, kPSR);  // read source
            toState(dstSlot, kRT);   // write dest
            D3D12_CPU_DESCRIPTOR_HANDLE dstRtv = d.OffRtv(dstSlot);
            d.list->OMSetRenderTargets(1, &dstRtv, FALSE, nullptr);
            d.list->SetGraphicsRootSignature(d.atrousRootSig.Get());
            d.list->SetPipelineState(d.atrousPso.Get());
            struct { int step; float phiN; float phiD; float phiL; int svgf; int iter; int w; int h; } ac = {
                (1 << it), fp.gi_denoise_phi_normal, fp.gi_denoise_phi_depth, fp.gi_denoise_phi_lum,
                svgf, it, (int)d.width, (int)d.height,
            };
            d.list->SetGraphicsRoot32BitConstants(0, 8, &ac, 0);
            d.list->SetGraphicsRootDescriptorTable(1, d.SrvGpu(srcSlot));  // t0 indirect src
            d.list->SetGraphicsRootDescriptorTable(2, d.SrvGpu(4));        // t1 gbuf
            d.list->SetGraphicsRootDescriptorTable(3, d.SrvGpu(5));        // t2 variance
            // u0 Accum: the SVGF colour-feedback target (same buffer SHADE wrote, i.e.
            // next frame's PrevAccum). Bound every iteration; PSAtrous only WRITES it
            // when atrousIter == 0, so iters > 0 leave the history untouched.
            d.list->SetGraphicsRootUnorderedAccessView(4, d.accumBuf[curBuf]->GetGPUVirtualAddress());
            d.list->DrawInstanced(3, 1, 0, 0);
            srcT = dstT;   // dst becomes next source
        }
        // After the loop, indirect[srcT] holds the final filtered result (it was the
        // last dst, transitioned to RT in this iteration). Make it readable for composite.
        const UINT finalSlot = (srcT == 0) ? 0u : 1u;
        toState(finalSlot, kPSR);
        // direct + albedo are read by composite too.
        toState(2, kPSR);  // direct
        toState(3, kPSR);  // albedo

        // ---- 3) DEFERRED LIGHTING (COMPOSITE): read the G-buffer -> final =
        //        direct + albedo*filtered_indirect; tonemap; dither -> backbuffer ----
        D3D12_RESOURCE_BARRIER bb{};
        bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bb.Transition.pResource = d.targets[idx].Get();
        bb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bb.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        bb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        d.list->ResourceBarrier(1, &bb);

        D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        bbRtv.ptr += static_cast<SIZE_T>(idx) * d.rtvSize;
        d.list->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
        const float clear[4] = {fp.clear[0], fp.clear[1], fp.clear[2], 1.0f};
        d.list->ClearRenderTargetView(bbRtv, clear, 0, nullptr);

        d.list->SetGraphicsRootSignature(d.compositeRootSig.Get());
        d.list->SetPipelineState(d.compositePso.Get());
        struct { float exposure; int hdr; int dither; int w; int h; } cc = {
            fp.exposure, fp.hdr, fp.dither, (int)d.width, (int)d.height,
        };
        d.list->SetGraphicsRoot32BitConstants(0, 5, &cc, 0);
        d.list->SetGraphicsRootDescriptorTable(1, d.SrvGpu(2));          // t0 direct
        d.list->SetGraphicsRootDescriptorTable(2, d.SrvGpu(3));          // t1 albedo
        d.list->SetGraphicsRootDescriptorTable(3, d.SrvGpu(finalSlot));  // t2 filtered indirect
        d.list->SetGraphicsRootShaderResourceView(4, d.blueNoiseBuf->GetGPUVirtualAddress());  // t3 BlueNoise
        d.list->DrawInstanced(3, 1, 0, 0);

        bb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        d.list->ResourceBarrier(1, &bb);

        // Restore ALL offscreen targets to RENDER_TARGET so next frame starts from a
        // known state (the toState() tracker has them mostly in PSR now). WaitIdle at
        // frame end guarantees the GPU is done before next frame mutates them.
        for (int i = 0; i < 6; ++i) toState((UINT)i, kRT);
    }

    d.list->Close();

    ID3D12CommandList* lists[] = {d.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.swap->Present(1, 0);
    d.WaitIdle();

    // Flip the ping-pong: the Accum/HitPos (and Moments + DirectAccum) written this frame
    // become next frame's "previous" (the reprojection source). Only meaningful in QUALITY
    // mode but harmless to flip always. WaitIdle above guarantees the GPU finished this
    // frame's writes before the swap reuses those buffers as the read side next frame.
    d.accumPair ^= 1u;
}

void Renderer::SetVoxels(const std::vector<std::uint32_t>& grid, const std::uint32_t* palette256) {
    if (!valid_) return;
    Impl& d = *impl_;
    d.WaitIdle();  // ensure GPU is not reading either buffer mid-frame

    constexpr std::size_t kVoxels = static_cast<std::size_t>(kGrid) * kGrid * kGrid;
    constexpr std::size_t kPalEntries = 256;

    // Copy voxel grid (clamp to buffer size).
    if (d.voxelPtr) {
        const std::size_t toCopy = std::min(grid.size(), kVoxels);
        std::memcpy(d.voxelPtr, grid.data(), toCopy * sizeof(std::uint32_t));
        // Zero-fill any remainder (in case caller supplies a smaller grid).
        if (toCopy < kVoxels)
            std::memset(d.voxelPtr + toCopy, 0, (kVoxels - toCopy) * sizeof(std::uint32_t));
    }

    // Copy palette.
    if (d.palettePtr && palette256) {
        std::memcpy(d.palettePtr, palette256, kPalEntries * sizeof(std::uint32_t));
    }

    // Rebuild the brick-occupancy acceleration structure from the voxels that
    // actually landed in the GPU buffer (exactly kGrid^3 after clamp/zero-fill),
    // so empty-space skipping stays conservative and correct after live edits.
    if (d.brickPtr && d.voxelPtr) {
        std::memset(d.brickPtr, 0, static_cast<std::size_t>(kBrickCount) * sizeof(std::uint32_t));
        for (UINT z = 0; z < kGrid; ++z) {
            const UINT bz = z / kBrickDim;
            for (UINT y = 0; y < kGrid; ++y) {
                const UINT by = y / kBrickDim;
                const std::size_t rowBase = (static_cast<std::size_t>(z) * kGrid + y) * kGrid;
                for (UINT x = 0; x < kGrid; ++x) {
                    if (d.voxelPtr[rowBase + x] != 0u) {
                        const UINT bx = x / kBrickDim;
                        d.brickPtr[(static_cast<std::size_t>(bz) * kBrickGrid + by) * kBrickGrid + bx] = 1u;
                    }
                }
            }
        }
    }

    // Reset GI accumulation so the new geometry doesn't ghost.
    d.accumFrame = 0;
    d.accHave    = false;
}

void Renderer::EditVoxels(int x0, int y0, int z0, int x1, int y1, int z1,
                          const std::uint32_t* region, const std::uint32_t* palette256) {
    if (!valid_ || !region) return;
    Impl& d = *impl_;
    const int G = static_cast<int>(kGrid);
    x0 = std::max(0, x0); y0 = std::max(0, y0); z0 = std::max(0, z0);
    x1 = std::min(G, x1); y1 = std::min(G, y1); z1 = std::min(G, z1);
    if (x0 >= x1 || y0 >= y1 || z0 >= z1) return;
    const int dx = x1 - x0, dy = y1 - y0;

    // Write only the edited cells straight into the persistently-mapped buffer.
    // NO WaitIdle: aligned uint32 stores are atomic, so the GPU reads either the
    // old or new value per cell for at most one frame -- invisible for an edit.
    if (d.voxelPtr) {
        for (int z = z0; z < z1; ++z)
            for (int y = y0; y < y1; ++y) {
                const std::size_t dstRow = (static_cast<std::size_t>(z) * G + y) * G;
                const std::size_t srcRow = (static_cast<std::size_t>(z - z0) * dy + (y - y0)) * dx;
                for (int x = x0; x < x1; ++x)
                    d.voxelPtr[dstRow + x] = region[srcRow + (x - x0)];
            }
    }
    if (d.palettePtr && palette256)
        std::memcpy(d.palettePtr, palette256, 256 * sizeof(std::uint32_t));

    // Refresh only the brick-occupancy cells overlapping the edit box (rescan
    // each affected brick's voxels in the buffer we just updated).
    if (d.brickPtr && d.voxelPtr) {
        const int BD = static_cast<int>(kBrickDim);
        const int bx0 = x0 / BD, bx1 = (x1 - 1) / BD;
        const int by0 = y0 / BD, by1 = (y1 - 1) / BD;
        const int bz0 = z0 / BD, bz1 = (z1 - 1) / BD;
        for (int bz = bz0; bz <= bz1; ++bz)
            for (int by = by0; by <= by1; ++by)
                for (int bx = bx0; bx <= bx1; ++bx) {
                    std::uint32_t occ = 0u;
                    const int cz1 = std::min(G, bz * BD + BD);
                    const int cy1 = std::min(G, by * BD + BD);
                    const int cx1 = std::min(G, bx * BD + BD);
                    for (int z = bz * BD; z < cz1 && !occ; ++z)
                        for (int y = by * BD; y < cy1 && !occ; ++y) {
                            const std::size_t row = (static_cast<std::size_t>(z) * G + y) * G;
                            for (int x = bx * BD; x < cx1; ++x)
                                if (d.voxelPtr[row + x] != 0u) { occ = 1u; break; }
                        }
                    d.brickPtr[(static_cast<std::size_t>(bz) * kBrickGrid + by) * kBrickGrid + bx] = occ;
                }
    }

    // Geometry changed -> restart GI accumulation (cheap; converges again).
    d.accumFrame = 0;
    d.accHave    = false;
}

void Renderer::SetGiDenoise(bool enabled) {
    if (!impl_) return;
    Impl& d = *impl_;
    if (d.giDenoise == enabled) return;
    d.giDenoise = enabled;
    // Restart accumulation on toggle so the A/B switch is clean (no stale history
    // bleeding across the mode change).
    d.accumFrame = 0;
    d.accHave    = false;
}

void Renderer::SetEmptySpaceSkip(bool enabled) {
    if (!impl_) return;
    Impl& d = *impl_;
    if (d.emptySkip == enabled) return;
    d.emptySkip = enabled;
    // Empty-space skipping is conservative, so the image is identical with it on
    // or off; restart GI accumulation anyway so an A/B toggle starts from a clean
    // history rather than blending two (visually identical) traversals.
    d.accumFrame = 0;
    d.accHave    = false;
}

void Renderer::SetGiDebug(bool enabled) {
    if (!impl_) return;
    Impl& d = *impl_;
    if (d.giDebug == enabled) return;
    d.giDebug = enabled;
    // Toggling the GI-only view changes what is accumulated, so restart so it
    // converges fresh to the debug (or normal) image instead of blending both.
    d.accumFrame = 0;
    d.accHave    = false;
}

void Renderer::SetGiReproject(bool enabled) {
    if (!impl_) return;
    Impl& d = *impl_;
    if (d.giReproject == enabled) return;
    d.giReproject = enabled;
    // Switching the history-alignment strategy (reprojected vs screen-space EMA)
    // changes how history is interpreted, so restart accumulation for a clean A/B
    // switch rather than blending the two regimes.
    d.accumFrame = 0;
    d.accHave    = false;
}

bool Renderer::CaptureScreenshot(const std::string& path, bool png) {
    if (!valid_) return false;
    Impl& d = *impl_;

    // Pick the current back-buffer index.
    const UINT i = d.swap->GetCurrentBackBufferIndex();

    // Query the texture description and compute the readback footprint.
    D3D12_RESOURCE_DESC texDesc = d.targets[i]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT    numRows      = 0;
    UINT64  rowSizeBytes = 0;
    UINT64  totalBytes   = 0;
    d.device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

    // Create a READBACK buffer.
    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width              = totalBytes;
    rbDesc.Height             = 1;
    rbDesc.DepthOrArraySize   = 1;
    rbDesc.MipLevels          = 1;
    rbDesc.Format             = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count   = 1;
    rbDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(d.device->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&readback)))) {
        vox::log::Error("screenshot: failed to create readback buffer");
        return false;
    }

    // Record: barrier PRESENT->COPY_SOURCE, copy, barrier back, execute, wait.
    d.alloc->Reset();
    d.list->Reset(d.alloc.Get(), nullptr);

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource   = d.targets[i].Get();
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    d.list->ResourceBarrier(1, &bar);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource        = readback.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint  = footprint;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = d.targets[i].Get();
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    d.list->ResourceBarrier(1, &bar);
    d.list->Close();

    ID3D12CommandList* lists[] = {d.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();

    // Map the readback buffer and copy rows (respecting RowPitch alignment).
    void* mapped = nullptr;
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(totalBytes)};
    if (FAILED(readback->Map(0, &readRange, &mapped))) {
        vox::log::Error("screenshot: Map failed");
        return false;
    }

    const UINT width  = d.width;
    const UINT height = d.height;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    const UINT rowPitch = footprint.Footprint.RowPitch;
    const std::uint8_t* src_ptr = static_cast<const std::uint8_t*>(mapped);
    for (UINT row = 0; row < height; ++row) {
        std::memcpy(rgba.data() + row * width * 4u, src_ptr + row * rowPitch, width * 4u);
    }

    D3D12_RANGE writeRange{0, 0};
    readback->Unmap(0, &writeRange);

    bool ok;
    if (png) {
        ok = stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height),
                            4, rgba.data(), static_cast<int>(width) * 4) != 0;
    } else {
        ok = stbi_write_bmp(path.c_str(), static_cast<int>(width), static_cast<int>(height),
                            4, rgba.data()) != 0;
    }
    if (ok)  vox::log::Info("screenshot: saved {}", path);
    else     vox::log::Error("screenshot: write failed to {}", path);
    return ok;
}

void Renderer::Shutdown() {
    if (impl_) {
        // Join the background bake thread before tearing down DX12 resources.
        if (impl_->bnThread.joinable()) impl_->bnThread.join();
        impl_->WaitIdle();
    }
    if (impl_ && impl_->fenceEvent) {
        CloseHandle(impl_->fenceEvent);
        impl_->fenceEvent = nullptr;
    }
    valid_ = false;
}

}  // namespace vox::render

#else  // ---- headless / no-DX12 stub ----

namespace vox::render {
struct Renderer::Impl {};
Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() {}
bool Renderer::Init(void*, int, int, const std::vector<std::uint32_t>*, const std::uint32_t*) { return false; }
void Renderer::RenderFrame(const FrameParams&) {}
void Renderer::SetVoxels(const std::vector<std::uint32_t>&, const std::uint32_t*) {}
void Renderer::EditVoxels(int, int, int, int, int, int, const std::uint32_t*, const std::uint32_t*) {}
void Renderer::SetGiDenoise(bool) {}
void Renderer::SetEmptySpaceSkip(bool) {}
void Renderer::SetGiDebug(bool) {}
void Renderer::SetGiReproject(bool) {}
bool Renderer::CaptureScreenshot(const std::string&, bool) { return false; }
void Renderer::Shutdown() {}
}  // namespace vox::render

#endif
