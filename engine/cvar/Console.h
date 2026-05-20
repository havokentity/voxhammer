// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// Voxhammer cvar / command registry. The vocabulary mirrors demont-engine's
// pt::console 1:1 (CVar / Command / Console / Execute, ports 27960/27961) so
// the full network-console port stays a near-copy. Voxhammer's twist over
// demont: persistence is TOML (cvars.toml) with a [meta] schema_version and an
// additive migration policy (ADR-001), instead of demont's plain-text .cfg.
//
// This skeleton-pass build implements register / find / execute / enumerate /
// TOML load+save+migrate. Smart-resolve, undo/redo, favourites, and history
// (which demont's Console also carries) are intentionally deferred -- see TODOs.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

namespace vox::console {

enum CVarFlag : std::uint32_t {
    CVAR_NONE      = 0,
    CVAR_ARCHIVE   = 1u << 0,  // persist to cvars.toml when != default
    CVAR_READONLY  = 1u << 1,  // engine-set; user/network cannot mutate
    CVAR_CHEAT     = 1u << 2,  // gameplay-affecting; gated behind a cheats toggle
    CVAR_DEVELOPER = 1u << 3,  // dev-only visibility in the web UI
};

// Drives the web UI control choice (slider / dropdown / toggle / colour
// picker / vector inputs) and value validation.
enum class CVarType { Int, Float, Bool, Enum, Vec3, Color, String };

struct CVar {
    std::string  name;
    std::string  value;
    std::string  default_value;
    std::string  description;
    CVarType     type  = CVarType::String;
    std::uint32_t flags = CVAR_NONE;

    // For Enum: the accepted values (also drives value-position autocomplete).
    std::vector<std::string> enum_values;

    // For numerics: a draggable range in the web UI when range_max > range_min.
    float range_min  = 0.0f;
    float range_max  = 0.0f;
    float range_step = 0.0f;

    // Fired on every successful value change (engine-side state re-sync).
    std::function<void(const CVar&)> on_change;

    bool live_tunable = true;  // false => requires restart to take effect

    int   GetInt()   const;
    float GetFloat() const;
    bool  GetBool()  const;
};

// Optional metadata bundle for RegisterCVar so callers can set everything in
// one call site without a long positional argument list.
struct CVarParams {
    CVarType     type  = CVarType::String;
    std::uint32_t flags = CVAR_NONE;
    std::vector<std::string> enum_values;
    float range_min  = 0.0f;
    float range_max  = 0.0f;
    float range_step = 0.0f;
    bool  live_tunable = true;
    std::function<void(const CVar&)> on_change;
};

// Output sink handed to command callbacks; its buffer is what Execute returns
// to the caller (and ultimately the remote client over WS / TCP).
class Output {
public:
    void Print(std::string_view s) { buf_.append(s.data(), s.size()); }
    void PrintLine(std::string_view s) { Print(s); buf_.push_back('\n'); }

    template <typename... Args>
    void Format(fmt::format_string<Args...> f, Args&&... args) {
        Print(fmt::format(f, std::forward<Args>(args)...));
    }
    template <typename... Args>
    void FormatLine(fmt::format_string<Args...> f, Args&&... args) {
        PrintLine(fmt::format(f, std::forward<Args>(args)...));
    }

    const std::string& Buffer() const { return buf_; }

private:
    std::string buf_;
};

using CommandCallback =
    std::function<void(std::span<const std::string_view> args, Output& out)>;

struct Command {
    std::string     name;
    std::string     description;
    CommandCallback callback;
};

struct ExecuteResult {
    bool        ok = true;
    std::string output;
    std::string error;
};

class Console {
public:
    static constexpr int kSchemaVersion = 1;  // ADR-001

    static Console& Get();

    CVar*    RegisterCVar(std::string name, std::string default_value,
                          std::string description, CVarParams params = {});
    Command* RegisterCommand(std::string name, std::string description,
                             CommandCallback callback);

    CVar*    FindCVar(std::string_view name);
    Command* FindCommand(std::string_view name);

    // Set bypassing CVAR_READONLY (engine + cfg-load use). Fires on_change.
    bool SetCVarOverride(std::string_view name, std::string_view value);

    // Tokenize + dispatch one console line. Honors `//` comments.
    ExecuteResult Execute(std::string_view line);
    // Multi-statement: newline OR ';' separated; outputs concatenated.
    ExecuteResult ExecuteScript(std::string_view body);

    // Sorted iteration with optional prefix filter (autocomplete + web list).
    void EnumerateCVars(std::string_view prefix, const std::function<void(CVar&)>& visitor);
    void EnumerateCommands(std::string_view prefix, const std::function<void(Command&)>& visitor);

    std::size_t CVarCount() const { return cvars_.size(); }
    std::size_t CommandCount() const { return commands_.size(); }

    // Main-thread task queue. Network threads enqueue work (cvar reads/writes,
    // replies); the engine loop runs Drain() once per frame so all registry
    // access happens on one thread. Keeps the registry lock-free.
    void QueueTask(std::function<void()> task);
    void Drain();

    // --- Persistence (TOML, ADR-001) --------------------------------------
    // Save: writes [meta].schema_version + a [cvars] table of every
    // CVAR_ARCHIVE cvar whose value differs from default, plus any unknown
    // keys preserved from a prior load. Returns lines written or -1 on error.
    int  SaveCvarsToml(const std::string& path);
    // Load: reads schema_version, applies pending additive migrations in
    // sequence, then sets each known cvar (READONLY archived values written
    // through). Unknown cvars are preserved + warned. Returns false on parse
    // error (caller decides whether to abort startup).
    bool LoadCvarsToml(const std::string& path);

private:
    Console() = default;

    // Validate + apply a set; returns an error string (empty == ok). Honors
    // readonly unless `override_readonly`, enum membership, and range clamp.
    std::string ApplySet(CVar& cv, std::string_view value, bool override_readonly);

    // Additive schema migrations: transform the parsed-but-not-yet-applied
    // key/value map from `from_version` up to kSchemaVersion. v1 is the
    // baseline (no-op). Documented in docs/cvar-migrations.md.
    void ApplyMigrations(int from_version, std::map<std::string, std::string>& kv);

    std::map<std::string, CVar, std::less<>>    cvars_;
    std::map<std::string, Command, std::less<>> commands_;

    std::mutex                          task_mutex_;
    std::vector<std::function<void()>>  tasks_;

    // Keys present in a loaded cvars.toml that didn't match a registered cvar.
    // Preserved verbatim so a save round-trips a forward-compatible file.
    std::map<std::string, std::string> unknown_cvars_;
};

// Quote-aware single-line tokenizer ("a b" stays one token). Stops at newline;
// `//` begins a line comment. `storage` backs the returned string_views.
std::vector<std::string_view> TokenizeLine(std::string_view line, std::string& storage);

}  // namespace vox::console

// Static-init registration macros (Construct-On-First-Use keeps order safe).
#define VOX_CVAR(varname, default_value, description, ...)                      \
    static ::vox::console::CVar* varname =                                     \
        ::vox::console::Console::Get().RegisterCVar(                           \
            #varname, default_value, description __VA_OPT__(,) __VA_ARGS__)

#define VOX_COMMAND(varname, description, lambda)                              \
    static ::vox::console::Command* varname =                                  \
        ::vox::console::Console::Get().RegisterCommand(#varname, description, lambda)
