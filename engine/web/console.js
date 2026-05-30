// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
// Voxhammer command console -- modern, touch-first HUD logic. Vanilla JS, no
// build step. Speaks the ConsoleServer JSON protocol; when no live server is
// reachable it drives an in-page mock so the HUD is fully interactive offline.
//
// Protocol (matches the ConsoleServer port):
//   send {type:'list_cvars'}    -> {type:'result', cvars:[CVar...]}
//   send {type:'list_commands'} -> {type:'result', commands:[{name,description}...]}
//   send {type:'set_cvar',name,value} -> {type:'result', ok, cvar:CVar, error?}
//   send {type:'exec',line}     -> {type:'result', ok, output?, error?}
//   send {type:'subscribe',topics:[...]}
//   recv events {type:'event', topic:'frame_stats'|'log', ts, data:{...}}
// CVar = {name,value,default,description,type,flags,enum_values,
//         range_min,range_max,range_step,live_tunable}
// type in {'int','float','bool','enum','color','vec3','string'}; flags bits:
// ARCHIVE 1, READONLY 2, CHEAT 4, DEVELOPER 8.
"use strict";
(function () {
const $ = (s, r) => (r || document).querySelector(s);
const el = (t, c, html) => { const e = document.createElement(t); if (c) e.className = c; if (html != null) e.innerHTML = html; return e; };
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const ls = (k, d) => localStorage.getItem(k) || d;
const F = { ARCHIVE: 1, READONLY: 2, CHEAT: 4, DEVELOPER: 8 };

const CATS = {
    renderer:  { label: "Render",   icon: "▣" },
    camera:    { label: "Camera",   icon: "◉" },
    physics:   { label: "Physics",  icon: "⬡" },
    sim:       { label: "Sim",      icon: "≋" },
    voxel:     { label: "World",    icon: "◫" },
    audio:     { label: "Audio",    icon: "♪" },
    debug:     { label: "Debug",    icon: "⚑" },
    profiling: { label: "Profiler", icon: "◷" },
    remote:    { label: "Remote",   icon: "⌬" },
};

// ---------- Graphics quality presets ----------
// Each preset lists the cvar values it sets. The active preset is detected by
// comparing current cvar values against every preset's cvar map.
const GFX_PRESETS = [
    { id: "Low",    label: "Low",    cvars: { "renderer.lighting.mode": "PERFORMANCE", "renderer.ao.samples": "4",  "renderer.shadow.samples": "2", "renderer.dither": "1" } },
    { id: "Medium", label: "Medium", cvars: { "renderer.lighting.mode": "PERFORMANCE", "renderer.ao.samples": "8",  "renderer.shadow.samples": "6", "renderer.dither": "1" } },
    { id: "High",   label: "High",   cvars: { "renderer.lighting.mode": "QUALITY",     "renderer.gi.samples": "1",  "renderer.shadow.samples": "6", "renderer.dither": "1" } },
    { id: "Ultra",  label: "Ultra",  cvars: { "renderer.lighting.mode": "QUALITY",     "renderer.gi.samples": "4",  "renderer.shadow.samples": "8", "renderer.dither": "1" } },
];

// Advanced renderer.* cvars shown in the Graphics "Advanced" subsection.
// Only cvars that actually exist in state.cvars are surfaced.
const GFX_ADVANCED_CVARS = [
    "renderer.lighting.mode",
    "renderer.ao.samples",
    "renderer.ao.strength",
    "renderer.ao.radius",
    "renderer.shadow.samples",
    "renderer.shadow.softness",
    "renderer.gi.samples",
    "renderer.gi.bounces",
    "renderer.gi.denoise",
    "renderer.gi.reproject",
    "renderer.gi.reproject_history",
    "renderer.gi.denoiser",
    "renderer.gi.atrous_iters",
    "renderer.gi.denoise_phi_normal",
    "renderer.gi.denoise_phi_depth",
    "renderer.gi.denoise_phi_lum",
    "renderer.gi.intensity",
    "renderer.gi.emissive",
    "renderer.vox.emissive_boost",
    "renderer.emissive.surface",
    "renderer.empty_space_skip",
    "renderer.dither",
    "renderer.exposure",
    "renderer.ambient",
    "renderer.hdr.enabled",
    "renderer.hdr.peak_nits",
    "renderer.vsync",
    "renderer.upscaling.mode",
    "renderer.frame_gen.factor",
];

function activeGfxPreset() {
    for (const p of GFX_PRESETS) {
        let match = true;
        let checkedAny = false;
        for (const [k, v] of Object.entries(p.cvars)) {
            const cv = state.cvars.get(k);
            // If this cvar doesn't exist in the build, skip it — a missing cvar
            // shouldn't prevent an otherwise-matching preset from being "active".
            if (!cv) continue;
            checkedAny = true;
            if (cv.value !== v) { match = false; break; }
        }
        // Only claim a match if we actually found and checked at least one cvar.
        if (match && checkedAny) return p.id;
    }
    return "Custom";
}
const catOf = (n) => n.split(".")[0];
const shortName = (n) => n.split(".").slice(1).join(".") || n;

const THEMES = [
    { id: "aurora",   c: "#6e8bff" }, { id: "command", c: "#ff9d3d" },
    { id: "tiberium", c: "#8be04a" }, { id: "arclight", c: "#3aa0ff" }, { id: "mono", c: "#cbd5e3" },
];
const SKINS = ["modern", "tactical"];
const DENSITIES = ["comfortable", "compact"];
// One-tap signature looks (set skin + theme together).
const PRESETS = [{ id: "Modern", skin: "modern", theme: "aurora" }, { id: "Classic", skin: "tactical", theme: "command" }];

const ABILITY_DEFS = [
    { name: "screenshot", ico: "▤", lbl: "Shot" },
    { name: "pix_capture_next_frame", ico: "▦", lbl: "PIX" },
    { name: "reload_scene", ico: "⟳", lbl: "Reload" },
    { name: "physics.dump_islands", ico: "⬡", lbl: "Islands" },
    { name: "voxel.dump_chunks", ico: "◫", lbl: "Chunks" },
    { name: "console.rotate_cert", ico: "⚿", lbl: "Cert" },
    { name: "console.list_sessions", ico: "⧉", lbl: "Sessions" },
    { name: "lua", ico: "✦", lbl: "Lua" },
    { name: "bluenoise.clear_cache", ico: "⟲", lbl: "Clear BN", danger: true, hold: 1100 },
    { name: "open_data_folder", ico: "📂", lbl: "AppData" },
    { name: "cvars.reset", ico: "↺", lbl: "Reset Cfg", danger: true, hold: 1100 },
    { name: "keybindings.reset", ico: "⌫", lbl: "Reset Keys", danger: true, hold: 1100 },
    { name: "quit", ico: "⏻", lbl: "Quit", danger: true },
];

// ---------- base64 encoder (handles large buffers in 8 KB chunks) ----------
function bytesToBase64(bytes) {
    const CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    let out = "";
    const CHUNK = 8192;  // process in chunks to avoid call stack overflow
    for (let i = 0; i < u8.length; i += CHUNK) {
        const slice = u8.subarray(i, Math.min(i + CHUNK, u8.length));
        out += String.fromCharCode(...slice);
    }
    return btoa(out);
}

// ---------- .vox upload via WS ----------
function sendVoxFile(file) {
    const reader = new FileReader();
    reader.onload = (ev) => {
        const b64 = bytesToBase64(new Uint8Array(ev.target.result));
        if (state.demo) {
            pushLog("info", "(demo) would load " + file.name + " (" + file.size + " bytes)");
            return;
        }
        bus.send({ type: "load_vox", name: file.name, data: b64 });
        pushLog("info", "uploading " + file.name + " (" + (file.size / 1024).toFixed(1) + " KB)...", "echo");
    };
    reader.onerror = () => pushLog("error", "FileReader error for " + file.name);
    reader.readAsArrayBuffer(file);
}

function attachVoxDropZone(el) {
    el.addEventListener("dragover", (e) => {
        if ([...e.dataTransfer.items].some((i) => i.kind === "file")) {
            e.preventDefault(); el.classList.add("vox-drop-hover");
        }
    });
    el.addEventListener("dragleave", () => el.classList.remove("vox-drop-hover"));
    el.addEventListener("drop", (e) => {
        e.preventDefault(); el.classList.remove("vox-drop-hover");
        const files = [...e.dataTransfer.files].filter((f) => f.name.toLowerCase().endsWith(".vox"));
        if (!files.length) { pushLog("warn", "drop a .vox file"); return; }
        for (const f of files) sendVoxFile(f);
    });
}

// ---------- colour helpers (clear_color is "r g b" floats 0..1) ----------
function floatsToHex(s) {
    const p = String(s).trim().split(/\s+/).map(Number);
    const c = [0, 1, 2].map((i) => clamp(Math.round((p[i] || 0) * 255), 0, 255));
    return "#" + c.map((x) => x.toString(16).padStart(2, "0")).join("");
}
function hexToFloats(hex) {
    const m = /^#?([0-9a-f]{6})$/i.exec(hex); if (!m) return "0 0 0";
    const n = parseInt(m[1], 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255].map((x) => (x / 255).toFixed(3)).join(" ");
}
function floatsToCss(s) {
    const p = String(s).trim().split(/\s+/).map(Number);
    const c = [0, 1, 2].map((i) => clamp(Math.round((p[i] || 0) * 255), 0, 255));
    return `rgb(${c[0]},${c[1]},${c[2]})`;
}

// ---------- mock engine (offline / demo) ----------
function mockCvars() {
    const C = (name, value, type, flags, description, extra) =>
        Object.assign({ name, value, default: value, type, flags: flags || 0, description: description || "",
                        enum_values: [], range_min: 0, range_max: 0, range_step: 0, live_tunable: true }, extra || {});
    const R = (min, max, step) => ({ range_min: min, range_max: max, range_step: step });
    return [
        C("renderer.gi.bounces", "1", "int", F.ARCHIVE, "QUALITY GI path depth (0 = direct only; 1 = single bounce; 2-5 = room fill).", R(0, 5, 1)),
        C("renderer.gi.denoise", "1", "bool", F.ARCHIVE, "QUALITY temporal GI denoise (history across motion)."),
        C("renderer.gi.reproject", "1", "bool", F.ARCHIVE, "QUALITY GI temporal reprojection (realign history across camera motion)."),
        C("renderer.gi.reproject_history", "8", "int", F.ARCHIVE, "Reproject history depth during motion (swim<->grain knob).", R(8, 1024, 8)),
        C("renderer.gi.denoiser", "NONE", "enum", F.ARCHIVE, "GI spatial denoiser: NONE (single pass) / ATROUS / SVGF.", { enum_values: ["NONE", "ATROUS", "SVGF"] }),
        C("renderer.gi.atrous_iters", "5", "int", F.ARCHIVE, "Denoiser a-trous wavelet iterations.", R(1, 6, 1)),
        C("renderer.gi.denoise_phi_normal", "64.0", "float", F.ARCHIVE, "Denoiser normal edge-stop exponent.", R(1, 256, 1)),
        C("renderer.gi.denoise_phi_depth", "1.0", "float", F.ARCHIVE, "Denoiser depth edge-stop scale.", R(0.05, 8, 0.05)),
        C("renderer.gi.denoise_phi_lum", "4.0", "float", F.ARCHIVE, "Denoiser luminance edge-stop sigma (SVGF scales by sqrt(variance)).", R(0.1, 32, 0.1)),
        C("renderer.gi.restir.spatial_passes", "2", "int", F.ARCHIVE, "ReSTIR GI spatial resampling passes.", R(0, 4, 1)),
        C("renderer.upscaling.mode", "DLSS_Q", "enum", F.ARCHIVE, "Super-resolution / upscaler preset.",
          { enum_values: ["DLSS_DLAA", "DLSS_Q", "DLSS_B", "DLSS_P", "DLSS_UP", "FSR_Q", "FSR_B", "FSR_P", "NATIVE"] }),
        C("renderer.frame_gen.factor", "OFF", "enum", F.ARCHIVE, "Frame-generation multiplier.", { enum_values: ["OFF", "2X", "3X", "4X"] }),
        C("renderer.hdr.enabled", "0", "bool", F.ARCHIVE, "HDR10 output (auto-detected display)."),
        C("renderer.hdr.peak_nits", "1000", "int", F.ARCHIVE, "HDR peak luminance target.", R(400, 4000, 50)),
        C("renderer.debug.clear_color", "0.05 0.05 0.08", "color", F.ARCHIVE, "Swapchain clear color (RGB 0..1)."),
        C("renderer.vsync", "0", "bool", F.ARCHIVE, "Vertical sync."),
        C("physics.gpu_rigids.enabled", "1", "bool", F.ARCHIVE, "GPU rigid bodies (NVIDIA)."),
        C("physics.gpu_rigids.max_islands", "10000", "int", F.ARCHIVE, "Max active dynamic islands.", R(256, 16384, 256)),
        C("physics.solver.position_iters", "8", "int", F.ARCHIVE, "Solver position iterations.", R(1, 32, 1)),
        C("sim.fluid.grid_resolution", "MED", "enum", F.ARCHIVE, "FLIP/Eulerian fluid grid resolution.", { enum_values: ["LOW", "MED", "HIGH", "ULTRA"] }),
        C("sim.fire.combustion_rate", "1.0", "float", F.ARCHIVE, "Combustion rate multiplier.", R(0.1, 5.0, 0.1)),
        C("voxel.streaming.horizon_meters", "256", "float", F.ARCHIVE, "Voxel streaming horizon.", R(64, 512, 8)),
        C("voxel.lod.aggressive_eviction", "0", "bool", F.ARCHIVE, "Aggressively evict distant chunks."),
        C("audio.master_volume", "0.8", "float", F.ARCHIVE, "Master output volume.", R(0, 1, 0.01)),
        C("renderer.exposure", "1.0", "float", F.ARCHIVE, "Render exposure (pre-tonemap multiplier).", R(0.1, 4.0, 0.05)),
        C("renderer.sun.azimuth", "0.7", "float", F.ARCHIVE, "Sun azimuth (radians).", R(0.0, 6.2832, 0.02)),
        C("renderer.sun.elevation", "0.6", "float", F.ARCHIVE, "Sun elevation (radians; lower = longer shadows).", R(0.1, 1.5, 0.02)),
        C("renderer.ambient", "0.28", "float", F.ARCHIVE, "Ambient fill light (lower = punchier shadows).", R(0.0, 1.0, 0.01)),
        C("camera.pos", "32 40 -24", "vec3", F.ARCHIVE, "Free-fly camera position (world units)."),
        C("camera.yaw", "0.0", "float", F.ARCHIVE, "Camera yaw (radians).", R(-3.1416, 3.1416, 0.02)),
        C("camera.pitch", "-0.5", "float", F.ARCHIVE, "Camera pitch (radians).", R(-1.5, 1.5, 0.02)),
        C("camera.fov", "1.2", "float", F.ARCHIVE, "Camera vertical FOV (radians).", R(0.4, 2.4, 0.02)),
        C("camera.invert_x", "1", "bool", F.ARCHIVE, "Invert horizontal mouse-look."),
        C("camera.invert_y", "0", "bool", F.ARCHIVE, "Invert vertical mouse-look."),
        C("camera.move_speed", "15", "float", F.ARCHIVE, "Fly-cam move speed (units/sec).", R(1, 120, 1)),
        C("camera.boost", "3.0", "float", F.ARCHIVE, "Fly-cam Shift speed multiplier.", R(1, 10, 0.5)),
        C("camera.sensitivity", "0.0025", "float", F.ARCHIVE, "Mouse-look sensitivity (rad/px).", R(0.0005, 0.01, 0.0005)),
        C("renderer.lighting.mode", "PERFORMANCE", "enum", F.ARCHIVE, "Lighting quality tier.", { enum_values: ["PERFORMANCE", "QUALITY"] }),
        C("renderer.ao.samples", "8", "int", F.ARCHIVE, "Ambient occlusion sample count.", R(1, 32, 1)),
        C("renderer.ao.strength", "0.75", "float", F.ARCHIVE, "Ambient occlusion strength.", R(0.0, 2.0, 0.05)),
        C("renderer.ao.radius", "0.5", "float", F.ARCHIVE, "Ambient occlusion world-space radius.", R(0.05, 4.0, 0.05)),
        C("renderer.shadow.samples", "6", "int", F.ARCHIVE, "Shadow PCF sample count.", R(1, 16, 1)),
        C("renderer.shadow.softness", "1.0", "float", F.ARCHIVE, "Shadow edge softness (penumbra scale).", R(0.0, 4.0, 0.1)),
        C("renderer.gi.samples", "1", "int", F.ARCHIVE, "Global illumination samples per frame.", R(1, 8, 1)),
        C("renderer.dither", "1", "bool", F.ARCHIVE, "Bayer dither to reduce color banding."),
        C("debug.show_brick_grid", "0", "bool", F.DEVELOPER, "Overlay the brick grid."),
        C("debug.pause_simulation", "0", "bool", F.CHEAT, "Pause the simulation."),
        C("profiling.tracy_enabled", "0", "bool", 0, "Stream to the Tracy profiler."),
        C("remote.port_ws", "27960", "int", F.READONLY, "WebSocket/HTTPS port."),
        C("remote.port_tcp", "27961", "int", F.READONLY, "Scripted TCP port."),
        C("remote.session_ttl_seconds", "3600", "int", F.ARCHIVE, "Console session idle TTL.", R(300, 86400, 60)),
        C("remote.password_hash", "argon2id$••••••••••••", "string", F.ARCHIVE | F.READONLY, "argon2id hash of the remote console password."),
    ];
}
const MOCK_COMMANDS = [
    ["screenshot", "Capture a screenshot."], ["pix_capture_next_frame", "Trigger a PIX GPU capture."],
    ["reload_scene", "Reload the current scene."], ["physics.dump_islands", "Dump active PhysX islands."],
    ["voxel.dump_chunks", "Dump resident voxel chunks."], ["console.rotate_cert", "Regenerate the TLS cert."],
    ["console.rotate_sessions", "Invalidate all sessions."], ["console.list_sessions", "List active console sessions."],
    ["lua", "Execute a Lua script."], ["quit", "Quit the engine."], ["version", "Print engine version."],
].map(([name, description]) => ({ name, description }));

class MockBus {
    constructor(emit) {
        this.emit = emit;
        this.cvars = mockCvars();
        this.gpu = 4200;
        this.binds = [
            { key: "Esc", mods: [], target: "command quit" },
            { key: "F11", mods: [], target: "cvar.toggle renderer.hdr.enabled" },
            { key: "F5", mods: [], target: "cvar.toggle debug.pause_simulation" },
            { key: "F3", mods: [], target: "cvar.toggle debug.show_brick_grid" },
        ];
        setTimeout(() => this.tick(), 200);
        setInterval(() => this.tick(), 280);
    }
    find(n) { return this.cvars.find((c) => c.name === n); }
    handle(msg) {
        const reply = (o) => this.emit(Object.assign({ type: "result", ok: true, id: msg.id }, o));
        if (msg.type === "list_cvars") return reply({ cvars: this.cvars.map((c) => Object.assign({}, c)) });
        if (msg.type === "list_commands") return reply({ commands: MOCK_COMMANDS });
        if (msg.type === "set_cvar") return this.set(msg.name, msg.value, reply);
        if (msg.type === "exec") return this.exec(msg.line, reply);
    }
    set(name, value, reply) {
        const cv = this.find(name);
        if (!cv) return reply({ ok: false, error: `unknown cvar '${name}'` });
        if (cv.flags & F.READONLY) return reply({ ok: false, error: `cvar '${name}' is read-only` });
        let v = String(value);
        if (cv.type === "enum" && !cv.enum_values.includes(v)) return reply({ ok: false, error: `'${v}' not valid for '${name}'` });
        if (cv.type === "bool") v = (v === "1" || v === "true" || v === "on" || v === "yes") ? "1" : "0";
        if ((cv.type === "int" || cv.type === "float") && cv.range_max > cv.range_min) {
            const n = clamp(parseFloat(v) || 0, cv.range_min, cv.range_max);
            v = cv.type === "int" ? String(Math.round(n)) : String(n);
        }
        cv.value = v; reply({ cvar: Object.assign({}, cv) });
    }
    exec(line, reply) {
        const parts = String(line).trim().split(/\s+/), name = parts[0];
        if (!name) return reply({});
        const cv = this.find(name);
        if (cv && parts.length > 1) return this.set(name, parts.slice(1).join(" "), reply);
        if (cv) return reply({ output: `${cv.name} = "${cv.value}"  (default "${cv.default}")` });
        if (name === "binds") return reply({ output: JSON.stringify(this.binds) });
        if (name === "bind" && parts.length >= 3) {
            const sp = parts[1].split("+");
            const key = sp.pop();
            this.binds = this.binds.filter((b) => !(b.key === key && b.mods.join() === sp.join()));
            this.binds.push({ key, mods: sp, target: parts.slice(2).join(" ") });
            return reply({ output: "bound" });
        }
        if (name === "unbind" && parts.length >= 2) {
            const sp = parts[1].split("+");
            const key = sp.pop();
            this.binds = this.binds.filter((b) => !(b.key === key && b.mods.join() === sp.join()));
            return reply({ output: "unbound" });
        }
        if (MOCK_COMMANDS.find((c) => c.name === name)) {
            const out = { screenshot: "wrote voxhammer_0001.png", version: "voxhammer 0.1.0",
                "physics.dump_islands": "0 active islands (sim not running)", "voxel.dump_chunks": "0 chunks resident",
                "console.list_sessions": "1 session · this browser · idle 0s",
                "console.rotate_cert": "cert rotated — re-verify fingerprint on reconnect",
                quit: "(demo) the real engine would exit now" }[name] || `${name}: ok`;
            return reply({ output: out });
        }
        return reply({ ok: false, error: `unknown cvar or command: '${name}'` });
    }
    tick() {
        const num = (n) => parseFloat((this.find(n) || {}).value) || 0;
        if ((this.find("debug.pause_simulation") || {}).value === "1") { this.emit({ type: "event", topic: "frame_stats", ts: Date.now(), data: Object.assign({ paused: true }, this.last || {}) }); return; }
        const fg = { OFF: 1, "2X": 1.85, "3X": 2.6, "4X": 3.2 }[(this.find("renderer.frame_gen.factor") || {}).value] || 1;
        let base = clamp(240 - num("renderer.gi.bounces") * 14 - num("renderer.gi.restir.spatial_passes") * 6, 45, 240) * fg + (Math.random() * 14 - 7);
        const fps = Math.max(20, base);
        this.gpu = clamp(this.gpu + (Math.random() * 60 - 28), 3600, 15800);
        this.last = { fps: +fps.toFixed(1), frame_ms: +(1000 / fps).toFixed(2), gpu_mem_mb: Math.round(this.gpu), gpu_budget_mb: 16384,
            chunks: 1280 + (Math.random() * 40 | 0), islands: Math.round(2400 + Math.sin(Date.now() / 1400) * 1800 + Math.random() * 200),
            archetypes: 42, denoiser_ms: +(1.1 + Math.random() * 0.4).toFixed(2), upscaler_ms: +(0.6 + Math.random() * 0.3).toFixed(2), paused: false };
        this.emit({ type: "event", topic: "frame_stats", ts: Date.now(), data: this.last });
    }
}

// ---------- transport (login -> WSS, or offline mock) ----------
let bus = { send() {} };
let sessionToken = null;   // current session token (for reconnect)
let reconnectTimer = null;
function setLink(mode) {
    const link = $("#link");
    link.className = "link " + (mode === "live" ? "link-live" : mode === "demo" ? "link-demo" : mode === "down" ? "link-down" : "link-init");
    $("#link-text").textContent = mode === "live" ? "link" : mode === "demo" ? "demo" : mode === "down" ? "offline" : "init…";
    state.demo = mode === "demo";
}
function startMock() {
    const mock = new MockBus((m) => onMessage(m));
    bus = { mode: "mock", send: (o) => mock.handle(o) };
    setLink("demo");
    boot();
}
function openLiveWs(tok) {
    clearTimeout(reconnectTimer);
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    const ws = new WebSocket(proto + "//" + location.host + "/ws?token=" + encodeURIComponent(tok));
    let opened = false;
    const failT = setTimeout(() => { if (!opened) { try { ws.close(); } catch (e) {} } }, 1600);
    ws.onopen = () => {
        opened = true; clearTimeout(failT); sessionToken = tok; booted = false;
        bus = { mode: "live", ws, send: (o) => { if (ws.readyState === 1) ws.send(JSON.stringify(o)); } };
        setLink("live"); boot();
    };
    ws.onmessage = (e) => { try { onMessage(JSON.parse(e.data)); } catch (x) {} };
    ws.onclose = () => {
        clearTimeout(failT);
        bus = { send() {} };
        setLink("down");
        if (opened) scheduleReconnect();  // lost an established link -> keep retrying
        else showLogin();                 // engine reachable but token rejected (restart) -> re-auth
    };
    ws.onerror = () => {};
}
// Auto-reconnect: while the tab is open, poll for the engine; reuse the session
// if it's still valid (transient blip), else prompt re-login (engine restarted
// -> sessions are in-memory, so re-auth is by design).
function scheduleReconnect() {
    clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(tryReconnect, 2000);
}
async function tryReconnect() {
    try {
        const r = await fetch("/api/login", { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" });
        if (r.status === 401 || r.status === 200 || r.status === 429) {
            if (sessionToken) openLiveWs(sessionToken); else showLogin();
            return;
        }
    } catch (e) { /* engine still down */ }
    scheduleReconnect();  // keep polling until it returns
}
async function doLogin(pw) {
    const msg = $("#login-msg");
    msg.textContent = "";
    try {
        const r = await fetch("/api/login", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ password: pw }) });
        if (r.status === 429) { msg.textContent = "locked out — too many attempts; wait, then retry"; return; }
        const j = await r.json().catch(() => ({}));
        if (r.ok && j.token) { $("#login").hidden = true; openLiveWs(j.token); return; }
        msg.textContent = (j && j.error) ? j.error : "invalid password";
    } catch (e) {
        msg.textContent = "connection failed";
    }
}
function showLogin() {
    $("#login").hidden = false;
    $("#login-pw").focus();
    $("#login-form").onsubmit = (e) => { e.preventDefault(); doLogin($("#login-pw").value); };
}
async function startTransport() {
    setLink("init");
    try {
        // Probe: the real server answers /api/login (401 for an empty body); a
        // plain static host does not. If reachable, show the login modal.
        const r = await fetch("/api/login", { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" });
        if (r.status === 401 || r.status === 200 || r.status === 429) { showLogin(); return; }
    } catch (e) { /* no engine reachable */ }
    startMock();  // offline demo
}

// ---------- state ----------
const state = {
    cvars: new Map(), commands: [], cat: "renderer", search: "", demo: false,
    bindings: null, capturedKey: "",
    pinned: new Set(JSON.parse(ls("vox.pinned", "[]"))),
    logFilters: Object.assign({ info: true, warn: true, error: true }, JSON.parse(ls("vox.logfilters", "{}"))),
};
const persistPins = () => localStorage.setItem("vox.pinned", JSON.stringify([...state.pinned]));
const cssEsc = (s) => s.replace(/["\\]/g, "\\$&");

function setCvar(name, value) {
    state.editName = name;          // remember what the user is actively editing
    state.editAt = Date.now();
    const cv = state.cvars.get(name);
    if (cv) { cv.value = String(value); afterValueChange(name); }
    bus.send({ type: "set_cvar", name, value: String(value) });
}
function afterValueChange(name) {
    const cv = state.cvars.get(name);
    if (cv) { const card = $(`.ctrl[data-name="${cssEsc(name)}"]`); if (card) card.classList.toggle("changed", cv.value !== cv.default); }
    if (name.startsWith("renderer.") || name === "debug.show_brick_grid" || name === "debug.pause_simulation") updateViewport();
    // When the lighting tier OR the GI denoiser mode changes, re-render the deck so
    // context-sensitive cvar visibility (AO vs GI; and the denoiser sub-settings that
    // gray out when the denoiser is NONE) is immediately reflected in both the Render
    // category and the Graphics Advanced panel.
    if (name === "renderer.lighting.mode" || name === "renderer.gi.denoiser") renderDeck();
}
// Merge an incoming cvar in place (keep the object identity) so a widget that
// captured the reference -- e.g. an open color picker -- keeps repainting.
function upsertCvar(data) {
    const cur = state.cvars.get(data.name);
    if (cur) Object.assign(cur, data); else state.cvars.set(data.name, data);
}

// ---------- rail ----------
function renderRail() {
    const rail = $("#rail"); rail.innerHTML = "";
    const counts = {};
    for (const cv of state.cvars.values()) counts[catOf(cv.name)] = (counts[catOf(cv.name)] || 0) + 1;
    for (const id of Object.keys(CATS)) {
        if (!counts[id]) continue;
        const b = el("button", "cat" + (state.cat === id ? " active" : ""));
        b.innerHTML = `<span class="cat-icon">${CATS[id].icon}</span><span>${CATS[id].label}</span><span class="cat-n">${counts[id]}</span>`;
        b.onclick = () => { state.cat = id; renderRail(); renderDeck(); updateFavBtn(); };
        rail.appendChild(b);
    }
    if (state.pinned.size) {
        const b = el("button", "cat" + (state.cat === "pinned" ? " active" : ""));
        b.innerHTML = `<span class="cat-icon">★</span><span>Pinned</span><span class="cat-n">${state.pinned.size}</span>`;
        b.onclick = () => { state.cat = "pinned"; renderRail(); renderDeck(); updateFavBtn(); };
        rail.appendChild(b);
    }
    updateFavBtn();
    // Graphics settings tab (always shown)
    const gfx = el("button", "cat" + (state.cat === "graphics" ? " active" : ""));
    gfx.innerHTML = `<span class="cat-icon">◈</span><span>Graphics</span>`;
    gfx.onclick = () => { state.cat = "graphics"; renderRail(); renderDeck(); updateFavBtn(); };
    rail.appendChild(gfx);
    const kb = el("button", "cat" + (state.cat === "keybindings" ? " active" : ""));
    kb.innerHTML = `<span class="cat-icon">⌨</span><span>Bindings</span>`;
    kb.onclick = () => { state.cat = "keybindings"; renderRail(); renderDeck(); updateFavBtn(); };
    rail.appendChild(kb);
    rail.appendChild(el("div", "rail-sep"));
    const cons = el("button", "cat ghost");
    cons.innerHTML = `<span class="cat-icon">❯</span><span>Console</span>`;
    cons.onclick = openConsole;
    rail.appendChild(cons);
}

// ---------- deck / controls ----------
function visibleCvars() {
    let list = [...state.cvars.values()];
    list = state.cat === "pinned" ? list.filter((c) => state.pinned.has(c.name)) : list.filter((c) => catOf(c.name) === state.cat);
    if (state.search) { const q = state.search.toLowerCase(); list = list.filter((c) => c.name.toLowerCase().includes(q) || (c.description || "").toLowerCase().includes(q)); }
    return list.sort((a, b) => a.name.localeCompare(b.name));
}
function renderDeck() {
    if (state.cat === "keybindings") { renderBindingsDeck(); return; }
    if (state.cat === "graphics") { renderGraphicsDeck(); return; }
    const meta = CATS[state.cat] || { label: "Pinned", icon: "★" };
    $("#deck-icon").textContent = meta.icon;
    $("#deck-name").textContent = meta.label || state.cat;
    const host = $("#controls"); host.innerHTML = "";
    // Inject the vox upload card at the top of the World (voxel) deck.
    if (state.cat === "voxel") { const vc = buildVoxCard(); vc.style.gridColumn = "1 / -1"; host.appendChild(vc); }
    let list = visibleCvars();
    // Context-sensitive inactive state: in the Render (and Pinned) category, cards
    // that don't apply to the current lighting mode are kept in their stable
    // positions but visually dimmed via `ctrl-inactive` — no re-ordering.
    const showLightingHint = (state.cat === "renderer" || state.cat === "pinned");
    if (showLightingHint) {
        const hint = lightingModeHint();
        if (hint && list.some((cv) => isInactiveForLightingMode(cv.name))) {
            const hintEl = el("div", "ctrl-desc"); hintEl.style.gridColumn = "1 / -1"; hintEl.textContent = hint;
            host.appendChild(hintEl);
        }
    }
    $("#deck-count").textContent = `· ${list.length}`;
    for (const cv of list) {
        const inactive = showLightingHint && isInactiveForLightingMode(cv.name);
        host.appendChild(buildControl(cv, inactive));
    }
}
function badgeHtml(cv) {
    let h = "";
    if (cv.flags & F.DEVELOPER) h += `<span class="badge dev">dev</span>`;
    if (cv.flags & F.CHEAT) h += `<span class="badge cheat">cheat</span>`;
    if (cv.flags & F.READONLY) h += `<span class="badge">lock</span>`;
    return h;
}
function buildControl(cv, inactive) {
    const inactiveCls = inactive ? " ctrl-inactive" : "";
    const card = el("div", "ctrl" + (cv.value !== cv.default ? " changed" : "") + inactiveCls); card.dataset.name = cv.name;
    const head = el("div", "ctrl-head");
    head.innerHTML = `<span class="ctrl-name" title="${cv.name}"><span class="ctrl-cat">${catOf(cv.name)}.</span>${shortName(cv.name)}</span><span class="ctrl-badges">${badgeHtml(cv)}${inactive ? `<span class="badge inactive-badge">${inactiveModeLabel()}</span>` : ""}</span>`;
    const tools = el("div", "ctrl-tools");
    const pin = el("button", "icon-btn" + (state.pinned.has(cv.name) ? " pinned" : ""), "★"); pin.title = "pin";
    pin.onclick = () => { state.pinned.has(cv.name) ? state.pinned.delete(cv.name) : state.pinned.add(cv.name); persistPins(); pin.classList.toggle("pinned"); renderRail(); if (state.cat === "pinned") renderDeck(); };
    tools.appendChild(pin);
    if (!(cv.flags & F.READONLY)) { const rst = el("button", "icon-btn", "⟲"); rst.title = "reset to default"; rst.onclick = () => { setCvar(cv.name, cv.default); syncControl(cv.name); }; tools.appendChild(rst); }
    head.appendChild(tools); card.appendChild(head);
    const body = el("div", "ctrl-body"); body.appendChild(buildWidget(cv)); card.appendChild(body);
    card.appendChild(el("div", "ctrl-desc", cv.description || ""));
    return card;
}
function buildWidget(cv) {
    if (cv.flags & F.READONLY) { const r = el("div", "ro-val", cv.value); r.title = cv.value; return r; }
    switch (cv.type) {
        case "bool": return widgetToggle(cv);
        case "enum": return widgetSeg(cv);
        case "color": return widgetColor(cv);
        case "vec3": return widgetVec3(cv);
        case "int": case "float": return (cv.range_max > cv.range_min) ? widgetSlider(cv) : widgetString(cv);
        default: return widgetString(cv);
    }
}
const isOn = (v) => v === "1" || v === "true" || v === "on" || v === "yes";
function widgetToggle(cv) {
    const t = el("div", "toggle" + (isOn(cv.value) ? " on" : "")); t.innerHTML = `<span class="lbl off">off</span><span class="lbl on">on</span><span class="knob"></span>`;
    t.onclick = () => { const nv = t.classList.contains("on") ? "0" : "1"; setCvar(cv.name, nv); t.classList.toggle("on", nv === "1"); };
    return t;
}
function widgetSeg(cv) {
    const seg = el("div", "seg");
    for (const opt of cv.enum_values) { const b = el("button", "seg-opt" + (cv.value === opt ? " selected" : ""), opt); b.onclick = () => { setCvar(cv.name, opt); [...seg.children].forEach((c) => c.classList.toggle("selected", c === b)); }; seg.appendChild(b); }
    return seg;
}
function widgetSlider(cv) {
    const step = cv.range_step > 0 ? cv.range_step : (cv.type === "int" ? 1 : (cv.range_max - cv.range_min) / 100);
    const wrap = el("div", "slider"); const dec = el("button", "nudge", "−");
    const inp = el("input"); inp.type = "range"; inp.min = cv.range_min; inp.max = cv.range_max; inp.step = step; inp.value = cv.value;
    const inc = el("button", "nudge", "+"); const out = el("span", "readout");
    const fmt = (v) => cv.type === "int" ? String(Math.round(v)) : (Math.round(v * 1000) / 1000).toString();
    const paint = () => { out.textContent = fmt(parseFloat(inp.value)); inp.style.setProperty("--fill", ((inp.value - cv.range_min) / (cv.range_max - cv.range_min) * 100) + "%"); };
    const commit = () => { setCvar(cv.name, fmt(parseFloat(inp.value))); paint(); };
    inp.oninput = commit;
    const nudge = (d) => { inp.value = clamp(parseFloat(inp.value) + d * step, cv.range_min, cv.range_max); commit(); };
    dec.onclick = () => nudge(-1); inc.onclick = () => nudge(1);
    inp.onwheel = (e) => { e.preventDefault(); nudge(e.deltaY < 0 ? 1 : -1); };
    paint(); wrap.append(dec, inp, inc, out); return wrap;
}
function widgetColor(cv) {
    const wrap = el("div", "color"); const sw = el("div", "swatch");
    const inp = el("input"); inp.type = "color"; inp.value = floatsToHex(cv.value);
    const txt = el("div"); const hex = el("span", "hex"); const rgb = el("span", "rgb");
    const paint = () => { sw.style.background = floatsToCss(cv.value); hex.textContent = floatsToHex(cv.value); rgb.textContent = cv.value; };
    sw.onclick = () => inp.click();
    inp.oninput = () => { setCvar(cv.name, hexToFloats(inp.value)); paint(); };
    paint(); txt.append(hex, rgb); wrap.append(sw, inp, txt); return wrap;
}
function widgetString(cv) {
    const inp = el("input", "str-in"); inp.type = "text"; inp.value = cv.value; inp.spellcheck = false;
    inp.onchange = () => setCvar(cv.name, inp.value); return inp;
}
function widgetVec3(cv) {
    const wrap = el("div", "vec3");
    const parts = String(cv.value).trim().split(/\s+/);
    const inputs = [];
    ["x", "y", "z"].forEach((ax, i) => {
        const f = el("div", "vec3-field");
        const inp = el("input"); inp.type = "number"; inp.step = "1"; inp.value = parts[i] || "0";
        inp.oninput = () => setCvar(cv.name, inputs.map((x) => x.value || "0").join(" "));
        inputs.push(inp);
        f.append(el("span", "vec3-ax", ax), inp);
        wrap.appendChild(f);
    });
    return wrap;
}
function syncControl(name) {
    const cv = state.cvars.get(name); if (!cv) return;
    const card = $(`.ctrl[data-name="${cssEsc(name)}"]`); if (!card) return;
    card.classList.toggle("changed", cv.value !== cv.default);
    const body = $(".ctrl-body", card); body.innerHTML = ""; body.appendChild(buildWidget(cv));
}

// ---------- keybindings panel ----------
function renderBindingsDeck() {
    $("#deck-icon").textContent = "⌨";
    $("#deck-name").textContent = "Keybindings";
    $("#deck-count").textContent = state.bindings ? `· ${state.bindings.length}` : "";
    const host = $("#controls");
    host.innerHTML = "";
    const list = el("div", "bind-list");
    list.id = "bind-list";
    host.appendChild(list);
    host.appendChild(bindAddCard());
    renderBindingsList();
    bus.send({ type: "exec", line: "binds", id: "__binds__" });  // refresh from engine
}
function renderBindingsList() {
    const list = $("#bind-list");
    if (!list) return;
    list.innerHTML = "";
    const binds = state.bindings || [];
    if (!binds.length) { list.appendChild(el("div", "ctrl-desc", "no bindings — add one below")); }
    for (const b of binds) {
        const spec = (b.mods && b.mods.length ? b.mods.join("+") + "+" : "") + b.key;
        const row = el("div", "bind");
        row.innerHTML = `<span class="keycap">${spec}</span><span class="bind-target">${b.target}</span>`;
        const rm = el("button", "icon-btn", "✕");
        rm.title = "remove";
        rm.onclick = () => { bus.send({ type: "exec", line: "unbind " + spec }); setTimeout(refreshBinds, 60); };
        row.appendChild(rm);
        list.appendChild(row);
    }
}
const refreshBinds = () => bus.send({ type: "exec", line: "binds", id: "__binds__" });
function bindAddCard() {
    const card = el("div", "bind-add");
    const capBtn = el("button", "cap-key", state.capturedKey || "press a key…");
    capBtn.type = "button";
    capBtn.onclick = () => captureKey(capBtn);
    const action = el("select");
    ["cvar.toggle", "command"].forEach((a) => action.appendChild(new Option(a, a)));
    const target = el("select");
    const fill = () => {
        target.innerHTML = "";
        if (action.value === "command") for (const c of state.commands) target.appendChild(new Option(c.name, c.name));
        else for (const cv of [...state.cvars.values()].filter((c) => c.type === "bool" && !(c.flags & F.READONLY))) target.appendChild(new Option(cv.name, cv.name));
    };
    action.onchange = fill;
    fill();
    const add = el("button", "seg-opt", "add binding");
    add.type = "button";
    add.onclick = () => {
        if (!state.capturedKey) { pushLog("warn", "press a key first"); return; }
        if (!target.value) return;
        bus.send({ type: "exec", line: `bind ${state.capturedKey} ${action.value} ${target.value}` });
        state.capturedKey = "";
        setTimeout(refreshBinds, 60);
    };
    card.append(capBtn, action, target, add);
    return card;
}
function captureKey(btn) {
    btn.textContent = "press a key…";
    const onKey = (e) => {
        e.preventDefault();
        const mods = [];
        if (e.ctrlKey) mods.push("Ctrl");
        if (e.shiftKey) mods.push("Shift");
        if (e.altKey) mods.push("Alt");
        let k = "";
        if (/^F\d+$/.test(e.key)) k = e.key;
        else if (e.key === "Escape") k = "Esc";
        else if (e.key === "PrintScreen") k = "PrintScreen";
        else if (e.key === " ") k = "Space";
        else if (e.key.length === 1 && /[a-z0-9]/i.test(e.key)) k = e.key.toUpperCase();
        if (!k) return;
        state.capturedKey = (mods.length ? mods.join("+") + "+" : "") + k;
        btn.textContent = state.capturedKey;
        document.removeEventListener("keydown", onKey, true);
    };
    document.addEventListener("keydown", onKey, true);
}

// ---------- lighting-mode cvar inactive state ----------
// Returns true if the given cvar name is INACTIVE (does not apply) for the current
// lighting mode.  Cards are kept in their stable positions — the caller adds a
// `ctrl-inactive` class instead of removing them from the DOM.
// QUALITY-only GI cvars: all gray out in PERFORMANCE (no GI is computed there).
const LIGHTING_INACTIVE_PERFORMANCE = new Set([
    "renderer.gi.samples", "renderer.gi.bounces", "renderer.gi.intensity",
    "renderer.gi.denoise", "renderer.gi.reproject", "renderer.gi.reproject_history",
    "renderer.gi.denoiser", "renderer.gi.atrous_iters",
    "renderer.gi.denoise_phi_normal", "renderer.gi.denoise_phi_depth", "renderer.gi.denoise_phi_lum",
]);
const LIGHTING_INACTIVE_QUALITY     = new Set(["renderer.ao.samples", "renderer.ao.strength", "renderer.ao.radius"]);
// The a-trous/SVGF sub-settings only apply when a multi-pass denoiser is SELECTED
// (renderer.gi.denoiser != NONE) -- so they gray out in-place when it's NONE. This is
// the "mutually exclusive denoiser, respective settings grayed when not selected" rule.
const DENOISER_SUBSETTINGS = new Set([
    "renderer.gi.atrous_iters", "renderer.gi.denoise_phi_normal",
    "renderer.gi.denoise_phi_depth", "renderer.gi.denoise_phi_lum",
]);
function isInactiveForLightingMode(name) {
    const mode = (state.cvars.get("renderer.lighting.mode") || {}).value || "PERFORMANCE";
    if (mode === "PERFORMANCE") return LIGHTING_INACTIVE_PERFORMANCE.has(name);
    if (mode === "QUALITY") {
        if (LIGHTING_INACTIVE_QUALITY.has(name)) return true;
        // Denoiser sub-settings are live only when ATROUS or SVGF is the chosen denoiser.
        if (DENOISER_SUBSETTINGS.has(name)) {
            const dn = (state.cvars.get("renderer.gi.denoiser") || {}).value || "NONE";
            return dn === "NONE";
        }
    }
    return false;
}
function lightingModeHint() {
    const mode = (state.cvars.get("renderer.lighting.mode") || {}).value || "PERFORMANCE";
    if (mode === "QUALITY")     return "GI mode active — AO cvars inactive";
    if (mode === "PERFORMANCE") return "Performance mode — GI cvars inactive";
    return "";
}
// Returns a short label for the badge shown on inactive cards (e.g. "N/A in GI").
function inactiveModeLabel() {
    const mode = (state.cvars.get("renderer.lighting.mode") || {}).value || "PERFORMANCE";
    if (mode === "QUALITY")     return "N/A in GI";
    if (mode === "PERFORMANCE") return "N/A in Perf";
    return "inactive";
}

// ---------- graphics panel ----------
function renderGraphicsDeck() {
    $("#deck-icon").textContent = "◈";
    $("#deck-name").textContent = "Graphics";
    $("#deck-count").textContent = "";
    const host = $("#controls");
    host.innerHTML = "";

    const q = (state.search || "").toLowerCase().trim();

    // --- Quality Presets section (hidden while searching) ---
    const presetsSection = el("div", "gfx-section");
    presetsSection.appendChild(el("div", "gfx-section-hd", "Quality Presets"));

    const presetsRow = el("div", "gfx-presets");
    const currentPreset = activeGfxPreset();
    for (const p of GFX_PRESETS) {
        const btn = el("button", "gfx-preset" + (currentPreset === p.id ? " active" : ""));
        btn.textContent = p.label;
        btn.title = Object.entries(p.cvars).map(([k, v]) => k + " = " + v).join("\n");
        btn.onclick = () => {
            for (const [k, v] of Object.entries(p.cvars)) {
                if (state.cvars.has(k)) setCvar(k, v);
            }
            // Re-render the deck so the active state reflects the new selection
            // and the Advanced controls update their widget values.
            requestAnimationFrame(() => renderGraphicsDeck());
        };
        presetsRow.appendChild(btn);
    }
    // "Custom" indicator shown when no preset matches
    const customBadge = el("span", "gfx-custom" + (currentPreset === "Custom" ? " visible" : ""), "Custom");
    presetsSection.appendChild(presetsRow);
    presetsSection.appendChild(customBadge);
    if (!q) host.appendChild(presetsSection);

    // --- Advanced section ---
    const advSection = el("div", "gfx-section gfx-advanced-section");
    advSection.appendChild(el("div", "gfx-section-hd", "Advanced"));

    // Mode hint (e.g. "GI mode — AO cvars inactive")
    const hint = lightingModeHint();
    if (hint) advSection.appendChild(el("div", "ctrl-desc", hint));

    const grid = el("div", "gfx-advanced-grid");
    let shown = 0;
    for (const name of GFX_ADVANCED_CVARS) {
        const cv = state.cvars.get(name);
        if (!cv) continue; // gracefully skip missing cvars
        // Honor the search box: filter by cvar name or description.
        if (q && !(name.toLowerCase().includes(q) || (cv.description || "").toLowerCase().includes(q))) continue;
        // Cards are always rendered in their stable order; inactive ones are
        // dimmed and non-interactive rather than removed from the DOM.
        const inactive = isInactiveForLightingMode(name);
        grid.appendChild(buildControl(cv, inactive));
        shown++;
    }
    if (!shown) {
        grid.appendChild(el("div", "ctrl-desc", q ? `No graphics cvars match "${q}".` : "No advanced renderer cvars available in this build."));
    }
    if (q) $("#deck-count").textContent = String(shown);
    advSection.appendChild(grid);
    host.appendChild(advSection);
}

// ---------- vox drop / upload card (injected at the top of the World deck) ----------
function buildVoxCard() {
    const card = el("div", "ctrl vox-upload-card");

    // Header
    const head = el("div", "ctrl-head");
    head.innerHTML = `<span class="ctrl-name"><span class="ctrl-cat">voxel.</span>drop &amp; place</span>`;
    card.appendChild(head);

    // Drop zone
    const zone = el("div", "vox-dropzone", "Drop a .vox file here");
    zone.id = "vox-dropzone";
    attachVoxDropZone(zone);
    card.appendChild(zone);

    // File picker button
    const fileInput = el("input");
    fileInput.type = "file"; fileInput.accept = ".vox"; fileInput.style.display = "none";
    fileInput.multiple = false;
    fileInput.onchange = () => { if (fileInput.files.length) sendVoxFile(fileInput.files[0]); fileInput.value = ""; };
    card.appendChild(fileInput);

    const btnRow = el("div", "vox-btn-row");

    const pick = el("button", "seg-opt", "Browse .vox…");
    pick.type = "button";
    pick.onclick = () => fileInput.click();
    btnRow.appendChild(pick);

    const place = el("button", "seg-opt", "Place");
    place.type = "button";
    place.title = "exec voxel.place (stamp voxel.import_path at cursor)";
    place.onclick = () => { pushLog("info", "❯ voxel.place", "echo"); bus.send({ type: "exec", line: "voxel.place" }); };
    btnRow.appendChild(place);

    const clear = el("button", "seg-opt", "Clear");
    clear.type = "button";
    clear.title = "exec voxel.clear (wipe the world)";
    clear.onclick = () => { if (!confirm("Clear voxel world?")) return; pushLog("info", "❯ voxel.clear", "echo"); bus.send({ type: "exec", line: "voxel.clear" }); };
    btnRow.appendChild(clear);

    card.appendChild(btnRow);

    const desc = el("div", "ctrl-desc", "Drag-drop a .vox or browse — stamped at voxel.cursor.x/y/z. Place re-stamps import_path; Clear wipes world.");
    card.appendChild(desc);

    return card;
}

// ---------- resources + telemetry ----------
const RES = [["fps", "fps", true], ["frame_ms", "ms"], ["gpu_mem_mb", "vram"], ["chunks", "chunks"], ["islands", "islands"]];
function renderResbar() { const bar = $("#resbar"); bar.innerHTML = ""; for (const [k, lbl, hot] of RES) { const d = el("div", "res" + (hot ? " hot" : "")); d.innerHTML = `<b id="res-${k}">—</b><span>${lbl}</span>`; bar.appendChild(d); } }
// ---------- telemetry rolling buffers (fps + frame_ms) ----------
const GRAPH_LEN = 120;
const samples = [];       // fps history (for legacy spark)
const fpsSamples = [];    // fps for graph
const msSamples = [];     // frame_ms for graph

function graphAccent() { return getComputedStyle(document.documentElement).getPropertyValue("--accent").trim() || "#6e8bff"; }
function graphAccent2() { return getComputedStyle(document.documentElement).getPropertyValue("--accent-2").trim() || "#33d6c2"; }

function drawLineGraph(canvasId, data, color, labelId, labelFmt) {
    const c = document.getElementById(canvasId); if (!c) return;
    // Sync canvas pixel size to CSS layout size for crispness
    const dpr = window.devicePixelRatio || 1;
    const rect = c.getBoundingClientRect();
    const cw = Math.round(rect.width * dpr) || c.width;
    const ch = Math.round(rect.height * dpr) || c.height;
    if (c.width !== cw || c.height !== ch) { c.width = cw; c.height = ch; }
    const ctx = c.getContext("2d");
    const w = c.width, h = c.height;
    ctx.clearRect(0, 0, w, h);
    if (data.length < 2) return;

    const max = Math.max(...data), min = Math.min(...data);
    const span = Math.max(max - min, max * 0.05, 0.001); // at least 5% or 0.001 to avoid flat line
    const padT = Math.round(h * 0.08), padB = Math.round(h * 0.08);
    const plotH = h - padT - padB;

    // Faint horizontal grid lines (3)
    ctx.strokeStyle = "rgba(255,255,255,0.06)"; ctx.lineWidth = 1;
    for (let i = 0; i <= 2; i++) {
        const y = padT + i * plotH / 2;
        ctx.beginPath(); ctx.moveTo(0, y + 0.5); ctx.lineTo(w, y + 0.5); ctx.stroke();
    }

    // Fill under the line
    const toY = (v) => padT + plotH - ((v - min) / span) * plotH;
    ctx.beginPath();
    data.forEach((v, i) => {
        const x = (i / (data.length - 1)) * w;
        const y = toY(v);
        i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    });
    ctx.lineTo(w, h); ctx.lineTo(0, h); ctx.closePath();
    // Semi-transparent fill under the line
    ctx.globalAlpha = 0.18; ctx.fillStyle = color; ctx.fill(); ctx.globalAlpha = 1;

    // Stroke the line
    ctx.beginPath();
    data.forEach((v, i) => {
        const x = (i / (data.length - 1)) * w;
        const y = toY(v);
        i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    });
    ctx.strokeStyle = color; ctx.lineWidth = 1.5 * dpr; ctx.lineJoin = "round"; ctx.stroke();

    // Dot at latest sample
    const last = data[data.length - 1];
    const lx = w, ly = toY(last);
    ctx.beginPath(); ctx.arc(lx - 1, ly, 3 * dpr, 0, Math.PI * 2);
    ctx.fillStyle = color; ctx.fill();

    // Update label
    if (labelId) {
        const el2 = document.getElementById(labelId);
        if (el2) el2.textContent = labelFmt ? labelFmt(last) : last.toFixed(1);
    }
}

function onFrameStats(d) {
    $("#tele-state").textContent = d.paused ? "paused" : "live";
    if (d.fps != null) { $("#res-fps").textContent = Math.round(d.fps); $("#fps-num").textContent = Math.round(d.fps); }
    if (d.frame_ms != null) $("#res-frame_ms").textContent = d.frame_ms;
    if (d.gpu_mem_mb != null) $("#res-gpu_mem_mb").textContent = (d.gpu_mem_mb / 1024).toFixed(1) + "g";
    if (d.chunks != null) $("#res-chunks").textContent = d.chunks;
    if (d.islands != null) $("#res-islands").textContent = d.islands;
    if (!d.paused) {
        if (d.fps != null) {
            samples.push(d.fps); if (samples.length > 64) samples.shift();
            fpsSamples.push(d.fps); if (fpsSamples.length > GRAPH_LEN) fpsSamples.shift();
            drawSpark();
            drawLineGraph("graph-fps", fpsSamples, graphAccent(), "tg-fps-val", (v) => Math.round(v) + " fps");
        }
        if (d.frame_ms != null) {
            msSamples.push(d.frame_ms); if (msSamples.length > GRAPH_LEN) msSamples.shift();
            drawLineGraph("graph-ms", msSamples, graphAccent2(), "tg-ms-val", (v) => v.toFixed(2) + " ms");
        }
    }
    renderGauges(d);
}
function renderGauges(d) {
    const rows = [
        ["frame", (d.frame_ms ?? "—") + " ms"],
        ["gpu", `${((d.gpu_mem_mb || 0) / 1024).toFixed(1)} / 16 GB`, (d.gpu_mem_mb || 0) / (d.gpu_budget_mb || 16384)],
        ["islands", d.islands ?? "—"], ["chunks", d.chunks ?? "—"],
        ["denoise", (d.denoiser_ms ?? "—") + " ms"], ["upscale", (d.upscaler_ms ?? "—") + " ms"],
    ];
    const host = $("#gauges"); host.innerHTML = "";
    for (const [k, v, frac] of rows) {
        const g = el("div", "gauge");
        g.innerHTML = `<div class="g-top"><span class="g-k">${k}</span><span class="g-v">${v}</span></div>` + (frac != null ? `<div class="g-bar"><div class="g-fill" style="width:${clamp(frac * 100, 0, 100)}%"></div></div>` : "");
        host.appendChild(g);
    }
}
function drawSpark() {
    const c = $("#spark"), ctx = c.getContext("2d"), w = c.width, h = c.height;
    ctx.clearRect(0, 0, w, h); if (samples.length < 2) return;
    const max = Math.max(...samples) * 1.1, min = Math.min(...samples) * 0.9, span = Math.max(1, max - min);
    const acc = graphAccent();
    ctx.beginPath();
    samples.forEach((s, i) => { const x = i / (samples.length - 1) * w, y = h - (s - min) / span * h; i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); });
    ctx.strokeStyle = acc; ctx.lineWidth = 1.6; ctx.stroke();
    ctx.lineTo(w, h); ctx.lineTo(0, h); ctx.closePath(); ctx.globalAlpha = 0.13; ctx.fillStyle = acc; ctx.fill(); ctx.globalAlpha = 1;
}

// ---------- viewport ----------
const getv = (n) => { const c = state.cvars.get(n); return c ? c.value : undefined; };
function updateViewport() {
    const vp = $("#viewport"); if (!vp) return;
    const cc = getv("renderer.debug.clear_color") || "0 0 0";
    vp.style.background = floatsToCss(cc);
    $("#vp-grid").classList.toggle("on", isOn(getv("debug.show_brick_grid")));
    $("#vp-res").textContent = floatsToHex(cc) + " · clear_color";
    const badges = $("#vp-badges"); badges.innerHTML = "";
    const add = (t, warn) => badges.appendChild(el("span", "vp-badge" + (warn ? " warn" : ""), t));
    add(getv("renderer.upscaling.mode") || "NATIVE");
    const fg = getv("renderer.frame_gen.factor"); if (fg && fg !== "OFF") add("FG " + fg);
    if (isOn(getv("renderer.hdr.enabled"))) add("HDR10");
    if (isOn(getv("renderer.vsync"))) add("VSYNC");
    if (isOn(getv("debug.pause_simulation"))) add("PAUSED", true);
    $("#vp-readout").textContent = `${getv("renderer.gi.bounces") || 0} bounces`;
}

// ---------- log ----------
function pushLog(level, msg, cls) {
    // Mirror every line into BOTH the intel log and the console-overlay log.
    for (const log of [$("#log"), document.getElementById("console-log")]) {
        if (!log) continue;
        const near = log.scrollTop + log.clientHeight > log.scrollHeight - 30;
        const ln = el("div", "ln " + (cls || level)); ln.innerHTML = `<span class="t">${new Date().toLocaleTimeString("en-GB")}</span><span class="m"></span>`;
        $(".m", ln).textContent = msg; ln.dataset.lvl = level; log.appendChild(ln); applyLogFilter(ln);
        if (near) log.scrollTop = log.scrollHeight;
        while (log.children.length > 500) log.removeChild(log.firstChild);
    }
}
function applyLogFilter(ln) { const lvl = ln.dataset.lvl; if (lvl in state.logFilters) ln.style.display = state.logFilters[lvl] ? "" : "none"; }
// Console overlay (unified REPL) open/close.
function openConsole() {
    const o = $("#console-overlay"); if (!o) return;
    o.hidden = false;
    const cl = $("#console-log"); if (cl) cl.scrollTop = cl.scrollHeight;
    const ci = $("#console-modal-input"); if (ci) ci.focus();
}
function closeConsole() { const o = $("#console-overlay"); if (o) o.hidden = true; }
function wireLogFilters() {
    for (const chip of document.querySelectorAll(".chip[data-lvl]")) {
        chip.classList.toggle("on", state.logFilters[chip.dataset.lvl]);
        chip.onclick = () => { state.logFilters[chip.dataset.lvl] = !state.logFilters[chip.dataset.lvl]; chip.classList.toggle("on"); localStorage.setItem("vox.logfilters", JSON.stringify(state.logFilters)); document.querySelectorAll("#log .ln").forEach(applyLogFilter); };
    }
    $("#log-clear").onclick = () => { $("#log").innerHTML = ""; };
}

// ---------- abilities ----------
function renderAbilities() {
    const host = $("#abilities"); host.innerHTML = "";
    const have = new Set(state.commands.map((c) => c.name));
    for (const a of ABILITY_DEFS) {
        if (!have.has(a.name)) continue;
        const b = el("button", "ability" + (a.danger ? " danger" : "") + (a.hold ? " hold" : ""));
        b.innerHTML = `<span class="ab-ico">${a.ico}</span><span class="ab-lbl">${a.lbl}</span>`;
        b.title = (state.commands.find((c) => c.name === a.name) || {}).description || a.name;
        if (a.hold) { b.title += " — press & hold to confirm"; wireHoldAbility(b, a); }
        else b.onclick = () => runAbility(a.name);
        host.appendChild(b);
    }
}
// Press-and-hold-to-confirm: a fill bar wipes across over a.hold ms; releasing
// early cancels. Used for destructive abilities (e.g. keybindings.reset).
function wireHoldAbility(btn, a) {
    const fill = el("span", "ab-fill"); btn.appendChild(fill);
    let timer = 0, firing = false;
    const reset = () => { fill.style.transition = "width .14s ease, opacity .2s"; fill.style.width = "0%"; fill.style.opacity = "0"; btn.classList.remove("arming"); };
    const cancel = () => { if (timer) { clearTimeout(timer); timer = 0; if (!firing) reset(); } };
    const start = (ev) => {
        ev.preventDefault();
        if (timer || firing) return;
        btn.classList.add("arming");
        fill.style.opacity = "1"; fill.style.transition = "width " + a.hold + "ms linear"; fill.style.width = "100%";
        timer = setTimeout(() => {
            timer = 0; firing = true; btn.classList.remove("arming"); fill.style.width = "100%";
            runAbility(a.name);
            setTimeout(() => { firing = false; reset(); }, 220);
        }, a.hold);
    };
    btn.addEventListener("pointerdown", start);
    btn.addEventListener("pointerup", cancel);
    btn.addEventListener("pointerleave", cancel);
    btn.addEventListener("pointercancel", cancel);
}
function runAbility(name) {
    if (name === "lua") { if (document.body.dataset.layout === "narrow") setView("tools"); $("#console-input").value = 'lua "print(\'hi\')"'; $("#console-input").focus(); return; }
    if (name === "quit" && !confirm("Quit the engine?")) return;
    pushLog("info", "❯ " + name, "echo"); bus.send({ type: "exec", line: name });
}

// ---------- settings (skin / theme / density) ----------
function applyPrefs() {
    const r = document.documentElement;
    r.dataset.theme = ls("vox.theme", "aurora"); r.dataset.skin = ls("vox.skin", "modern"); r.dataset.density = ls("vox.density", "comfortable");
}
function setPref(k, v) { localStorage.setItem("vox." + k, v); document.documentElement.dataset[k] = v; }
function renderSettings() {
    const r = document.documentElement;
    const pr = $("#set-preset"); pr.innerHTML = "";
    PRESETS.forEach((p) => {
        const sel = r.dataset.skin === p.skin && r.dataset.theme === p.theme;
        const b = el("button", "seg-opt" + (sel ? " sel" : ""), p.id);
        b.onclick = () => { setPref("skin", p.skin); setPref("theme", p.theme); renderSettings(); drawSpark(); };
        pr.appendChild(b);
    });
    const sk = $("#set-skin"); sk.innerHTML = "";
    SKINS.forEach((s) => { const b = el("button", "seg-opt" + (r.dataset.skin === s ? " sel" : ""), s); b.onclick = () => { setPref("skin", s); renderSettings(); }; sk.appendChild(b); });
    const th = $("#set-theme"); th.innerHTML = "";
    THEMES.forEach((t) => { const b = el("button", "sw-btn" + (r.dataset.theme === t.id ? " sel" : "")); b.style.setProperty("--c", t.c); b.title = t.id; b.onclick = () => { setPref("theme", t.id); renderSettings(); drawSpark(); }; th.appendChild(b); });
    const de = $("#set-density"); de.innerHTML = "";
    DENSITIES.forEach((x) => { const b = el("button", "seg-opt" + (r.dataset.density === x ? " sel" : ""), x); b.onclick = () => { setPref("density", x); renderSettings(); }; de.appendChild(b); });
}
function wireSettings() {
    const pop = $("#settings");
    $("#gear").onclick = (e) => { e.stopPropagation(); pop.hidden = !pop.hidden; };
    document.addEventListener("click", (e) => { if (!pop.hidden && !pop.contains(e.target) && e.target !== $("#gear")) pop.hidden = true; });
}

// ---------- responsive (layout + bottom view tabs) ----------
const mqNarrow = matchMedia("(max-width: 979px)");
function applyLayout() { document.body.dataset.layout = mqNarrow.matches ? "narrow" : "wide"; }
function setView(v) { document.body.dataset.view = v; localStorage.setItem("vox.view", v); for (const b of $("#viewtabs").children) b.classList.toggle("active", b.dataset.view === v); }
function wireViewtabs() { for (const b of $("#viewtabs").children) b.onclick = () => setView(b.dataset.view); setView(ls("vox.view", "controls")); }

// ---------- message handler ----------
function onMessage(msg) {
    if (msg.type === "result") {
        if (msg.id === "__binds__" && typeof msg.output === "string") {
            try { state.bindings = JSON.parse(msg.output); } catch (e) { state.bindings = []; }
            if (state.cat === "keybindings") renderBindingsList();
            return;
        }
        if (Array.isArray(msg.cvars)) { state.cvars = new Map(msg.cvars.map((c) => [c.name, c])); renderRail(); renderDeck(); updateViewport(); }
        if (Array.isArray(msg.commands)) { state.commands = msg.commands; renderAbilities(); }
        if (msg.cvar) { upsertCvar(msg.cvar); afterValueChange(msg.cvar.name); }
        if (msg.output) pushLog("info", msg.output);
        if (msg.error) pushLog("error", msg.error);
    } else if (msg.type === "event") {
        if (msg.topic === "frame_stats") onFrameStats(msg.data || {});
        else if (msg.topic === "log" && msg.data) pushLog(msg.data.level || "info", msg.data.message || "", "evt");
        else if (msg.topic === "cvar" && msg.data && msg.data.name) {
            // Live update from another source (keybinding/TCP/other client).
            const cur = state.cvars.get(msg.data.name);
            const changed = !cur || cur.value !== msg.data.value;
            upsertCvar(msg.data);  // merge in place -- don't orphan open widgets
            // Don't rebuild a control the user is actively editing (a lagging
            // echo of their own drag would destroy the DOM and close the
            // native picker). External changes (F11 etc.) still rebuild.
            const editing = state.editName === msg.data.name && Date.now() - (state.editAt || 0) < 1500;
            if (!editing && changed) syncControl(msg.data.name);
            updateViewport();
        }
    }
}

// ---------- command palette ----------
const palette = {
    open: false,
    focusIdx: -1,
    results: [],
};

function fuzzyMatch(query, text) {
    // Returns { score, html } or null if no match.
    // Simple character-order fuzzy: every char in query must appear in order in text.
    if (!query) return { score: 1, html: escHtml(text) };
    const q = query.toLowerCase(), t = text.toLowerCase();
    let qi = 0, lastIdx = -1, score = 0;
    const positions = [];
    for (let i = 0; i < t.length && qi < q.length; i++) {
        if (t[i] === q[qi]) { positions.push(i); score += (i - lastIdx === 1) ? 3 : 1; lastIdx = i; qi++; }
    }
    if (qi < q.length) return null;
    // Build highlighted HTML
    let html = "", prev = 0;
    for (const pos of positions) {
        html += escHtml(text.slice(prev, pos)) + "<mark>" + escHtml(text[pos]) + "</mark>";
        prev = pos + 1;
    }
    html += escHtml(text.slice(prev));
    return { score, html };
}
function escHtml(s) { return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }

function paletteSearch(query) {
    const q = query.trim();
    const items = [];
    // Cvars
    for (const cv of state.cvars.values()) {
        const m = fuzzyMatch(q, cv.name);
        if (!m && q) {
            // also try description
            const md = fuzzyMatch(q, cv.description || "");
            if (!md) continue;
            items.push({ kind: "cvar", cv, score: md.score * 0.5, nameHtml: escHtml(cv.name), descHtml: md.html });
        } else {
            items.push({ kind: "cvar", cv, score: q ? m.score : 0.5, nameHtml: m ? m.html : escHtml(cv.name), descHtml: escHtml(cv.description || "") });
        }
    }
    // Commands
    for (const cmd of state.commands) {
        const m = fuzzyMatch(q, cmd.name);
        if (!m && q) {
            const md = fuzzyMatch(q, cmd.description || "");
            if (!md) continue;
            items.push({ kind: "cmd", cmd, score: md.score * 0.5, nameHtml: escHtml(cmd.name), descHtml: md.html });
        } else {
            items.push({ kind: "cmd", cmd, score: q ? m.score : 0.5, nameHtml: m ? m.html : escHtml(cmd.name), descHtml: escHtml(cmd.description || "") });
        }
    }
    // Sort: higher score first, then alphabetical
    items.sort((a, b) => (b.score - a.score) || ((a.kind === "cvar" ? a.cv.name : a.cmd.name).localeCompare(b.kind === "cvar" ? b.cv.name : b.cmd.name)));
    return items.slice(0, 60);
}

function renderPaletteResults() {
    const q = $("#palette-input").value;
    const items = paletteSearch(q);
    palette.results = items;
    palette.focusIdx = items.length > 0 ? 0 : -1;
    const host = $("#palette-results");
    host.innerHTML = "";
    if (!items.length) {
        host.innerHTML = `<div class="pal-empty">no matches</div>`;
        return;
    }
    // Split into cvars and commands sections when no query
    const cvars = items.filter((x) => x.kind === "cvar");
    const cmds = items.filter((x) => x.kind === "cmd");
    let idx = 0;
    const addSection = (title, list) => {
        if (!list.length) return;
        const sec = el("div", "pal-section");
        sec.appendChild(el("div", "pal-section-hd", title));
        for (const item of list) {
            const btn = el("button", "pal-item" + (idx === palette.focusIdx ? " focused" : ""));
            btn.dataset.idx = idx;
            if (item.kind === "cvar") {
                const cv = item.cv;
                const catIcon = (CATS[catOf(cv.name)] || {}).icon || "·";
                const pinned = state.pinned.has(cv.name) ? " ★" : "";
                btn.innerHTML = `<span class="pal-item-icon">${catIcon}</span><span class="pal-item-body"><span class="pal-item-name">${item.nameHtml}${escHtml(pinned)}</span><span class="pal-item-desc">${item.descHtml}</span></span><span class="pal-item-value">${escHtml(String(cv.value))}</span><span class="pal-item-badge">${escHtml(cv.type)}</span>`;
                btn.title = cv.name + " = " + cv.value;
                btn.onclick = () => { paletteActivate(item); };
            } else {
                const cmd = item.cmd;
                btn.innerHTML = `<span class="pal-item-icon">❯</span><span class="pal-item-body"><span class="pal-item-name">${item.nameHtml}</span><span class="pal-item-desc">${item.descHtml}</span></span><span class="pal-item-badge cmd">cmd</span>`;
                btn.title = cmd.name;
                btn.onclick = () => { paletteActivate(item); };
            }
            sec.appendChild(btn);
            idx++;
        }
        host.appendChild(sec);
    };
    if (q) {
        addSection("results", items);
    } else {
        addSection("cvars", cvars);
        addSection("commands", cmds);
    }
}

function paletteActivate(item) {
    closePalette();
    if (item.kind === "cmd") {
        pushLog("info", "❯ " + item.cmd.name, "echo");
        if (item.cmd.name === "quit" && !confirm("Quit the engine?")) return;
        bus.send({ type: "exec", line: item.cmd.name });
    } else {
        // Navigate to the cvar
        const cv = item.cv;
        const cat = catOf(cv.name);
        if (cat in CATS) { state.cat = cat; } else { state.cat = "pinned"; }
        state.search = "";
        $("#search").value = "";
        renderRail();
        renderDeck();
        // Switch to controls view on narrow layout
        if (document.body.dataset.layout === "narrow") setView("controls");
        // Scroll to the card
        requestAnimationFrame(() => {
            const card = document.querySelector(`.ctrl[data-name="${cssEsc(cv.name)}"]`);
            if (card) card.scrollIntoView({ behavior: "smooth", block: "center" });
        });
    }
}

function movePaletteFocus(delta) {
    const items = palette.results; if (!items.length) return;
    palette.focusIdx = clamp(palette.focusIdx + delta, 0, items.length - 1);
    const host = $("#palette-results");
    const allBtns = host.querySelectorAll(".pal-item");
    allBtns.forEach((b, i) => b.classList.toggle("focused", i === palette.focusIdx));
    const focused = allBtns[palette.focusIdx];
    if (focused) focused.scrollIntoView({ block: "nearest" });
}

function openPalette() {
    const ov = $("#palette-overlay"); ov.hidden = false;
    palette.open = true;
    const inp = $("#palette-input"); inp.value = ""; inp.focus();
    renderPaletteResults();
}
function closePalette() {
    const ov = $("#palette-overlay"); ov.hidden = true;
    palette.open = false;
}
function wirePalette() {
    $("#palette-btn").onclick = () => palette.open ? closePalette() : openPalette();
    $("#palette-backdrop").onclick = () => closePalette();
    $("#palette-input").oninput = () => renderPaletteResults();
    $("#palette-input").onkeydown = (e) => {
        if (e.key === "Escape") { e.preventDefault(); closePalette(); return; }
        if (e.key === "ArrowDown") { e.preventDefault(); movePaletteFocus(1); return; }
        if (e.key === "ArrowUp") { e.preventDefault(); movePaletteFocus(-1); return; }
        if (e.key === "Enter") {
            e.preventDefault();
            const item = palette.results[palette.focusIdx];
            if (item) paletteActivate(item);
            return;
        }
    };
    // Global Ctrl+K
    document.addEventListener("keydown", (e) => {
        if ((e.ctrlKey || e.metaKey) && e.key === "k") {
            e.preventDefault();
            palette.open ? closePalette() : openPalette();
        }
    });
}

// ---------- favorites / pinned filter ----------
function wireFavoritesBtn() {
    const btn = $("#show-favorites");
    if (!btn) return;
    btn.onclick = () => {
        if (state.cat === "pinned") {
            // toggle off: go back to renderer
            state.cat = "renderer";
            btn.classList.remove("active");
        } else {
            state.cat = "pinned";
            btn.classList.add("active");
        }
        renderRail();
        renderDeck();
    };
}
function updateFavBtn() {
    const btn = $("#show-favorites"); if (!btn) return;
    btn.classList.toggle("active", state.cat === "pinned");
}

// ---------- boot ----------
let booted = false;
function boot() {
    if (booted) return; booted = true;
    bus.send({ type: "list_cvars" }); bus.send({ type: "list_commands" }); bus.send({ type: "subscribe", topics: ["log", "frame_stats", "cvar"] });
    pushLog("info", state.demo ? "demo mode — no live engine; driving an in-page mock" : "connected to engine");
}
function init() {
    applyPrefs(); renderSettings(); wireSettings();
    renderResbar(); renderRail(); renderDeck(); renderGauges({}); wireLogFilters(); wireViewtabs();
    applyLayout(); mqNarrow.addEventListener ? mqNarrow.addEventListener("change", applyLayout) : mqNarrow.addListener(applyLayout);
    // Re-draw graphs when layout resizes (canvas DPR correction)
    if (window.ResizeObserver) {
        const ro = new ResizeObserver(() => {
            if (fpsSamples.length > 1) drawLineGraph("graph-fps", fpsSamples, graphAccent(), "tg-fps-val", (v) => Math.round(v) + " fps");
            if (msSamples.length > 1) drawLineGraph("graph-ms", msSamples, graphAccent2(), "tg-ms-val", (v) => v.toFixed(2) + " ms");
        });
        const tg = document.getElementById("tele-graphs"); if (tg) ro.observe(tg);
    }
    $("#search").oninput = (e) => { state.search = e.target.value; renderDeck(); };
    $("#reset-group").onclick = () => { for (const cv of visibleCvars()) if (!(cv.flags & F.READONLY)) setCvar(cv.name, cv.default); renderDeck(); };
    // Command inputs (bottom strip + console overlay) share persisted history.
    state.cmdHistory = state.cmdHistory || JSON.parse(ls("vox.cmdhist", "[]"));
    function wireCmdInput(inp, form) {
        if (!inp || !form) return;
        let idx = state.cmdHistory.length;
        form.onsubmit = (e) => {
            e.preventDefault();
            const v = inp.value.trim(); if (!v) return;
            pushLog("info", "❯ " + v, "echo"); bus.send({ type: "exec", line: v });
            if (state.cmdHistory[state.cmdHistory.length - 1] !== v) state.cmdHistory.push(v);
            if (state.cmdHistory.length > 100) state.cmdHistory.shift();
            localStorage.setItem("vox.cmdhist", JSON.stringify(state.cmdHistory));
            idx = state.cmdHistory.length; inp.value = "";
        };
        inp.addEventListener("keydown", (e) => {
            if (e.key === "ArrowUp") {
                if (idx > 0) { idx--; inp.value = state.cmdHistory[idx] || ""; e.preventDefault();
                    const n = inp.value.length; requestAnimationFrame(() => inp.setSelectionRange(n, n)); }
            } else if (e.key === "ArrowDown") {
                if (idx < state.cmdHistory.length - 1) { idx++; inp.value = state.cmdHistory[idx] || ""; }
                else { idx = state.cmdHistory.length; inp.value = ""; }
                e.preventDefault();
            }
        });
    }
    wireCmdInput($("#console-input"), $("#console-form"));
    wireCmdInput($("#console-modal-input"), $("#console-modal-form"));
    // Console overlay controls.
    $("#console-backdrop").onclick = closeConsole;
    $("#console-modal-close").onclick = closeConsole;
    $("#console-modal-clear").onclick = () => { const cl = $("#console-log"); if (cl) cl.innerHTML = ""; };
    document.addEventListener("keydown", (e) => { if (e.key === "Escape" && !$("#console-overlay").hidden) closeConsole(); });
    $("#link").onclick = () => pushLog("info", state.demo ? "DEMO: open the engine at https://localhost:27960/ for a live link" : "link active");
    wirePalette();
    wireFavoritesBtn();
    startTransport();
}
init();
})();
