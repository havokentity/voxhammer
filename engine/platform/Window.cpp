// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "platform/Window.h"

#include "platform/Log.h"

#if defined(VOX_HAVE_WINDOW)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

namespace vox::platform {
namespace {

std::string KeyName(int key) {
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F25) return "F" + std::to_string(key - GLFW_KEY_F1 + 1);
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) return std::string(1, static_cast<char>('A' + (key - GLFW_KEY_A)));
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) return std::string(1, static_cast<char>('0' + (key - GLFW_KEY_0)));
    switch (key) {
    case GLFW_KEY_ESCAPE: return "Esc";
    case GLFW_KEY_PRINT_SCREEN: return "PrintScreen";
    case GLFW_KEY_SPACE: return "Space";
    case GLFW_KEY_ENTER: return "Enter";
    case GLFW_KEY_TAB: return "Tab";
    case GLFW_KEY_GRAVE_ACCENT: return "Grave";
    default: return {};
    }
}

std::uint32_t ModMask(int glfwMods) {
    std::uint32_t m = 0;
    if (glfwMods & GLFW_MOD_SHIFT) m |= 1u << 0;
    if (glfwMods & GLFW_MOD_CONTROL) m |= 1u << 1;
    if (glfwMods & GLFW_MOD_ALT) m |= 1u << 2;
    return m;
}

void KeyThunk(GLFWwindow* w, int key, int /*sc*/, int action, int mods) {
    if (action != GLFW_PRESS) return;
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (!self) return;
    std::string name = KeyName(key);
    if (name.empty()) return;
    self->DispatchKey(name, ModMask(mods));
}

bool g_glfw_init = false;
}  // namespace

void Window::DispatchKey(const std::string& key, std::uint32_t mods) {
    if (key_handler_) key_handler_(key, mods);
}

bool Window::Create(int width, int height, const char* title) {
    if (!g_glfw_init) {
        if (!glfwInit()) {
            vox::log::Error("glfwInit failed");
            return false;
        }
        g_glfw_init = true;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // DX12 owns the surface
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);     // fixed size keeps the swapchain simple this pass
    win_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!win_) {
        vox::log::Error("glfwCreateWindow failed");
        return false;
    }
    width_ = width;
    height_ = height;
    glfwSetWindowUserPointer(win_, this);
    glfwSetKeyCallback(win_, &KeyThunk);
    vox::log::Info("window: {}x{} '{}'", width, height, title);
    return true;
}

void Window::Destroy() {
    if (win_) {
        glfwDestroyWindow(win_);
        win_ = nullptr;
    }
    if (g_glfw_init) {
        glfwTerminate();
        g_glfw_init = false;
    }
}

bool Window::ShouldClose() const { return win_ ? glfwWindowShouldClose(win_) != 0 : true; }
void Window::RequestClose() { if (win_) glfwSetWindowShouldClose(win_, GLFW_TRUE); }
void Window::PollEvents() { if (win_) glfwPollEvents(); }
void* Window::NativeHandle() const { return win_ ? static_cast<void*>(glfwGetWin32Window(win_)) : nullptr; }

Window::CameraInput Window::PollCameraInput() {
    CameraInput ci;
    if (!win_) return ci;
    bool look = glfwGetMouseButton(win_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    double mx = 0, my = 0;
    glfwGetCursorPos(win_, &mx, &my);
    if (look) {
        ci.active = true;
        auto kd = [&](int k) { return glfwGetKey(win_, k) == GLFW_PRESS ? 1.0f : 0.0f; };
        ci.move_strafe = kd(GLFW_KEY_D) - kd(GLFW_KEY_A);
        ci.move_up = kd(GLFW_KEY_E) - kd(GLFW_KEY_Q);
        ci.move_fwd = kd(GLFW_KEY_W) - kd(GLFW_KEY_S);
        ci.fast = glfwGetKey(win_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(win_, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        if (looking_) {
            ci.look_dx = static_cast<float>(mx - last_cx_);
            ci.look_dy = static_cast<float>(my - last_cy_);
        }
        looking_ = true;
    } else {
        looking_ = false;
    }
    last_cx_ = mx;
    last_cy_ = my;
    return ci;
}

}  // namespace vox::platform

#else  // ---- headless stub ----

namespace vox::platform {
bool Window::Create(int, int, const char*) { return false; }
void Window::Destroy() {}
bool Window::ShouldClose() const { return true; }
void Window::RequestClose() {}
void Window::PollEvents() {}
void* Window::NativeHandle() const { return nullptr; }
Window::CameraInput Window::PollCameraInput() { return {}; }
}  // namespace vox::platform

#endif
