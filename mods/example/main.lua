-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 Rajesh D'Monte
-- Example Voxhammer mod entry point. The LuaJIT host + FFI bindings to the
-- engine (cvars, commands, ECS) land in M6; this file documents the intended
-- shape. It is not executed in the skeleton pass.

local vox = require("vox") -- engine FFI table (provided by the host in M6)

-- Register a console command callable from the web console.
vox.command("hello_voxhammer", "Example mod command.", function(args, out)
    out:print("hello from the example mod!")
end)

-- React to a cvar change.
vox.on_cvar_change("renderer.gi.bounces", function(cv)
    vox.log.info("example mod sees gi bounces = " .. cv.value)
end)
