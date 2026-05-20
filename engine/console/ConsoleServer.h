// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// Network ConsoleServer -- the web-only control surface. WebSocket + HTTPS on
// one port, raw line-protocol TCP on a second (automation; TODO). Security
// model: self-signed TLS, argon2id password auth, HttpOnly+Secure session
// cookies + a matching WS query-param token (anti-CSRF), Origin enforcement,
// and rate-limited auth. The JSON protocol is handled on the engine's main
// thread via Console's task queue, so the cvar registry stays lock-free.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct mg_context;
struct mg_connection;

namespace vox::console {

class Console;

class ConsoleServer {
public:
    struct Config {
        std::string   bind_address = "127.0.0.1";  // never bound externally
        std::uint16_t http_port    = 27960;        // HTTPS + WSS
        std::uint16_t line_port    = 27961;        // scripted TCP automation

        // argon2id hash of the remote password. EMPTY => the server does not
        // bind (no escape hatch). Set via --remote-password.
        std::string   password_hash;

        bool          enable_tls   = true;
        std::string   cert_dir;                     // %APPDATA%/Voxhammer/cert

        int session_ttl_seconds    = 3600;
        int fails_for_lockout      = 5;
        int lockout_seconds        = 60;
    };

    ConsoleServer();
    ~ConsoleServer();
    ConsoleServer(const ConsoleServer&)            = delete;
    ConsoleServer& operator=(const ConsoleServer&) = delete;

    // Binds the WS/HTTPS listener. Returns false (and logs the fix) when the
    // password is unset or the cert can't be generated. On success, wires
    // vox::log's sink to BroadcastEvent("log", ...).
    bool Start(const Config& cfg, Console* console);
    void Stop();
    bool IsRunning() const { return running_; }

    // Set after 20 failed auths; the engine loop should Stop() the server.
    bool UnbindRequested() const;

    // Push a telemetry event to clients subscribed to `topic`. Envelope:
    // {"type":"event","topic":..,"ts":..,"data":<data_json>}
    void BroadcastEvent(std::string_view topic, std::string_view data_json);

    // Authed session operations (exposed as console.* commands by the host).
    void RotateCert();
    void RotateSessions();
    std::string ListSessions() const;

private:
    // civetweb callbacks (dispatch via cbdata == this).
    static int  HttpHandler(mg_connection* conn, void* cbdata);
    static int  WsConnect(const mg_connection* conn, void* cbdata);
    static void WsReady(mg_connection* conn, void* cbdata);
    static int  WsData(mg_connection* conn, int bits, char* data, std::size_t len, void* cbdata);
    static void WsClose(const mg_connection* conn, void* cbdata);

    int  HandleLogin(mg_connection* conn);
    void HandleMessage(const mg_connection* conn, const std::string& text);  // main thread
    void SendWs(const mg_connection* conn, const std::string& text);

    // TCP line-protocol automation (port line_port): plain socket on
    // bind_address, password-authed on the first line, then one console line
    // per line. Dedicated threads; execs route through Console::QueueTask.
    void StartLineServer();
    void LineAcceptLoop();
    void LineClientLoop(std::uintptr_t client);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    Config   config_;
    Console* console_ = nullptr;
    bool     running_ = false;
};

}  // namespace vox::console
