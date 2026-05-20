// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// Minimal header-only logging facility. Foundational so any subsystem can use
// it without a link dependency (all functions inline). A single optional sink
// lets the ConsoleServer forward log lines to the web "log" topic.

#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace vox::log {

enum class Level { Trace, Info, Warn, Error };

inline const char* LevelName(Level l) {
    switch (l) {
    case Level::Trace: return "trace";
    case Level::Info:  return "info";
    case Level::Warn:  return "warn";
    case Level::Error: return "error";
    }
    return "info";
}

// Optional global sink (e.g. ConsoleServer broadcasting the "log" topic).
// Function-local static keeps the facility header-only.
using Sink = std::function<void(Level, const std::string&)>;
inline Sink& GlobalSink() {
    static Sink sink;
    return sink;
}
inline void SetSink(Sink s) { GlobalSink() = std::move(s); }

inline std::mutex& LogMutex() {
    static std::mutex m;
    return m;
}

inline void Emit(Level level, const std::string& body) {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[16];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

    {
        std::lock_guard<std::mutex> lk(LogMutex());
        std::FILE* out = (level == Level::Error || level == Level::Warn) ? stderr : stdout;
        fmt::print(out, "[{}] [{:>5}] {}\n", ts, LevelName(level), body);
        std::fflush(out);
    }
    if (auto& s = GlobalSink()) {
        s(level, body);
    }
}

template <typename... Args>
inline void Info(fmt::format_string<Args...> f, Args&&... args) {
    Emit(Level::Info, fmt::format(f, std::forward<Args>(args)...));
}
template <typename... Args>
inline void Warn(fmt::format_string<Args...> f, Args&&... args) {
    Emit(Level::Warn, fmt::format(f, std::forward<Args>(args)...));
}
template <typename... Args>
inline void Error(fmt::format_string<Args...> f, Args&&... args) {
    Emit(Level::Error, fmt::format(f, std::forward<Args>(args)...));
}
template <typename... Args>
inline void Trace(fmt::format_string<Args...> f, Args&&... args) {
    Emit(Level::Trace, fmt::format(f, std::forward<Args>(args)...));
}

}  // namespace vox::log
