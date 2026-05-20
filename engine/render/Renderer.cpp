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
    int    giDebug;        int   giBounces;    float giEmissive;  int giPad10c;   // row 10
};
StructuredBuffer<uint> Voxels : register(t0);
RWStructuredBuffer<float4> Accum : register(u0);  // per-pixel GI accumulation: rgb + sample count

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

// Emissive strength, packed in the palette ALPHA byte (0 = not emissive). The
// .vox importer writes saturate(emit * 2^(flux-1)) there; here we expand back to
// an HDR range (x8) and scale by the giEmissive cvar. Emissive surfaces act as
// light sources in the GI path, so (with multi-bounce) bright voxels fill rooms.
float emission(uint m) {
    uint p = Palette[m & 255u];
    return (float((p >> 24) & 255u) / 255.0) * 8.0 * giEmissive;
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
// Split into two adjacent raw-string literals (C++ concatenates them at compile
// time). MSVC caps a single string literal token at 16380 bytes; the combined
// shader is near that, so the break keeps each chunk comfortably under the limit.
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

// One-bounce diffuse path-traced radiance at the primary hit.
// giSamples indirect bounces are averaged per frame; temporal accumulation converges the result.
float3 giRadiance(float3 hp, float3 nrm, uint mat, float2 screenPos) {
    float3 alb = palette(mat);

    // Direct sun, jittered inside the penumbra cone (accumulates -> soft shadow).
    // k=4,5 reserved for this pair (k=0..3 used by shadow/AO blue samples).
    float2 xs = sampleXi(screenPos, accumFrame, 4);
    float3 st, sb; buildBasis(sunDir, st, sb);
    float  ang = xs.y * 6.2832;
    float  rad = sqrt(xs.x) * tan(max(shadowSoftness, 0.0008));
    float3 sd  = normalize(sunDir + (st * cos(ang) + sb * sin(ang)) * rad);
    float3 direct = directSun(hp, nrm, sd);

    // Indirect: average giSamples hemisphere PATHS, each up to giBounces deep.
    // Light "walks" surface->surface (sun -> floor -> wall -> ...) so enclosed
    // rooms fill in, not just directly sun-lit faces. Lambertian + cosine-
    // weighted sampling => the per-bounce estimator weight is just the surface
    // albedo (the pi/pdf cancel). depth==1 reproduces the old single-bounce look;
    // depth>=2 is real multi-bounce GI. Temporal accumulation converges it.
    int    numSamples = clamp(giSamples, 1, 8);
    int    maxDepth   = clamp(giBounces, 0, 5);
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
        indirectSum += L;
    }
    float3 indirectAvg = indirectSum / float(numSamples);
    if (giDebug != 0) return indirectAvg;   // GI debug view: show ONLY the indirect bounce radiance
    return alb * (direct + indirectAvg) + alb * emission(mat);   // + self-emission (emissive voxels glow)
}

float4 PSMain(VSOut i) : SV_Target {
    float2 ndc = (i.pos.xy / viewport) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float tf = tan(fov * 0.5);
    float3 rd = normalize(camFwd + camRight * (ndc.x * tf * aspect) + camUp * (ndc.y * tf));
    float3 ro = camPos;

    float3 hp, nrm; uint mat;
    float3 col;
    if (!traceVoxel(ro, rd, 768, hp, nrm, mat)) {
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
        col += alb * emission(mat);   // emissive voxels glow (no GI fill in PERFORMANCE, but they still emit)
    }

    // QUALITY: progressive temporal accumulation. New-sample blend weight is
    // 1/(n+1), so the per-pixel sample cap (giHistMax) is the only denoise knob:
    // still -> cap 2048 (converges clean); moving -> small cap => EMA that keeps
    // history (kills the 1-spp noise storm). accumFrame==0 still forces a fresh
    // start on a genuine reset; giDenoise off keeps the cap at 2048 (original).
    if (lightingMode == 1) {
        uint w = (uint)viewport.x;
        uint pix = (uint)i.pos.y * w + (uint)i.pos.x;
        float4 hist = Accum[pix];
        float cap = (giDenoise != 0) ? max(giHistMax, 0.0) : 2048.0;
        float n = (accumFrame == 0) ? 0.0 : min(hist.a, cap);
        float3 avg = (hist.rgb * n + col) / (n + 1.0);
        Accum[pix] = float4(avg, n + 1.0);
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
    int   giDebug;         int   giBounces;        float giEmissive; int giPad10c;       // row 10
};

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
    ComPtr<ID3D12Resource>            accumBuf;       // GI temporal accumulation (RWStructuredBuffer<float4>)
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
    float                             accCamKey[6]    = {};
    float                             accSceneKey[17] = {};
    bool                              accHave = false;
    UINT                              accumFrame = 0;
    bool                              giDenoise = true;   // QUALITY temporal denoise (Renderer::SetGiDenoise)
    bool                              emptySkip = true;   // empty-space skipping (Renderer::SetEmptySpaceSkip)
    bool                              giDebug   = false;  // GI-only debug view (Renderer::SetGiDebug)

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
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() { Shutdown(); }

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

    // --- root signature: CBV(b0) + SRV(t0 Voxels) + UAV(u0 Accum) + SRV(t1 BlueNoise) + SRV(t2 Palette) + SRV(t3 BrickOccupancy) ---
    D3D12_ROOT_PARAMETER rp[6]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[0].Descriptor.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[1].Descriptor.ShaderRegister = 0;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[2].Descriptor.ShaderRegister = 0;
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
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 6;
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

    // --- GI temporal-accumulation buffer (default-heap UAV: float4 per pixel) ---
    d.accumBuf = d.MakeDefaultUAV(static_cast<UINT64>(d.width) * d.height * 16);
    if (!d.accumBuf) return false;

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
    const float camKey[6] = {
        fp.cam_pos[0], fp.cam_pos[1], fp.cam_pos[2], fp.cam_yaw, fp.cam_pitch, fp.cam_fov,
    };
    const float sceneKey[19] = {
        fp.sun[0], fp.sun[1], fp.sun[2], fp.exposure, fp.ambient,
        fp.clear[0], fp.clear[1], fp.clear[2], fp.shadow_softness, fp.ao_strength, fp.ao_radius,
        static_cast<float>(fp.lighting_mode), static_cast<float>(fp.hdr),
        static_cast<float>(fp.dither), static_cast<float>(fp.ao_samples),
        static_cast<float>(fp.shadow_samples), static_cast<float>(fp.gi_samples),
        static_cast<float>(fp.gi_bounces), fp.gi_emissive,
    };
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
    } else if (d.giDenoise) {
        // Pure camera motion with denoise ON: KEEP history, advance the sample
        // sequence, and shorten the cap based on motion magnitude. Larger motion
        // -> smaller cap -> higher EMA alpha -> less ghosting (but a touch noisier).
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
        d.accumFrame = 0;                 // camera moved, denoise OFF: original hard reset
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

    d.alloc->Reset();
    d.list->Reset(d.alloc.Get(), d.pso.Get());

    const UINT idx = d.swap->GetCurrentBackBufferIndex();
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

    D3D12_VIEWPORT vp{0, 0, float(d.width), float(d.height), 0, 1};
    D3D12_RECT sc{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
    d.list->RSSetViewports(1, &vp);
    d.list->RSSetScissorRects(1, &sc);

    const float clear[4] = {fp.clear[0], fp.clear[1], fp.clear[2], 1.0f};
    d.list->ClearRenderTargetView(rtv, clear, 0, nullptr);

    d.list->SetGraphicsRootSignature(d.rootSig.Get());
    d.list->SetGraphicsRootConstantBufferView(0, d.camBuf->GetGPUVirtualAddress());
    d.list->SetGraphicsRootShaderResourceView(1, d.voxelBuf->GetGPUVirtualAddress());
    d.list->SetGraphicsRootUnorderedAccessView(2, d.accumBuf->GetGPUVirtualAddress());
    d.list->SetGraphicsRootShaderResourceView(3, d.blueNoiseBuf->GetGPUVirtualAddress());
    d.list->SetGraphicsRootShaderResourceView(4, d.paletteBuf->GetGPUVirtualAddress());
    d.list->SetGraphicsRootShaderResourceView(5, d.brickBuf->GetGPUVirtualAddress());
    d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.list->DrawInstanced(3, 1, 0, 0);

    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    d.list->ResourceBarrier(1, &bar);
    d.list->Close();

    ID3D12CommandList* lists[] = {d.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.swap->Present(1, 0);
    d.WaitIdle();
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
bool Renderer::CaptureScreenshot(const std::string&, bool) { return false; }
void Renderer::Shutdown() {}
}  // namespace vox::render

#endif
