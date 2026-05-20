// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// GLFW window + keyboard input. Created with GLFW_NO_API so a DX12 swapchain
// can target its HWND. Key presses are translated to (key-name, modifier-mask)
// and forwarded to a handler (wired to the Keybindings dispatcher). When the
// 'window' feature is off, this is a no-op stub and the engine runs headless.

#include <cstdint>
#include <functional>
#include <string>

struct GLFWwindow;

namespace vox::platform {

class Window {
public:
    using KeyHandler = std::function<void(const std::string& key, std::uint32_t mods)>;

    bool Create(int width, int height, const char* title);
    void Destroy();
    bool Valid() const { return win_ != nullptr; }
    bool ShouldClose() const;
    void RequestClose();
    void PollEvents();

    void* NativeHandle() const;  // HWND on Windows (nullptr if no window)
    int   Width() const { return width_; }
    int   Height() const { return height_; }

    void SetKeyHandler(KeyHandler h) { key_handler_ = std::move(h); }

    // Invoked by the GLFW key callback; forwards to the handler. Public so the
    // free C callback can reach it without a friend declaration.
    void DispatchKey(const std::string& key, std::uint32_t mods);

private:
    GLFWwindow* win_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    KeyHandler key_handler_;
};

}  // namespace vox::platform
