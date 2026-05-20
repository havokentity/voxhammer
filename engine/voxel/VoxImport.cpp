// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "voxel/VoxImport.h"

#include "platform/Log.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace vox::voxel {

// ---------------------------------------------------------------------------
// MagicaVoxel default palette (256 RGBA entries).
// Index 0 is unused by convention; indices 1-255 map to solid voxel colours.
// Values are 0xAABBGGRR (little-endian RGBA on disk becomes ABGR in memory).
// ---------------------------------------------------------------------------
// Default MagicaVoxel palette – 255 non-zero colours at indices 1..255.
// Index 0 is always unused/transparent. Initialized lazily as grey gradient
// for indices not covered by a real palette; the RGBA chunk overrides at load time.
// We keep this simple: a 7-level RGB ramp (7^3 = 343 > 255) reduced to fit.
// The exact colours only matter for rendering, not for the integration contract.
static void FillDefaultPalette(VoxPalette& pal) {
    pal[0] = 0x00000000u;  // unused
    // Simple greyscale ramp for a recognisable default.
    for (int i = 1; i < 256; ++i) {
        const auto v = static_cast<std::uint8_t>(i);
        pal[static_cast<std::size_t>(i)] =
            0xFF000000u |
            (static_cast<std::uint32_t>(v) << 16) |
            (static_cast<std::uint32_t>(v) <<  8) |
            static_cast<std::uint32_t>(v);
    }
}

// ---------------------------------------------------------------------------
// Low-level reader helpers
// ---------------------------------------------------------------------------
namespace {

struct Reader {
    const std::uint8_t* data;
    std::size_t         size;
    std::size_t         pos = 0;

    bool canRead(std::size_t n) const noexcept { return pos + n <= size; }

    std::uint8_t u8() {
        if (!canRead(1)) { pos = size + 1; return 0; }
        return data[pos++];
    }
    std::uint32_t u32() {
        if (!canRead(4)) { pos = size + 1; return 0; }
        std::uint32_t v = 0;
        std::memcpy(&v, data + pos, 4);
        pos += 4;
        return v;
    }
    void skip(std::size_t n) { pos += n; }

    bool readTag(char tag[4]) {
        if (!canRead(4)) return false;
        std::memcpy(tag, data + pos, 4);
        pos += 4;
        return true;
    }
};

bool matchTag(const char a[4], const char* b) {
    return std::memcmp(a, b, 4) == 0;
}

bool ParseVox(Reader& r, VoxScene& out) {
    // File header: "VOX " + version uint32
    char magic[4];
    if (!r.readTag(magic)) return false;
    if (!matchTag(magic, "VOX ")) {
        vox::log::Error("voximp: not a .vox file (bad magic)");
        return false;
    }
    r.u32();  // version (150 or 200), ignored

    // MAIN chunk
    char tag[4];
    if (!r.readTag(tag) || !matchTag(tag, "MAIN")) {
        vox::log::Error("voximp: missing MAIN chunk");
        return false;
    }
    r.u32();  // MAIN selfBytes (always 0)
    r.u32();  // MAIN childBytes

    // Load default palette; RGBA chunk may override below.
    FillDefaultPalette(out.palette);

    out.world.Init();
    out.sizeX = out.sizeY = out.sizeZ = 0;
    out.voxelCount = 0;

    // Walk child chunks until EOF.
    while (r.canRead(12)) {
        if (!r.readTag(tag)) break;
        const std::uint32_t selfBytes  = r.u32();
        const std::uint32_t childBytes = r.u32();
        const std::size_t   selfEnd    = r.pos + selfBytes;
        (void)childBytes;

        if (matchTag(tag, "SIZE")) {
            out.sizeX = r.u32();
            out.sizeY = r.u32();
            out.sizeZ = r.u32();
        } else if (matchTag(tag, "XYZI")) {
            const std::uint32_t nVox = r.u32();
            for (std::uint32_t i = 0; i < nVox; ++i) {
                if (!r.canRead(4)) break;
                const std::uint8_t x = r.u8();
                const std::uint8_t y = r.u8();
                const std::uint8_t z = r.u8();
                std::uint8_t       m = r.u8();
                if (m == 0) m = 1;  // spec: palette index 0 unused in XYZI; clamp
                out.world.SetVoxel(static_cast<int>(x),
                                   static_cast<int>(y),
                                   static_cast<int>(z), m);
                ++out.voxelCount;
            }
        } else if (matchTag(tag, "RGBA")) {
            for (int i = 0; i < 256 && r.canRead(4); ++i) {
                out.palette[i] = r.u32();
            }
        }

        // Skip any unread self-data (handles unknown/future chunks).
        if (r.pos < selfEnd) r.skip(selfEnd - r.pos);
    }

    vox::log::Info("voximp: loaded {}x{}x{} | {} voxels | {} chunks",
                   out.sizeX, out.sizeY, out.sizeZ,
                   out.voxelCount, out.world.ResidentChunks());
    return true;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool LoadVoxFromMemory(const std::uint8_t* data, std::size_t len, VoxScene& out) {
    if (!data || len < 12) {
        vox::log::Error("voximp: buffer too small ({} bytes)", len);
        return false;
    }
    Reader r{data, len};
    return ParseVox(r, out);
}

bool LoadVox(const std::string& path, VoxScene& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        vox::log::Error("voximp: cannot open '{}'", path);
        return false;
    }
    const auto sz = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::uint8_t> buf(sz);
    if (!f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz))) {
        vox::log::Error("voximp: read error '{}'", path);
        return false;
    }
    return LoadVoxFromMemory(buf.data(), buf.size(), out);
}

}  // namespace vox::voxel
