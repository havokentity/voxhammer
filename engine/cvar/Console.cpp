// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "cvar/Console.h"

#include "platform/Log.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>

#include <toml++/toml.h>

namespace vox::console {

// --- CVar value coercion ----------------------------------------------------

int CVar::GetInt() const {
    int v = 0;
    std::from_chars(value.data(), value.data() + value.size(), v);
    return v;
}

float CVar::GetFloat() const {
    float v = 0.0f;
    std::from_chars(value.data(), value.data() + value.size(), v);
    return v;
}

bool CVar::GetBool() const {
    return value == "1" || value == "true" || value == "on" || value == "yes";
}

// --- Singleton --------------------------------------------------------------

Console& Console::Get() {
    static Console instance;
    return instance;
}

// --- Registration -----------------------------------------------------------

CVar* Console::RegisterCVar(std::string name, std::string default_value,
                            std::string description, CVarParams params) {
    auto [it, inserted] = cvars_.try_emplace(name);
    CVar& cv = it->second;
    if (!inserted) {
        vox::log::Warn("cvar '{}' registered twice; keeping first", name);
        return &cv;
    }
    cv.name          = std::move(name);
    cv.value         = default_value;
    cv.default_value = std::move(default_value);
    cv.description   = std::move(description);
    cv.type          = params.type;
    cv.flags         = params.flags;
    cv.enum_values   = std::move(params.enum_values);
    cv.range_min     = params.range_min;
    cv.range_max     = params.range_max;
    cv.range_step    = params.range_step;
    cv.live_tunable  = params.live_tunable;
    cv.on_change     = std::move(params.on_change);
    return &cv;
}

Command* Console::RegisterCommand(std::string name, std::string description,
                                  CommandCallback callback) {
    auto [it, inserted] = commands_.try_emplace(name);
    Command& cmd = it->second;
    if (!inserted) {
        vox::log::Warn("command '{}' registered twice; keeping first", name);
        return &cmd;
    }
    cmd.name        = std::move(name);
    cmd.description = std::move(description);
    cmd.callback    = std::move(callback);
    return &cmd;
}

CVar* Console::FindCVar(std::string_view name) {
    auto it = cvars_.find(name);
    return it == cvars_.end() ? nullptr : &it->second;
}

Command* Console::FindCommand(std::string_view name) {
    auto it = commands_.find(name);
    return it == commands_.end() ? nullptr : &it->second;
}

// --- Set with validation ----------------------------------------------------

std::string Console::ApplySet(CVar& cv, std::string_view value, bool override_readonly) {
    if ((cv.flags & CVAR_READONLY) && !override_readonly) {
        return fmt::format("cvar '{}' is read-only", cv.name);
    }

    std::string v(value);

    if (cv.type == CVarType::Enum && !cv.enum_values.empty()) {
        if (std::find(cv.enum_values.begin(), cv.enum_values.end(), v) == cv.enum_values.end()) {
            std::string allowed;
            for (std::size_t k = 0; k < cv.enum_values.size(); ++k) {
                if (k) {
                    allowed += ", ";
                }
                allowed += cv.enum_values[k];
            }
            return fmt::format("'{}' not a valid value for '{}'; expected one of [{}]", v, cv.name, allowed);
        }
    } else if (cv.type == CVarType::Bool) {
        bool b = (v == "1" || v == "true" || v == "on" || v == "yes");
        v = b ? "1" : "0";
    } else if ((cv.type == CVarType::Int || cv.type == CVarType::Float) && cv.range_max > cv.range_min) {
        if (cv.type == CVarType::Int) {
            int n = 0;
            std::from_chars(v.data(), v.data() + v.size(), n);
            n = std::clamp(n, static_cast<int>(cv.range_min), static_cast<int>(cv.range_max));
            v = std::to_string(n);
        } else {
            float f = 0.0f;
            std::from_chars(v.data(), v.data() + v.size(), f);
            f = std::clamp(f, cv.range_min, cv.range_max);
            v = fmt::format("{}", f);
        }
    }

    cv.value = std::move(v);
    if (cv.on_change) {
        cv.on_change(cv);
    }
    return {};
}

bool Console::SetCVarOverride(std::string_view name, std::string_view value) {
    CVar* cv = FindCVar(name);
    if (!cv) {
        return false;
    }
    return ApplySet(*cv, value, /*override_readonly=*/true).empty();
}

// --- Tokenizer --------------------------------------------------------------

std::vector<std::string_view> TokenizeLine(std::string_view line, std::string& storage) {
    struct Span {
        std::size_t off;
        std::size_t len;
    };
    std::vector<Span> spans;
    storage.clear();

    std::size_t i = 0;
    const std::size_t n = line.size();
    while (i < n) {
        // skip whitespace
        while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i >= n) {
            break;
        }
        // `//` comment terminates the line
        if (line[i] == '/' && i + 1 < n && line[i + 1] == '/') {
            break;
        }

        std::size_t off = storage.size();
        if (line[i] == '"') {
            ++i;  // opening quote
            while (i < n && line[i] != '"') {
                storage.push_back(line[i]);
                ++i;
            }
            if (i < n) {
                ++i;  // closing quote
            }
        } else {
            while (i < n && !std::isspace(static_cast<unsigned char>(line[i]))) {
                if (line[i] == '/' && i + 1 < n && line[i + 1] == '/') {
                    break;
                }
                storage.push_back(line[i]);
                ++i;
            }
        }
        spans.push_back({off, storage.size() - off});
    }

    // Build views only after `storage` is final (no further reallocation).
    std::vector<std::string_view> tokens;
    tokens.reserve(spans.size());
    for (const Span& s : spans) {
        tokens.emplace_back(storage.data() + s.off, s.len);
    }
    return tokens;
}

// --- Execute ----------------------------------------------------------------

ExecuteResult Console::Execute(std::string_view line) {
    std::string storage;
    std::vector<std::string_view> tokens = TokenizeLine(line, storage);
    if (tokens.empty()) {
        return {};
    }

    std::string_view name = tokens[0];
    std::span<const std::string_view> args(tokens.data() + 1, tokens.size() - 1);

    if (Command* cmd = FindCommand(name)) {
        Output out;
        cmd->callback(args, out);
        return {true, out.Buffer(), {}};
    }

    if (CVar* cv = FindCVar(name)) {
        if (args.empty()) {  // get
            return {true,
                    fmt::format("{} = \"{}\"  (default \"{}\")  -- {}", cv->name, cv->value,
                                cv->default_value, cv->description),
                    {}};
        }
        // set: join remaining tokens so vec3/color "1 2 3" works unquoted.
        std::string joined;
        for (std::size_t k = 0; k < args.size(); ++k) {
            if (k) {
                joined.push_back(' ');
            }
            joined.append(args[k]);
        }
        std::string err = ApplySet(*cv, joined, /*override_readonly=*/false);
        if (!err.empty()) {
            return {false, {}, err};
        }
        vox::log::Info("cvar {} = {}", cv->name, cv->value);
        return {true, fmt::format("{} = \"{}\"", cv->name, cv->value), {}};
    }

    return {false, {}, fmt::format("unknown cvar or command: '{}'", name)};
}

ExecuteResult Console::ExecuteScript(std::string_view body) {
    ExecuteResult agg;
    std::string stmt;
    auto flush = [&]() {
        if (stmt.find_first_not_of(" \t\r\n") == std::string::npos) {
            stmt.clear();
            return;
        }
        ExecuteResult r = Execute(stmt);
        if (!r.output.empty()) {
            agg.output += r.output;
            agg.output.push_back('\n');
        }
        if (!r.ok) {
            agg.ok = false;
            if (agg.error.empty()) {
                agg.error = r.error;
            }
        }
        stmt.clear();
    };
    for (char c : body) {
        if (c == '\n' || c == ';') {
            flush();
        } else {
            stmt.push_back(c);
        }
    }
    flush();
    return agg;
}

// --- Main-thread task queue -------------------------------------------------

void Console::QueueTask(std::function<void()> task) {
    std::lock_guard<std::mutex> lk(task_mutex_);
    tasks_.push_back(std::move(task));
}

void Console::Drain() {
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lk(task_mutex_);
        batch.swap(tasks_);
    }
    for (auto& t : batch) {
        t();
    }
}

// --- Enumeration ------------------------------------------------------------

void Console::EnumerateCVars(std::string_view prefix, const std::function<void(CVar&)>& visitor) {
    for (auto& [name, cv] : cvars_) {
        if (prefix.empty() || name.compare(0, prefix.size(), prefix) == 0) {
            visitor(cv);
        }
    }
}

void Console::EnumerateCommands(std::string_view prefix, const std::function<void(Command&)>& visitor) {
    for (auto& [name, cmd] : commands_) {
        if (prefix.empty() || name.compare(0, prefix.size(), prefix) == 0) {
            visitor(cmd);
        }
    }
}

// --- TOML persistence (ADR-001) ---------------------------------------------

void Console::ApplyMigrations(int from_version, std::map<std::string, std::string>& kv) {
    // Additive, never destructive. Each step transforms `kv` from version K to
    // K+1. v1 is the baseline -- no transforms yet. New steps documented in
    // docs/cvar-migrations.md.
    (void)kv;
    if (from_version < 1) {
        // v0 -> v1: baseline adoption (no key renames/removals).
        from_version = 1;
    }
    // future: while (from_version < kSchemaVersion) { ...; ++from_version; }
}

bool Console::LoadCvarsToml(const std::string& path) {
    // Missing file is not an error -- the engine regenerates defaults on save.
    {
        std::ifstream probe(path);
        if (!probe.good()) {
            vox::log::Info("no cvars file at {}; using defaults", path);
            return true;
        }
    }

    toml::table root;
    try {
        root = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        vox::log::Error("cvars.toml parse error in {}: {}", path, std::string(e.description()));
        return false;
    } catch (...) {
        vox::log::Error("cvars.toml: unknown error reading {}", path);
        return false;
    }

    int file_version = root["meta"]["schema_version"].value<int>().value_or(0);

    std::map<std::string, std::string> kv;
    if (const auto* cvars = root["cvars"].as_table()) {
        for (auto&& [k, v] : *cvars) {
            if (auto s = v.value<std::string>()) {
                kv.emplace(std::string(k.str()), *s);
            }
        }
    }

    ApplyMigrations(file_version, kv);

    std::size_t applied = 0;
    unknown_cvars_.clear();
    for (auto& [key, value] : kv) {
        if (FindCVar(key)) {
            SetCVarOverride(key, value);  // archived READONLY values write through
            ++applied;
        } else {
            vox::log::Warn("unrecognized cvar '{}' preserved from {}", key, path);
            unknown_cvars_.emplace(key, value);
        }
    }

    vox::log::Info("loaded {} cvars from {} (schema v{} -> v{})", applied, path, file_version,
                   kSchemaVersion);
    return true;
}

int Console::SaveCvarsToml(const std::string& path) {
    toml::table root;

    toml::table meta;
    meta.insert_or_assign("schema_version", static_cast<int64_t>(kSchemaVersion));
    meta.insert_or_assign("engine", "voxhammer");
    root.insert_or_assign("meta", std::move(meta));

    toml::table cvars;
    int count = 0;
    for (auto& [name, cv] : cvars_) {
        if ((cv.flags & CVAR_ARCHIVE) && cv.value != cv.default_value) {
            cvars.insert_or_assign(name, cv.value);
            ++count;
        }
    }
    for (auto& [key, value] : unknown_cvars_) {
        cvars.insert_or_assign(key, value);
        ++count;
    }
    root.insert_or_assign("cvars", std::move(cvars));

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        vox::log::Error("could not write {}", path);
        return -1;
    }
    f << "# Voxhammer cvars -- auto-generated. schema_version drives migration (ADR-001).\n";
    f << root << '\n';
    vox::log::Info("saved {} archived cvars to {}", count, path);
    return count;
}

}  // namespace vox::console
