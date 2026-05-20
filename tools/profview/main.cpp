// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// profview -- Voxhammer profile-data viewer.
//
// APPROACH: JSON-lines frame-stats file parser (no live engine required).
//
// The engine writes frame telemetry to a JSON-lines file (one JSON object per
// line) when launched with  --frame-stats-out <path>.  Each line has at least:
//   {"frame":<n>,"fps":<f>,"frame_ms":<ms>}
//
// profview reads the file and prints a min/avg/max summary for both fps and
// frame_ms.
//
// Usage:
//   profview <path-to-frame-stats.jsonl>
//   profview --help
//
// Exit codes: 0 = success, 1 = usage/IO error, 2 = no valid frames in file.
//
// NOTE: The TCP automation path (port 27961) is described in ConsoleServer.h.
//       A future pass can add a --live <password> flag that connects via
//       Winsock, sends the password on line 1, issues "r_fps" / "r_frame_ms"
//       cvar queries, and streams results here. The JSON-lines path is chosen
//       for M0 because it is fully verifiable without a running engine.

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

#include <fmt/core.h>

// ---------------------------------------------------------------------------
// Minimal JSON-lines parser
// We only need to extract numeric values for known keys from flat JSON objects.
// Using std::from_chars keeps the zero-dependency promise.
// ---------------------------------------------------------------------------

static bool ParseDouble(std::string_view s, double& out) {
    // Trim surrounding whitespace
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.remove_suffix(1);
    if (s.empty()) return false;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{};
}

// Returns the string value for `key` in a flat JSON object line, or empty.
// Only handles simple {"key":value,...} where value is a number or string
// (no nested objects).
static std::string ExtractValue(std::string_view line, std::string_view key) {
    // Build the search pattern  "key":
    std::string pattern;
    pattern.reserve(key.size() + 3);
    pattern += '"';
    pattern.append(key.data(), key.size());
    pattern += '"';
    pattern += ':';

    auto pos = line.find(pattern);
    if (pos == std::string_view::npos) return {};

    std::size_t vstart = pos + pattern.size();
    // Skip optional whitespace
    while (vstart < line.size() && line[vstart] == ' ') ++vstart;
    if (vstart >= line.size()) return {};

    char first = line[vstart];
    std::size_t vend = vstart;

    if (first == '"') {
        // String value — skip to closing quote
        ++vend;
        while (vend < line.size() && line[vend] != '"') ++vend;
        return std::string(line.substr(vstart + 1, vend - vstart - 1));
    } else {
        // Number value — read until comma, } or whitespace
        while (vend < line.size() && line[vend] != ',' && line[vend] != '}' &&
               line[vend] != ' ' && line[vend] != '\t' && line[vend] != '\r' && line[vend] != '\n') {
            ++vend;
        }
        return std::string(line.substr(vstart, vend - vstart));
    }
}

// ---------------------------------------------------------------------------

struct FrameStats {
    double fps_min      =  std::numeric_limits<double>::infinity();
    double fps_max      = -std::numeric_limits<double>::infinity();
    double fps_sum      = 0.0;
    double frame_ms_min =  std::numeric_limits<double>::infinity();
    double frame_ms_max = -std::numeric_limits<double>::infinity();
    double frame_ms_sum = 0.0;
    long long count     = 0;
    long long skipped   = 0;
};

static FrameStats ParseFile(std::ifstream& file) {
    FrameStats st;
    std::string line;
    bool first_line = true;
    while (std::getline(file, line)) {
        // Strip CR for Windows line endings
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Strip UTF-8 BOM (EF BB BF) from the very first line
        if (first_line) {
            first_line = false;
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }
        // Skip blank / comment lines
        if (line.empty() || line.front() == '#' || line.front() == '/') continue;
        // Must start with '{'
        if (line.front() != '{') { ++st.skipped; continue; }

        std::string fps_str      = ExtractValue(line, "fps");
        std::string frame_ms_str = ExtractValue(line, "frame_ms");

        double fps = 0.0, frame_ms = 0.0;
        bool have_fps      = ParseDouble(fps_str, fps);
        bool have_frame_ms = ParseDouble(frame_ms_str, frame_ms);

        if (!have_fps && !have_frame_ms) { ++st.skipped; continue; }

        if (have_fps) {
            st.fps_sum += fps;
            st.fps_min = std::min(st.fps_min, fps);
            st.fps_max = std::max(st.fps_max, fps);
        }
        if (have_frame_ms) {
            st.frame_ms_sum += frame_ms;
            st.frame_ms_min = std::min(st.frame_ms_min, frame_ms);
            st.frame_ms_max = std::max(st.frame_ms_max, frame_ms);
        }
        ++st.count;
    }
    return st;
}

static void PrintHelp(const char* prog) {
    fmt::print("Usage: {} <frame-stats.jsonl>\n", prog);
    fmt::print("\n");
    fmt::print("Reads a JSON-lines frame-stats file produced by the Voxhammer engine\n");
    fmt::print("(--frame-stats-out <path>) and prints min/avg/max fps and frame_ms.\n");
    fmt::print("\n");
    fmt::print("Each line must be a flat JSON object containing at least one of:\n");
    fmt::print("  {{\"frame\":<n>, \"fps\":<f>, \"frame_ms\":<ms>}}\n");
    fmt::print("\n");
    fmt::print("Exit codes: 0=ok  1=usage/IO  2=no valid frames\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintHelp(argv[0]);
        return 1;
    }

    std::string_view arg1(argv[1]);
    if (arg1 == "--help" || arg1 == "-h" || arg1 == "/h" || arg1 == "/?") {
        PrintHelp(argv[0]);
        return 0;
    }

    std::ifstream file(argv[1]);
    if (!file.is_open()) {
        fmt::print(stderr, "profview: cannot open '{}': {}\n", argv[1], std::strerror(errno));
        return 1;
    }

    FrameStats st = ParseFile(file);

    if (st.count == 0) {
        fmt::print(stderr, "profview: no valid frame-stats records found in '{}'\n", argv[1]);
        if (st.skipped > 0) {
            fmt::print(stderr, "  ({} lines skipped as non-matching)\n", st.skipped);
        }
        return 2;
    }

    double fps_avg      = st.fps_sum      / static_cast<double>(st.count);
    double frame_ms_avg = st.frame_ms_sum / static_cast<double>(st.count);

    fmt::print("profview: '{}' -- {} frames", argv[1], st.count);
    if (st.skipped) fmt::print(" ({} skipped)", st.skipped);
    fmt::print("\n");
    fmt::print("  fps       min={:.1f}  avg={:.1f}  max={:.1f}\n",
               st.fps_min, fps_avg, st.fps_max);
    fmt::print("  frame_ms  min={:.3f}  avg={:.3f}  max={:.3f}\n",
               st.frame_ms_min, frame_ms_avg, st.frame_ms_max);

    return 0;
}
