// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "platform/Keybindings.h"

#include "platform/Log.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <toml++/toml.h>

namespace vox::platform {

std::vector<KeyBinding> DefaultBindings() {
    return {
        {"Esc", MOD_NONE, "command quit"},
        {"F1", MOD_NONE, "cvar.toggle editor.active"},
        {"F2", MOD_NONE, "command screenshot"},
        {"F3", MOD_NONE, "cvar.toggle debug.show_brick_grid"},
        {"F4", MOD_NONE, "cvar.toggle debug.show_physx_wireframe"},
        {"F5", MOD_NONE, "cvar.toggle debug.pause_simulation"},
        {"F6", MOD_NONE, "command physics.dump_islands"},
        {"F7", MOD_NONE, "command setcursor"},
        {"X", MOD_NONE, "command voxel.break"},
        {"F11", MOD_NONE, "cvar.toggle renderer.hdr.enabled"},
        {"PrintScreen", MOD_NONE, "command pix_capture_next_frame"},
    };
}

std::uint32_t ParseMods(const std::vector<std::string>& names) {
    std::uint32_t m = MOD_NONE;
    for (const std::string& n : names) {
        if (n == "Shift") m |= MOD_SHIFT;
        else if (n == "Ctrl") m |= MOD_CTRL;
        else if (n == "Alt") m |= MOD_ALT;
    }
    return m;
}

std::vector<std::string> ModNames(std::uint32_t mods) {
    std::vector<std::string> out;
    if (mods & MOD_CTRL) out.push_back("Ctrl");
    if (mods & MOD_SHIFT) out.push_back("Shift");
    if (mods & MOD_ALT) out.push_back("Alt");
    return out;
}

std::string TargetToConsoleLine(const std::string& target) {
    std::istringstream in(target);
    std::string kind;
    in >> kind;
    std::string rest;
    std::getline(in, rest);
    if (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);

    if (kind == "command") return rest;
    if (kind == "cvar.set") return rest;                 // "<path> <value>"
    if (kind == "cvar.toggle") return "toggle " + rest;  // resolved by the `toggle` console command
    vox::log::Warn("keybinding: unknown target kind '{}'", kind);
    return {};
}

void Keybindings::Set(const KeyBinding& b) {
    for (KeyBinding& e : bindings_) {
        if (e.key == b.key && e.modifiers == b.modifiers) {
            e.target = b.target;
            return;
        }
    }
    bindings_.push_back(b);
}

bool Keybindings::Remove(const std::string& key, std::uint32_t modifiers) {
    auto it = std::remove_if(bindings_.begin(), bindings_.end(),
                             [&](const KeyBinding& b) { return b.key == key && b.modifiers == modifiers; });
    if (it == bindings_.end()) return false;
    bindings_.erase(it, bindings_.end());
    return true;
}

void Keybindings::ResetToDefaults() {
    bindings_ = DefaultBindings();
    Save();
    vox::log::Info("keybindings reset to defaults ({} bindings)", bindings_.size());
}

bool Keybindings::Dispatch(const std::string& key, std::uint32_t modifiers,
                           const std::function<void(const std::string&)>& exec_line) const {
    for (const KeyBinding& b : bindings_) {
        if (b.key == key && b.modifiers == modifiers) {
            std::string line = TargetToConsoleLine(b.target);
            if (!line.empty() && exec_line) {
                exec_line(line);
            }
            return true;
        }
    }
    return false;
}

static std::int64_t MtimeOf(const std::string& path) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return t.time_since_epoch().count();
}

bool Keybindings::Load(const std::string& path) {
    path_ = path;
    toml::table root;
    try {
        root = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        vox::log::Error("keybindings.toml parse error: {}", std::string(e.description()));
        return false;
    } catch (...) {
        return false;
    }

    std::vector<KeyBinding> loaded;
    if (const auto* arr = root["bind"].as_array()) {
        for (auto&& node : *arr) {
            const auto* t = node.as_table();
            if (!t) continue;
            KeyBinding b;
            b.key = (*t)["key"].value_or(std::string{});
            b.target = (*t)["target"].value_or(std::string{});
            std::vector<std::string> mods;
            if (const auto* m = (*t)["mods"].as_array()) {
                for (auto&& e : *m) {
                    if (auto s = e.value<std::string>()) mods.push_back(*s);
                }
            }
            b.modifiers = ParseMods(mods);
            if (b.key.empty() || b.target.empty()) continue;
            // Conflict policy: keep first, warn on duplicate (key, mods).
            bool dup = std::any_of(loaded.begin(), loaded.end(), [&](const KeyBinding& x) {
                return x.key == b.key && x.modifiers == b.modifiers;
            });
            if (dup) {
                vox::log::Warn("keybindings: duplicate binding for '{}' ignored", b.key);
                continue;
            }
            loaded.push_back(std::move(b));
        }
    }
    bindings_ = std::move(loaded);
    last_write_ = MtimeOf(path_);
    return true;
}

void Keybindings::LoadOrCreateDefaults(const std::string& path) {
    path_ = path;
    if (std::filesystem::exists(path)) {
        Load(path);
        vox::log::Info("loaded {} keybindings from {}", bindings_.size(), path);
    } else {
        bindings_ = DefaultBindings();
        Save();
        vox::log::Info("wrote default keybindings to {}", path);
    }
}

int Keybindings::Save() const {
    toml::array binds;
    for (const KeyBinding& b : bindings_) {
        toml::table t;
        t.insert("key", b.key);
        toml::array mods;
        for (const std::string& n : ModNames(b.modifiers)) mods.push_back(n);
        t.insert("mods", std::move(mods));
        t.insert("target", b.target);
        binds.push_back(std::move(t));
    }
    toml::table root;
    root.insert("bind", std::move(binds));

    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    if (!f) {
        vox::log::Error("could not write {}", path_);
        return -1;
    }
    f << "# Voxhammer keybindings -- editable here or via the web console.\n";
    f << "# target: 'cvar.toggle <path>' | 'cvar.set <path> <value>' | 'command <name> [args]'\n";
    f << root << '\n';
    return static_cast<int>(bindings_.size());
}

void Keybindings::Reload() {
    if (!path_.empty()) Load(path_);
}

bool Keybindings::CheckHotReload() {
    if (path_.empty()) return false;
    std::int64_t m = MtimeOf(path_);
    if (m != 0 && m != last_write_) {
        Reload();
        vox::log::Info("keybindings hot-reloaded ({} bindings)", bindings_.size());
        return true;
    }
    return false;
}

}  // namespace vox::platform
