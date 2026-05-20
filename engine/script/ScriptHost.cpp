// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "script/ScriptHost.h"

#include "platform/Log.h"

namespace vox::script {

void ScriptHost::Init() { vox::log::Trace("script: LuaJIT stub init"); }
void ScriptHost::Shutdown() {}

std::string ScriptHost::Eval(std::string_view source) {
    (void)source;
    return "script host stubbed (LuaJIT lands in M6)";
}

}  // namespace vox::script
