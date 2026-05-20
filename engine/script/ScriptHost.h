// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

#include <string>
#include <string_view>

namespace vox::script {

// LuaJIT host + FFI direct to engine structs / ECS components. File-watcher
// hot-reload; mods under /mods/<name>/. Lua runs live via the web console's
// `lua "<script>"` command. Stub this pass (M6).
class ScriptHost {
public:
    void Init();
    void Shutdown();
    std::string Eval(std::string_view source);  // returns result/diagnostic
};

}  // namespace vox::script
