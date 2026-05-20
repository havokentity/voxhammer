// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// Voxhammer sandbox entry point. Boots the control plane (cvar registry,
// hardened web ConsoleServer), opens a window with a minimal DX12 clear, runs
// the keybindings system, and drives the engine loop: drain queued console
// work, render, broadcast telemetry -- until `quit` (or the window closes).

#include "VoxVersion.h"
#include "console/ConsoleServer.h"
#include "console/Password.h"
#include "cvar/Console.h"
#include "platform/Keybindings.h"
#include "platform/Log.h"
#include "platform/Platform.h"
#include "platform/Window.h"
#include "ecs/EcsWorld.h"
#include "jobs/JobScheduler.h"
#include "render/Renderer.h"
#include "script/ScriptHost.h"
#include "voxel/VoxImport.h"
#include "voxel/VoxelWorld.h"

#include <algorithm>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Windows Shell API for bluenoise.open_folder — included last to avoid macro
// collisions (winuser.h defines MOD_SHIFT/MOD_ALT/MOD_CTRL which clash with
// vox::platform::MOD_* constants used above).
#include <windows.h>
#include <shellapi.h>
// Undefine the conflicting keyboard-modifier macros from winuser.h.
#ifdef MOD_SHIFT
#undef MOD_SHIFT
#endif
#ifdef MOD_CTRL
#undef MOD_CTRL
#endif
#ifdef MOD_ALT
#undef MOD_ALT
#endif

#ifndef VOX_VERSION_STRING
#define VOX_VERSION_STRING "0.0.0-dev"
#endif

namespace {
using namespace vox::console;
namespace pf = vox::platform;

std::atomic<bool> g_running{true};

void ParseRGB(const std::string& s, float& r, float& g, float& b) {
    r = g = b = 0.0f;
    std::sscanf(s.c_str(), "%f %f %f", &r, &g, &b);
}

void Usage() {
    vox::log::Info("Voxhammer {} -- standalone DX12 voxel-destruction engine", VOX_VERSION_STRING);
    vox::log::Info("Usage: voxhammer [flags]");
    vox::log::Info("  --remote-password=<pw>  Hash (argon2id) + store remote.password_hash, then run.");
    vox::log::Info("  --reset-cert            Regenerate the self-signed TLS cert before binding.");
    vox::log::Info("  --remote / --no-remote  Force the ConsoleServer on / off.");
    vox::log::Info("  --no-window             Run headless (no GLFW window / DX12).");
    vox::log::Info("  --cvars=<path>          Override the cvars file (default cvars.toml).");
    vox::log::Info("  --vsync=on|off          Override renderer.vsync for this run.");
    vox::log::Info("  --version / --help");
}

void RegisterCoreCvars() {
    Console& c = Console::Get();
    auto reg = [&](const char* n, const char* d, const char* desc, CVarParams p) { c.RegisterCVar(n, d, desc, std::move(p)); };
    reg("renderer.gi.bounces", "3", "Diffuse GI bounce count.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 0, .range_max = 6, .range_step = 1});
    reg("renderer.gi.restir.spatial_passes", "2", "ReSTIR GI spatial resampling passes.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 0, .range_max = 4, .range_step = 1});
    reg("renderer.upscaling.mode", "DLSS_Q", "Super-resolution / upscaler preset.", {.type = CVarType::Enum, .flags = CVAR_ARCHIVE, .enum_values = {"DLSS_DLAA", "DLSS_Q", "DLSS_B", "DLSS_P", "DLSS_UP", "FSR_Q", "FSR_B", "FSR_P", "NATIVE"}});
    reg("renderer.frame_gen.factor", "OFF", "Frame-generation multiplier.", {.type = CVarType::Enum, .flags = CVAR_ARCHIVE, .enum_values = {"OFF", "2X", "3X", "4X"}});
    reg("renderer.hdr.enabled", "0", "HDR10 output (auto-detected display).", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("renderer.hdr.peak_nits", "1000", "HDR peak luminance target.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 400, .range_max = 4000, .range_step = 50});
    reg("renderer.debug.clear_color", "0.05 0.05 0.08", "Swapchain clear color (RGB 0..1).", {.type = CVarType::Color, .flags = CVAR_ARCHIVE});
    reg("renderer.vsync", "1", "Vertical sync.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("renderer.exposure", "1.0", "Render exposure (pre-tonemap multiplier).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.1f, .range_max = 4.0f, .range_step = 0.05f});
    reg("renderer.sun.azimuth", "0.7", "Sun azimuth (radians).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 6.2832f, .range_step = 0.02f});
    reg("renderer.sun.elevation", "0.6", "Sun elevation (radians; lower = longer shadows).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.1f, .range_max = 1.5f, .range_step = 0.02f});
    reg("renderer.ambient", "0.28", "Ambient fill light (lower = punchier shadows).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 1.0f, .range_step = 0.01f});
    reg("renderer.shadow.softness", "0.08", "Soft-shadow penumbra half-angle (radians; 0 = hard).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 0.5f, .range_step = 0.01f});
    reg("renderer.ao.strength", "0.55", "Ambient-occlusion darkening (0 = off).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 1.0f, .range_step = 0.01f});
    reg("renderer.ao.radius", "4.0", "Ambient-occlusion ray reach (voxels).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 1.0f, .range_max = 24.0f, .range_step = 1.0f});
    reg("renderer.lighting.mode", "PERFORMANCE", "Lighting tier: PERFORMANCE (raymarch + AO, high FPS) or QUALITY (path-traced GI; accumulates while the view is still).", {.type = CVarType::Enum, .flags = CVAR_ARCHIVE, .enum_values = {"PERFORMANCE", "QUALITY"}});
    reg("renderer.dither", "1", "Triangular dither before 8-bit output (hides gradient banding).", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("renderer.ao.samples", "8", "Ambient-occlusion rays per pixel.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 1, .range_max = 32, .range_step = 1});
    reg("renderer.shadow.samples", "12", "Soft-shadow penumbra rays per pixel.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 1, .range_max = 16, .range_step = 1});
    reg("renderer.gi.samples", "1", "QUALITY: indirect GI bounce samples accumulated per frame.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 1, .range_max = 8, .range_step = 1});
    reg("physics.gpu_rigids.enabled", "1", "GPU rigid bodies (NVIDIA).", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("physics.gpu_rigids.max_islands", "10000", "Max active dynamic islands.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 256, .range_max = 16384, .range_step = 256});
    reg("physics.solver.position_iters", "8", "Solver position iterations.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 1, .range_max = 32, .range_step = 1});
    reg("sim.fluid.grid_resolution", "MED", "FLIP/Eulerian fluid grid resolution.", {.type = CVarType::Enum, .flags = CVAR_ARCHIVE, .enum_values = {"LOW", "MED", "HIGH", "ULTRA"}});
    reg("sim.fire.combustion_rate", "1.0", "Combustion rate multiplier.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.1f, .range_max = 5.0f, .range_step = 0.1f});
    reg("voxel.streaming.horizon_meters", "256", "Voxel streaming horizon.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 64.0f, .range_max = 512.0f, .range_step = 8.0f});
    reg("voxel.lod.aggressive_eviction", "0", "Aggressively evict distant chunks.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("voxel.import_path", "", "MagicaVoxel .vox file to load at startup (empty = procedural demo scene).", {.type = CVarType::String, .flags = CVAR_ARCHIVE});
    reg("audio.master_volume", "0.8", "Master output volume.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 1.0f, .range_step = 0.01f});
    reg("camera.pos", "32 40 -24", "Free-fly camera position (world units).", {.type = CVarType::Vec3, .flags = CVAR_ARCHIVE});
    reg("camera.yaw", "0.0", "Camera yaw (radians).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = -3.1416f, .range_max = 3.1416f, .range_step = 0.02f});
    reg("camera.pitch", "-0.5", "Camera pitch (radians).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = -1.5f, .range_max = 1.5f, .range_step = 0.02f});
    reg("camera.fov", "1.2", "Camera vertical FOV (radians).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.4f, .range_max = 2.4f, .range_step = 0.02f});
    reg("camera.invert_x", "1", "Invert horizontal mouse-look.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("camera.invert_y", "0", "Invert vertical mouse-look.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("camera.move_speed", "15", "Fly-cam move speed (units/sec).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 1.0f, .range_max = 120.0f, .range_step = 1.0f});
    reg("camera.boost", "3.0", "Fly-cam Shift speed multiplier.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 1.0f, .range_max = 10.0f, .range_step = 0.5f});
    reg("camera.sensitivity", "0.0025", "Mouse-look sensitivity (rad/px).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0005f, .range_max = 0.01f, .range_step = 0.0005f});
    reg("editor.active", "0", "Editor mode (vs play).", {.type = CVarType::Bool});
    reg("debug.show_brick_grid", "0", "Overlay the brick grid.", {.type = CVarType::Bool, .flags = CVAR_DEVELOPER});
    reg("debug.show_physx_wireframe", "0", "Overlay PhysX collision wireframe.", {.type = CVarType::Bool, .flags = CVAR_DEVELOPER});
    reg("debug.pause_simulation", "0", "Pause the simulation.", {.type = CVarType::Bool, .flags = CVAR_CHEAT});
    reg("profiling.tracy_enabled", "0", "Stream to the Tracy profiler.", {.type = CVarType::Bool});
    reg("remote.port_ws", "27960", "WebSocket/HTTPS port.", {.type = CVarType::Int, .flags = CVAR_READONLY});
    reg("remote.port_tcp", "27961", "Scripted TCP port.", {.type = CVarType::Int, .flags = CVAR_READONLY});
    reg("remote.session_ttl_seconds", "3600", "Console session idle TTL.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 300, .range_max = 86400, .range_step = 60});
    reg("remote.password_hash", "", "argon2id hash of the remote console password.", {.type = CVarType::String, .flags = CVAR_ARCHIVE | CVAR_READONLY});
}

// "Ctrl+Shift+F5" -> (key="F5", mods). Single token (no spaces).
void ParseKeySpec(const std::string& spec, std::string& key, std::uint32_t& mods) {
    mods = pf::MOD_NONE;
    std::string cur;
    std::vector<std::string> parts;
    for (char ch : spec) {
        if (ch == '+') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(ch);
    }
    parts.push_back(cur);
    key = parts.empty() ? "" : parts.back();
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i] == "Ctrl") mods |= pf::MOD_CTRL;
        else if (parts[i] == "Shift") mods |= pf::MOD_SHIFT;
        else if (parts[i] == "Alt") mods |= pf::MOD_ALT;
    }
}
std::string KeySpec(const std::string& key, std::uint32_t mods) {
    std::string s;
    for (auto& m : pf::ModNames(mods)) s += m + "+";
    return s + key;
}

void RegisterCoreCommands(ConsoleServer& server, pf::Keybindings& kb) {
    Console& c = Console::Get();
    auto outln = [](Output& o, std::string_view s) { o.PrintLine(s); };

    c.RegisterCommand("version", "Print engine version.", [](std::span<const std::string_view>, Output& o) { o.Print(VOX_VERSION_STRING); });
    c.RegisterCommand("quit", "Quit the engine.", [](std::span<const std::string_view>, Output& o) { g_running = false; o.Print("quitting"); });
    c.RegisterCommand("toggle", "Toggle a boolean cvar.", [](std::span<const std::string_view> a, Output& o) {
        if (a.empty()) { o.Print("usage: toggle <cvar>"); return; }
        CVar* cv = Console::Get().FindCVar(a[0]);
        if (!cv) { o.Format("unknown cvar: {}", a[0]); return; }
        std::string nv = cv->GetBool() ? "0" : "1";
        Console::Get().Execute(std::string(a[0]) + " " + nv);
    });
    c.RegisterCommand("screenshot", "Capture a screenshot (stub).", [](std::span<const std::string_view>, Output& o) { o.Print("screenshot: stub (renderer capture lands in M1)"); });
    c.RegisterCommand("pix_capture_next_frame", "Trigger a PIX GPU capture (stub).", [](std::span<const std::string_view>, Output& o) { o.Print("pix: stub"); });
    c.RegisterCommand("reload_scene", "Reload the current scene (stub).", [](std::span<const std::string_view>, Output& o) { o.Print("reload_scene: stub"); });
    c.RegisterCommand("physics.dump_islands", "Dump active PhysX islands (stub).", [](std::span<const std::string_view>, Output& o) { o.Print("0 active islands"); });
    c.RegisterCommand("voxel.dump_chunks", "Dump resident voxel chunks (stub).", [](std::span<const std::string_view>, Output& o) { o.Print("0 chunks resident"); });
    // `lua` command is registered by ScriptHost::Init() (engine/script) when scripting is enabled.

    c.RegisterCommand("console.rotate_cert", "Regenerate the TLS cert.", [&server](std::span<const std::string_view>, Output& o) { server.RotateCert(); o.Print("ok"); });
    c.RegisterCommand("console.rotate_sessions", "Invalidate all sessions.", [&server](std::span<const std::string_view>, Output& o) { server.RotateSessions(); o.Print("ok"); });
    c.RegisterCommand("console.list_sessions", "List active sessions.", [&server](std::span<const std::string_view>, Output& o) { o.Print(server.ListSessions()); });

    // --- blue-noise cache utilities ---
    c.RegisterCommand("bluenoise.clear_cache", "Delete the cached blue-noise tile (bluenoise64.bin); restart to re-bake.",
        [](std::span<const std::string_view>, Output& o) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(pf::UserDataDir()) / "bluenoise64.bin", ec);
            o.Print("blue-noise cache cleared - restart the engine to re-bake");
        });
    c.RegisterCommand("bluenoise.open_folder", "Open the engine user-data directory in Explorer.",
        [](std::span<const std::string_view>, Output& o) {
            std::wstring widePath = std::filesystem::path(pf::UserDataDir()).wstring();
            ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            o.Format("opened {}", pf::UserDataDir());
        });

    // --- keybindings management (the web editor drives these via `exec`) ---
    c.RegisterCommand("binds", "List keybindings as JSON.", [&kb](std::span<const std::string_view>, Output& o) {
        std::string j = "[";
        const auto& all = kb.All();
        for (std::size_t i = 0; i < all.size(); ++i) {
            const auto& b = all[i];
            std::string mods;
            const auto names = pf::ModNames(b.modifiers);
            for (std::size_t k = 0; k < names.size(); ++k) { if (k) mods += ","; mods += "\"" + names[k] + "\""; }
            if (i) j += ",";
            j += fmt::format(R"({{"key":"{}","mods":[{}],"target":"{}"}})", b.key, mods, b.target);
        }
        j += "]";
        o.Print(j);
    });
    c.RegisterCommand("bind", "bind <key[+mods]> <target...>", [&kb](std::span<const std::string_view> a, Output& o) {
        if (a.size() < 2) { o.Print("usage: bind <Key|Ctrl+Key> <cvar.toggle X | cvar.set X v | command N>"); return; }
        std::string key;
        std::uint32_t mods;
        ParseKeySpec(std::string(a[0]), key, mods);
        std::string target;
        for (std::size_t i = 1; i < a.size(); ++i) { if (i > 1) target += " "; target += a[i]; }
        kb.Set({key, mods, target});
        kb.Save();
        o.Format("bound {} -> {}", KeySpec(key, mods), target);
    });
    c.RegisterCommand("unbind", "unbind <key[+mods]>", [&kb](std::span<const std::string_view> a, Output& o) {
        if (a.empty()) { o.Print("usage: unbind <Key|Ctrl+Key>"); return; }
        std::string key;
        std::uint32_t mods;
        ParseKeySpec(std::string(a[0]), key, mods);
        bool ok = kb.Remove(key, mods);
        kb.Save();
        o.Print(ok ? "unbound" : "no such binding");
    });
    c.RegisterCommand("bind_reload", "Reload keybindings.toml.", [&kb](std::span<const std::string_view>, Output& o) { kb.Reload(); o.Print("reloaded"); });
    (void)outln;
}

struct Args {
    std::optional<std::string> remote_password;
    std::string cvars_path = "cvars.toml";
    bool no_remote = false;
    bool no_window = false;
    bool reset_cert = false;
    bool show_version = false;
    bool show_help = false;
};
std::string_view ValueOf(std::string_view a) {
    auto e = a.find('=');
    return e == std::string_view::npos ? std::string_view{} : a.substr(e + 1);
}
Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view s = argv[i];
        if (s == "--version") a.show_version = true;
        else if (s == "--help" || s == "-h") a.show_help = true;
        else if (s == "--no-remote") a.no_remote = true;
        else if (s == "--remote") a.no_remote = false;
        else if (s == "--no-window") a.no_window = true;
        else if (s == "--reset-cert") a.reset_cert = true;
        else if (s.rfind("--remote-password=", 0) == 0) a.remote_password = std::string(ValueOf(s));
        else if (s.rfind("--cvars=", 0) == 0) a.cvars_path = std::string(ValueOf(s));
        else if (s.rfind("--vsync=", 0) == 0) {}  // applied post-registration
        else vox::log::Warn("unknown flag: {}", s);
    }
    return a;
}
}  // namespace

int main(int argc, char** argv) {
    using namespace vox::console;
    Args args = ParseArgs(argc, argv);
    if (args.show_version) { vox::log::Info("voxhammer {}", VOX_VERSION_STRING); return 0; }
    if (args.show_help) { Usage(); return 0; }

    vox::log::Info("Voxhammer {} booting", VOX_VERSION_STRING);
    RegisterCoreCvars();
    Console& console = Console::Get();
    vox::platform::LogSummary();
    console.LoadCvarsToml(args.cvars_path);

    for (int i = 1; i < argc; ++i) {
        std::string_view s = argv[i];
        if (s.rfind("--vsync=", 0) == 0) console.Execute(std::string("renderer.vsync ") + (ValueOf(s) == "on" ? "1" : "0"));
    }
    if (args.remote_password) {
        std::string hash = vox::console::HashPassword(*args.remote_password);
        console.SetCVarOverride("remote.password_hash", hash);
        vox::log::Info("stored remote.password_hash ({})", vox::console::HashingIsCryptographic() ? "argon2id" : "PLACEHOLDER");
    }
    if (args.reset_cert) {
        std::error_code ec;
        std::filesystem::remove((std::filesystem::path(vox::platform::UserDataDir()) / "cert" / "cert.pem"), ec);
        vox::log::Info("--reset-cert: removed existing cert (regenerated on bind)");
    }

    // Keybindings.
    pf::Keybindings keybindings;
    keybindings.LoadOrCreateDefaults((std::filesystem::path(vox::platform::UserDataDir()) / "keybindings.toml").string());

    // ConsoleServer.
    ConsoleServer server;
    RegisterCoreCommands(server, keybindings);
    if (!args.no_remote) {
        ConsoleServer::Config cfg;
        if (CVar* p = console.FindCVar("remote.password_hash")) cfg.password_hash = p->value;
        if (CVar* p = console.FindCVar("remote.port_ws")) cfg.http_port = static_cast<std::uint16_t>(p->GetInt());
        if (CVar* p = console.FindCVar("remote.port_tcp")) cfg.line_port = static_cast<std::uint16_t>(p->GetInt());
        if (CVar* p = console.FindCVar("remote.session_ttl_seconds")) cfg.session_ttl_seconds = p->GetInt();
        cfg.cert_dir = (std::filesystem::path(vox::platform::UserDataDir()) / "cert").string();
        server.Start(cfg, &console);
    }

    // Engine subsystems (M1 bring-up): jobs scheduler, ECS, scripting, voxel world.
    vox::jobs::JobScheduler jobs;
    jobs.Init();  // 0 => auto-size from CPU topology
    vox::ecs::EcsWorld ecs;
    ecs.Init();
    vox::script::ScriptHost scripts;
    scripts.Init();  // registers the `lua` console command
    vox::voxel::VoxelWorld world;
    world.Init();

    // Optional .vox import overrides the renderer's procedural demo scene.
    std::vector<std::uint32_t> voxelGrid;
    bool haveVoxels = false;
    if (CVar* ip = console.FindCVar("voxel.import_path"); ip && !ip->value.empty()) {
        vox::voxel::VoxScene vs;
        if (vox::voxel::LoadVox(ip->value, vs)) {
            world = std::move(vs.world);
            voxelGrid = world.BakeFlatGrid(64);  // 64 = renderer kGrid
            haveVoxels = true;
            vox::log::Info("voxel: imported {} ({} voxels, {} chunks)", ip->value, vs.voxelCount, world.ResidentChunks());
        } else {
            vox::log::Warn("voxel: failed to import {}", ip->value);
        }
    }

    // Window + DX12 + key dispatch.
    pf::Window window;
    vox::render::Renderer renderer;
    bool hasWindow = false;
    if (!args.no_window) {
        hasWindow = window.Create(1280, 720, "Voxhammer");
        if (hasWindow) {
            renderer.Init(window.NativeHandle(), window.Width(), window.Height(), haveVoxels ? &voxelGrid : nullptr);
            window.SetKeyHandler([&keybindings](const std::string& key, std::uint32_t mods) {
                keybindings.Dispatch(key, mods, [](const std::string& line) {
                    vox::log::Info("keybind: {}", line);
                    Console::Get().Execute(line);
                });
            });
            vox::log::Info("camera: hold LEFT-MOUSE + WASD/QE to fly (Shift=fast); mouse looks");
        }
    }

    // Engine loop.
    vox::log::Info("entering main loop ({})", hasWindow ? "windowed" : "headless");
    using clk = std::chrono::steady_clock;
    auto prev = clk::now();
    auto lastTele = prev;
    while (g_running.load()) {
        if (hasWindow) {
            window.PollEvents();
            if (window.ShouldClose()) break;
        }
        keybindings.CheckHotReload();
        console.Drain();
        if (server.UnbindRequested() && server.IsRunning()) server.Stop();

        auto now = clk::now();
        float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;

        // Fly-cam: hold LEFT-MOUSE + WASD/QE to move, mouse to look, Shift=fast.
        // Writes the camera.* cvars (so the web console reflects it live).
        if (hasWindow) {
            pf::Window::CameraInput ci = window.PollCameraInput();
            if (ci.active && (ci.move_strafe || ci.move_up || ci.move_fwd || ci.look_dx || ci.look_dy)) {
                float yaw = console.FindCVar("camera.yaw")->GetFloat();
                float pitch = console.FindCVar("camera.pitch")->GetFloat();
                float pos[3] = {0, 0, 0};
                ParseRGB(console.FindCVar("camera.pos")->value, pos[0], pos[1], pos[2]);
                const float PI = 3.14159265f;
                float sens = console.FindCVar("camera.sensitivity")->GetFloat();
                float invx = console.FindCVar("camera.invert_x")->GetBool() ? -1.0f : 1.0f;
                float invy = console.FindCVar("camera.invert_y")->GetBool() ? -1.0f : 1.0f;
                yaw += ci.look_dx * sens * invx;
                pitch -= ci.look_dy * sens * invy;
                while (yaw > PI) yaw -= 2 * PI;
                while (yaw < -PI) yaw += 2 * PI;
                pitch = std::clamp(pitch, -1.5f, 1.5f);
                float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
                float fwd[3] = {cp * sy, sp, cp * cy};
                float rl = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
                float right[3] = {rl > 1e-5f ? -fwd[2] / rl : 1.0f, 0.0f, rl > 1e-5f ? fwd[0] / rl : 0.0f};
                float base = console.FindCVar("camera.move_speed")->GetFloat();
                float speed = (ci.fast ? base * console.FindCVar("camera.boost")->GetFloat() : base) * dt;
                for (int i = 0; i < 3; ++i)
                    pos[i] += (right[i] * ci.move_strafe + (i == 1 ? ci.move_up : 0.0f) + fwd[i] * ci.move_fwd) * speed;
                console.SetCVarOverride("camera.yaw", fmt::format("{:.4f}", yaw));
                console.SetCVarOverride("camera.pitch", fmt::format("{:.4f}", pitch));
                console.SetCVarOverride("camera.pos", fmt::format("{:.2f} {:.2f} {:.2f}", pos[0], pos[1], pos[2]));
            }
        }

        ecs.Step(dt);  // advance ECS systems (kinematics + lifetimes)

        if (renderer.Valid()) {
            vox::render::FrameParams fp;
            ParseRGB(console.FindCVar("renderer.debug.clear_color")->value, fp.clear[0], fp.clear[1], fp.clear[2]);
            ParseRGB(console.FindCVar("camera.pos")->value, fp.cam_pos[0], fp.cam_pos[1], fp.cam_pos[2]);
            fp.cam_yaw = console.FindCVar("camera.yaw")->GetFloat();
            fp.cam_pitch = console.FindCVar("camera.pitch")->GetFloat();
            fp.cam_fov = console.FindCVar("camera.fov")->GetFloat();
            fp.exposure = console.FindCVar("renderer.exposure")->GetFloat();
            fp.hdr = console.FindCVar("renderer.hdr.enabled")->GetBool() ? 1 : 0;
            float saz = console.FindCVar("renderer.sun.azimuth")->GetFloat();
            float sel = console.FindCVar("renderer.sun.elevation")->GetFloat();
            float ce = std::cos(sel);
            fp.sun[0] = ce * std::sin(saz);
            fp.sun[1] = std::sin(sel);
            fp.sun[2] = ce * std::cos(saz);
            fp.ambient = console.FindCVar("renderer.ambient")->GetFloat();
            fp.shadow_softness = console.FindCVar("renderer.shadow.softness")->GetFloat();
            fp.ao_strength = console.FindCVar("renderer.ao.strength")->GetFloat();
            fp.ao_radius = console.FindCVar("renderer.ao.radius")->GetFloat();
            fp.lighting_mode = (console.FindCVar("renderer.lighting.mode")->value == "QUALITY") ? 1 : 0;
            fp.dither = console.FindCVar("renderer.dither")->GetBool() ? 1 : 0;
            fp.ao_samples = console.FindCVar("renderer.ao.samples")->GetInt();
            fp.shadow_samples = console.FindCVar("renderer.shadow.samples")->GetInt();
            fp.gi_samples = console.FindCVar("renderer.gi.samples")->GetInt();
            renderer.RenderFrame(fp);  // vsync caps the loop
        }

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTele).count() >= 100) {
            lastTele = now;
            float fps = dt > 0.0f ? 1.0f / dt : 0.0f;
            bool paused = console.FindCVar("debug.pause_simulation")->GetBool();
            std::string data = fmt::format(
                R"({{"fps":{:.1f},"frame_ms":{:.2f},"gpu_mem_mb":0,"gpu_budget_mb":16384,"chunks":{},"islands":0,"archetypes":{},"denoiser_ms":0,"upscaler_ms":0,"paused":{}}})",
                fps, dt * 1000.0f, world.ResidentChunks(), ecs.EntityCount(), paused ? "true" : "false");
            server.BroadcastEvent("frame_stats", data);
        }

        if (!renderer.Valid()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));  // headless: don't spin
        }
    }

    vox::log::Info("shutting down");
    renderer.Shutdown();
    if (hasWindow) window.Destroy();
    scripts.Shutdown();
    ecs.Shutdown();
    world.Shutdown();
    jobs.Shutdown();
    server.Stop();
    console.SaveCvarsToml(args.cvars_path);
    return 0;
}
