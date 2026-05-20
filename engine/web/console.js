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
    { name: "quit", ico: "⏻", lbl: "Quit", danger: true },
];

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
        C("renderer.gi.bounces", "3", "int", F.ARCHIVE, "Diffuse GI bounce count.", R(0, 6, 1)),
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
        C("camera.pos", "32 40 -24", "vec3", F.ARCHIVE, "Free-fly camera position (world units)."),
        C("camera.yaw", "0.0", "float", F.ARCHIVE, "Camera yaw (radians).", R(-3.1416, 3.1416, 0.02)),
        C("camera.pitch", "-0.5", "float", F.ARCHIVE, "Camera pitch (radians).", R(-1.5, 1.5, 0.02)),
        C("camera.fov", "1.2", "float", F.ARCHIVE, "Camera vertical FOV (radians).", R(0.4, 2.4, 0.02)),
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
        b.onclick = () => { state.cat = id; renderRail(); renderDeck(); };
        rail.appendChild(b);
    }
    if (state.pinned.size) {
        const b = el("button", "cat" + (state.cat === "pinned" ? " active" : ""));
        b.innerHTML = `<span class="cat-icon">★</span><span>Pinned</span><span class="cat-n">${state.pinned.size}</span>`;
        b.onclick = () => { state.cat = "pinned"; renderRail(); renderDeck(); };
        rail.appendChild(b);
    }
    const kb = el("button", "cat" + (state.cat === "keybindings" ? " active" : ""));
    kb.innerHTML = `<span class="cat-icon">⌨</span><span>Bindings</span>`;
    kb.onclick = () => { state.cat = "keybindings"; renderRail(); renderDeck(); };
    rail.appendChild(kb);
    rail.appendChild(el("div", "rail-sep"));
    const cons = el("button", "cat ghost");
    cons.innerHTML = `<span class="cat-icon">❯</span><span>Console</span>`;
    cons.onclick = () => { if (document.body.dataset.layout === "narrow") setView("tools"); $("#console-input").focus(); };
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
    const meta = CATS[state.cat] || { label: "Pinned", icon: "★" };
    $("#deck-icon").textContent = meta.icon;
    $("#deck-name").textContent = meta.label || state.cat;
    const host = $("#controls"); host.innerHTML = "";
    const list = visibleCvars();
    $("#deck-count").textContent = `· ${list.length}`;
    for (const cv of list) host.appendChild(buildControl(cv));
}
function badgeHtml(cv) {
    let h = "";
    if (cv.flags & F.DEVELOPER) h += `<span class="badge dev">dev</span>`;
    if (cv.flags & F.CHEAT) h += `<span class="badge cheat">cheat</span>`;
    if (cv.flags & F.READONLY) h += `<span class="badge">lock</span>`;
    return h;
}
function buildControl(cv) {
    const card = el("div", "ctrl" + (cv.value !== cv.default ? " changed" : "")); card.dataset.name = cv.name;
    const head = el("div", "ctrl-head");
    head.innerHTML = `<span class="ctrl-name"><span class="ctrl-cat">${catOf(cv.name)}.</span>${shortName(cv.name)}</span><span class="ctrl-badges">${badgeHtml(cv)}</span>`;
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

// ---------- resources + telemetry ----------
const RES = [["fps", "fps", true], ["frame_ms", "ms"], ["gpu_mem_mb", "vram"], ["chunks", "chunks"], ["islands", "islands"]];
function renderResbar() { const bar = $("#resbar"); bar.innerHTML = ""; for (const [k, lbl, hot] of RES) { const d = el("div", "res" + (hot ? " hot" : "")); d.innerHTML = `<b id="res-${k}">—</b><span>${lbl}</span>`; bar.appendChild(d); } }
const samples = [];
function onFrameStats(d) {
    $("#tele-state").textContent = d.paused ? "paused" : "live";
    if (d.fps != null) { $("#res-fps").textContent = Math.round(d.fps); $("#fps-num").textContent = Math.round(d.fps); }
    if (d.frame_ms != null) $("#res-frame_ms").textContent = d.frame_ms;
    if (d.gpu_mem_mb != null) $("#res-gpu_mem_mb").textContent = (d.gpu_mem_mb / 1024).toFixed(1) + "g";
    if (d.chunks != null) $("#res-chunks").textContent = d.chunks;
    if (d.islands != null) $("#res-islands").textContent = d.islands;
    if (!d.paused && d.fps != null) { samples.push(d.fps); if (samples.length > 64) samples.shift(); drawSpark(); }
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
    const acc = getComputedStyle(document.documentElement).getPropertyValue("--accent").trim() || "#6e8bff";
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
    const log = $("#log"); const near = log.scrollTop + log.clientHeight > log.scrollHeight - 30;
    const ln = el("div", "ln " + (cls || level)); ln.innerHTML = `<span class="t">${new Date().toLocaleTimeString("en-GB")}</span><span class="m"></span>`;
    $(".m", ln).textContent = msg; ln.dataset.lvl = level; log.appendChild(ln); applyLogFilter(ln);
    if (near) log.scrollTop = log.scrollHeight;
    while (log.children.length > 500) log.removeChild(log.firstChild);
}
function applyLogFilter(ln) { const lvl = ln.dataset.lvl; if (lvl in state.logFilters) ln.style.display = state.logFilters[lvl] ? "" : "none"; }
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
        const b = el("button", "ability" + (a.danger ? " danger" : "")); b.innerHTML = `<span class="ab-ico">${a.ico}</span><span class="ab-lbl">${a.lbl}</span>`;
        b.title = (state.commands.find((c) => c.name === a.name) || {}).description || a.name; b.onclick = () => runAbility(a.name);
        host.appendChild(b);
    }
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
    $("#search").oninput = (e) => { state.search = e.target.value; renderDeck(); };
    $("#reset-group").onclick = () => { for (const cv of visibleCvars()) if (!(cv.flags & F.READONLY)) setCvar(cv.name, cv.default); renderDeck(); };
    $("#console-form").onsubmit = (e) => { e.preventDefault(); const v = $("#console-input").value.trim(); if (!v) return; pushLog("info", "❯ " + v, "echo"); bus.send({ type: "exec", line: v }); $("#console-input").value = ""; };
    $("#link").onclick = () => pushLog("info", state.demo ? "DEMO: open the engine at https://localhost:27960/ for a live link" : "link active");
    startTransport();
}
init();
})();
