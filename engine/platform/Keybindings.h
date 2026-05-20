// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// Keybindings -- the in-engine action surface that complements the web console.
// Each binding maps a key (+ modifiers) to a target that resolves to a console
// line: `cvar.toggle <path>`, `cvar.set <path> <value>`, or `command <name>
// [args...]`. Loaded from keybindings.toml in the user data dir (created with
// sensible defaults on first run), hot-reloadable on file change, editable via
// the web console.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vox::platform {

enum KeyMod : std::uint32_t { MOD_NONE = 0, MOD_SHIFT = 1u << 0, MOD_CTRL = 1u << 1, MOD_ALT = 1u << 2 };

struct KeyBinding {
    std::string   key;        // "F11", "Esc", "PrintScreen", "A", ...
    std::uint32_t modifiers;  // KeyMod bitmask
    std::string   target;     // "cvar.toggle <path>" | "cvar.set <p> <v>" | "command <name> [args]"
};

class Keybindings {
public:
    // Load from `path`; if missing, write the defaults and use them.
    void LoadOrCreateDefaults(const std::string& path);
    bool Load(const std::string& path);
    int  Save() const;            // returns bindings written, or -1 on error
    void Reload();                // re-read from the current path
    bool CheckHotReload();        // reload if the file's mtime changed; true if reloaded
    const std::string& Path() const { return path_; }

    // Resolve the binding matching (key, modifiers) and emit its console line
    // via `exec_line`. Returns true iff a binding fired.
    bool Dispatch(const std::string& key, std::uint32_t modifiers,
                  const std::function<void(const std::string&)>& exec_line) const;

    // CRUD (web editor). Set adds or replaces the binding for (key, modifiers).
    const std::vector<KeyBinding>& All() const { return bindings_; }
    void Set(const KeyBinding& b);
    bool Remove(const std::string& key, std::uint32_t modifiers);

private:
    std::string             path_;
    std::vector<KeyBinding>  bindings_;
    std::int64_t            last_write_ = 0;  // file mtime (ns) for hot-reload
};

std::vector<KeyBinding> DefaultBindings();

// "Shift+Ctrl" <-> bitmask helpers (used by the toml + web protocol).
std::uint32_t ParseMods(const std::vector<std::string>& names);
std::vector<std::string> ModNames(std::uint32_t mods);

// Translate a binding's target into a console line. Empty if malformed.
std::string TargetToConsoleLine(const std::string& target);

}  // namespace vox::platform
