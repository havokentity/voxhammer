// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "render/Renderer.h"

#include "platform/Log.h"

#if defined(VOX_HAVE_DX12)

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace vox::render {

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

    void WaitIdle() {
        if (!queue || !fence) return;
        const UINT64 v = ++fenceVal;
        queue->Signal(fence.Get(), v);
        if (fence->GetCompletedValue() < v) {
            fence->SetEventOnCompletion(v, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() { Shutdown(); }

bool Renderer::Init(void* hwndPtr, int width, int height) {
    HWND hwnd = static_cast<HWND>(hwndPtr);
    if (!hwnd) {
        return false;
    }
    Impl& d = *impl_;

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        vox::log::Error("DX12: CreateDXGIFactory2 failed");
        return false;
    }
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d.device))) &&
        FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.device)))) {
        vox::log::Error("DX12: no D3D12-capable device (RT-capable GPU required)");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(d.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&d.queue)))) return false;

    BOOL allowTearing = FALSE;
    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(factory.As(&f5))) {
        f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
    }
    d.tearing = allowTearing != 0;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = static_cast<UINT>(width);
    sd.Height = static_cast<UINT>(height);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = Impl::kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = d.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(d.queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        vox::log::Error("DX12: CreateSwapChainForHwnd failed");
        return false;
    }
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
    if (FAILED(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.alloc.Get(), nullptr,
                                           IID_PPV_ARGS(&d.list)))) return false;
    d.list->Close();
    if (FAILED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence)))) return false;
    d.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    valid_ = true;
    vox::log::Info("DX12: device + {}x{} flip-discard swapchain ready (tearing={})", width, height, d.tearing);
    return true;
}

void Renderer::RenderFrame(float r, float g, float b) {
    if (!valid_) return;
    Impl& d = *impl_;

    d.alloc->Reset();
    d.list->Reset(d.alloc.Get(), nullptr);

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

    const float clear[4] = {r, g, b, 1.0f};
    d.list->ClearRenderTargetView(rtv, clear, 0, nullptr);

    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    d.list->ResourceBarrier(1, &bar);
    d.list->Close();

    ID3D12CommandList* lists[] = {d.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.swap->Present(1, 0);  // vsync on (also caps the run loop)
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
bool Renderer::Init(void*, int, int) { return false; }
void Renderer::RenderFrame(float, float, float) {}
void Renderer::Shutdown() {}
}  // namespace vox::render

#endif
