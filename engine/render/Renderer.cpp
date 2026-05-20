// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "render/Renderer.h"

#include "platform/Log.h"

#if defined(VOX_HAVE_DX12)

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace vox::render {
namespace {

constexpr UINT kGrid = 64;  // 64^3 procedural voxel volume

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
    float  aoStrength;     float aoRadius;     float pad0; float pad1;
};
StructuredBuffer<uint> Voxels : register(t0);

struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    float2 t = float2((vid << 1) & 2, vid & 2);   // (0,0)(2,0)(0,2)
    o.pos = float4(t * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}

float3 palette(uint m) {
    if (m == 1) return float3(0.42, 0.66, 0.30);  // grass
    if (m == 2) return float3(0.48, 0.36, 0.26);  // dirt
    if (m == 3) return float3(0.55, 0.56, 0.60);  // stone
    if (m == 4) return float3(0.82, 0.30, 0.24);  // sphere
    return float3(0.7, 0.7, 0.7);
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

// Interleaved Gradient Noise (Jimenez 2014): a high-quality, low-discrepancy
// per-pixel value in [0,1). Used to rotate the sample kernels per pixel so the
// few taps read as fine film grain instead of the repeating tile a frac/sin
// hash produces.
float ign(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
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
    [loop] for (int i = 0; i < maxSteps; ++i) {
        if (tM.x < tM.y && tM.x < tM.z) { cell.x += s.x; tM.x += tD.x; }
        else if (tM.y < tM.z)           { cell.y += s.y; tM.y += tD.y; }
        else                            { cell.z += s.z; tM.z += tD.z; }
        if (cell.x < 0 || cell.y < 0 || cell.z < 0 ||
            cell.x >= gridDim || cell.y >= gridDim || cell.z >= gridDim) return false;
        if (Voxels[(uint)(cell.z * gridDim * gridDim + cell.y * gridDim + cell.x)] != 0) return true;
    }
    return false;
}

// Soft shadow: average N jittered shadow rays within a cone of half-angle shadowSoftness.
// Uses a deterministic screen-space seed so no temporal flicker.
float softShadow(float3 hp, float3 nrm, float3 sunD, float2 screenPos) {
    [branch] if (shadowSoftness < 1e-4) {
        // Hard shadow fast path.
        return occludedRay(hp + nrm * 0.02, sunD, 160) ? 0.0 : 1.0;
    }

    // Build a tangent frame around sunDir to jitter within the cone.
    float3 st, sb;
    buildBasis(sunD, st, sb);

    float rot  = ign(screenPos) * 6.2832;   // per-pixel kernel rotation
    float tanS = tan(shadowSoftness);
    float lit = 0.0;
    const int N = 6;
    [unroll] for (int s = 0; s < N; ++s) {
        // Vogel-disk tap inside the penumbra cone, rotated per pixel.
        float2 d   = vogelDisk(s, N, rot) * tanS;
        float3 dir = normalize(sunD + st * d.x + sb * d.y);
        lit += occludedRay(hp + nrm * 0.02, dir, 160) ? 0.0 : 1.0;
    }
    return lit / float(N);
}

// Ambient occlusion: sample hemisphere oriented around nrm.
float ambientOcclusion(float3 hp, float3 nrm, float2 screenPos) {
    [branch] if (aoStrength < 1e-4) return 1.0;

    float3 t, b;
    buildBasis(nrm, t, b);

    float rot   = ign(screenPos + 23.71) * 6.2832;  // decorrelated from the shadow kernel
    int   steps = (int)clamp(aoRadius, 2.0, 24.0);
    const float GOLDEN_ANGLE = 2.39996323;
    float occ = 0.0;
    const int M = 5;
    [unroll] for (int r = 0; r < M; ++r) {
        // Low-discrepancy cosine-weighted hemisphere sample, rotated per pixel:
        // radial strata -> elevation, golden-angle azimuth -> even spread.
        float u    = (float(r) + 0.5) / float(M);
        float phi  = float(r) * GOLDEN_ANGLE + rot;
        float cosT = sqrt(1.0 - u);           // cosine-weighted toward the normal
        float sinT = sqrt(u);
        float3 dir = normalize(nrm * cosT + (t * cos(phi) + b * sin(phi)) * sinT);
        if (occludedRay(hp + nrm * 0.02, dir, steps)) occ += 1.0;
    }
    float visibility = 1.0 - (occ / float(M)) * aoStrength;
    return saturate(visibility);
}

float4 PSMain(VSOut i) : SV_Target {
    float2 ndc = (i.pos.xy / viewport) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float tf = tan(fov * 0.5);
    float3 rd = normalize(camFwd + camRight * (ndc.x * tf * aspect) + camUp * (ndc.y * tf));
    if (abs(rd.x) < 1e-5) rd.x = 1e-5;
    if (abs(rd.y) < 1e-5) rd.y = 1e-5;
    if (abs(rd.z) < 1e-5) rd.z = 1e-5;
    float3 ro = camPos;

    float3 col;
    float3 bmin = float3(0,0,0), bmax = float3(gridDim, gridDim, gridDim);
    float3 t0 = (bmin - ro) / rd, t1 = (bmax - ro) / rd;
    float3 ts = min(t0, t1), tb = max(t0, t1);
    float tBox  = max(max(ts.x, ts.y), ts.z);
    float tExit = min(min(tb.x, tb.y), tb.z);

    if (tExit < max(tBox, 0.0)) {
        col = sky(rd);
    } else {
        ro = ro + rd * (max(tBox, 0.0) + 0.001);
        float3 cell = floor(ro);
        float3 stepv = sign(rd);
        float3 tDelta = abs(1.0 / rd);
        float3 tMax = ((cell + (stepv * 0.5 + 0.5)) - ro) / rd;
        float3 nrm = float3(0, 1, 0);
        float tEnter = 0.0;
        bool hit = false; uint mat = 0;
        [loop] for (int k = 0; k < 512; ++k) {
            if (cell.x < 0 || cell.y < 0 || cell.z < 0 ||
                cell.x >= gridDim || cell.y >= gridDim || cell.z >= gridDim) break;
            uint v = Voxels[(uint)(cell.z * gridDim * gridDim + cell.y * gridDim + cell.x)];
            if (v != 0) { hit = true; mat = v; break; }
            if (tMax.x < tMax.y && tMax.x < tMax.z) { tEnter = tMax.x; cell.x += stepv.x; tMax.x += tDelta.x; nrm = float3(-stepv.x, 0, 0); }
            else if (tMax.y < tMax.z)               { tEnter = tMax.y; cell.y += stepv.y; tMax.y += tDelta.y; nrm = float3(0, -stepv.y, 0); }
            else                                    { tEnter = tMax.z; cell.z += stepv.z; tMax.z += tDelta.z; nrm = float3(0, 0, -stepv.z); }
        }
        if (hit) {
            float3 hp = ro + rd * tEnter;
            float ndl = saturate(dot(nrm, sunDir));

            // Soft penumbra shadow (6 rays, jittered within shadowSoftness cone).
            float shadow = softShadow(hp, nrm, sunDir, i.pos.xy);

            // Short-range ambient occlusion (5 hemisphere rays, aoRadius steps each).
            float ao = ambientOcclusion(hp, nrm, i.pos.xy);

            float3 alb = palette(mat);
            float3 ambientLight = float3(ambient, ambient, ambient) * ao;
            col = alb * (ambientLight + (1.0 - ambient) * ndl * shadow);
        } else {
            col = sky(rd);
        }
    }

    col *= exposure;
    col = (hdr != 0) ? tonemapACES(col) : saturate(col);
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
    float aoStrength;      float aoRadius;         float pad0; float pad1; // row 7
};

std::vector<std::uint32_t> GenerateScene(UINT g) {
    std::vector<std::uint32_t> v(static_cast<size_t>(g) * g * g, 0);
    for (UINT z = 0; z < g; ++z)
        for (UINT x = 0; x < g; ++x) {
            float h = 22.0f + 8.0f * std::sin(x * 0.20f) * std::cos(z * 0.18f);
            for (UINT y = 0; y < g; ++y) {
                std::uint32_t m = 0;
                if (y < h) m = (y > h - 1.5f) ? 1u : (y > h - 5.0f ? 2u : 3u);
                float dx = float(x) - 44, dy = float(y) - 42, dz = float(z) - 40;
                if (dx * dx + dy * dy + dz * dz < 64.0f) m = 4;  // floating sphere r=8 (casts a clear shadow)
                v[size_t(z) * g * g + size_t(y) * g + x] = m;
            }
        }
    return v;
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
    ComPtr<ID3D12Resource>            camBuf;
    std::uint8_t*                     camPtr = nullptr;

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
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() { Shutdown(); }

bool Renderer::Init(void* hwndPtr, int width, int height, const std::vector<std::uint32_t>* voxels) {
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

    // --- root signature: CBV(b0) + SRV(t0) as root descriptors ---
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[0].Descriptor.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[1].Descriptor.ShaderRegister = 0;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
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
    void* p = nullptr;
    D3D12_RANGE none{0, 0};
    d.voxelBuf->Map(0, &none, &p);
    std::memcpy(p, scene.data(), scene.size() * sizeof(std::uint32_t));
    d.voxelBuf->Unmap(0, nullptr);

    // --- camera CBV (persistently mapped) ---
    d.camBuf = d.MakeUpload(256);
    if (!d.camBuf) return false;
    d.camBuf->Map(0, &none, reinterpret_cast<void**>(&d.camPtr));

    valid_ = true;
    vox::log::Info("DX12: voxel raymarcher ready ({}^3 grid, {}x{}, tearing={})", kGrid, width, height, d.tearing);
    return true;
}

void Renderer::RenderFrame(const FrameParams& fp) {
    if (!valid_) return;
    Impl& d = *impl_;

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
    cb.pad0 = 0.0f;
    cb.pad1 = 0.0f;
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

void Renderer::Shutdown() {
    if (impl_) impl_->WaitIdle();
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
bool Renderer::Init(void*, int, int, const std::vector<std::uint32_t>*) { return false; }
void Renderer::RenderFrame(const FrameParams&) {}
void Renderer::Shutdown() {}
}  // namespace vox::render

#endif
