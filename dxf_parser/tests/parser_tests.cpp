#include <catch2/catch_test_macros.hpp>

#include "DxfParser.h"
#include "DxfTypes.h"

#include <atomic>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// TEST_DATA_DIR is injected by CMake: points to docs/sample_data/.
static const fs::path kDataDir = TEST_DATA_DIR;

// Verify that at least one tile_*_lod0.bin file exists in dir and has the
// correct binary magic header.
static bool hasBinFiles(const fs::path& dir) {
    if (!fs::is_directory(dir)) return false;
    for (auto& entry : fs::directory_iterator(dir)) {
        auto name = entry.path().filename().string();
        if (name.find("_lod0.bin") == std::string::npos) continue;
        // Check magic = 0x544C4554
        std::ifstream f(entry.path(), std::ios::binary);
        uint32_t magic = 0;
        if (f.read(reinterpret_cast<char*>(&magic), 4) && magic == 0x544C4554u)
            return true;
    }
    return false;
}

// ── Fast test: 0217_SL_TRI.dxf (~34 MB, ~47 k faces) ────────────────────────

TEST_CASE("Parse 0217_SL_TRI.dxf into tile cache", "[dxf_parser]") {
    const fs::path dxfPath = kDataDir / "0217_SL_TRI.dxf";
    if (!fs::exists(dxfPath))
        SKIP("0217_SL_TRI.dxf not found in TEST_DATA_DIR");

    fs::path cacheDir = fs::temp_directory_path() / "tv_test_tri";
    fs::remove_all(cacheDir);

    std::atomic<float> progress{0.0f};
    auto result = dxf::parseToCache(dxfPath, cacheDir, progress);

    // Origin must have been found (file has a HEADER section).
    REQUIRE(result.origin[0] != 0.0f);

    // Extract degenerate face count from warnings (if any).
    size_t malformed = 0;
    for (auto& w : result.warnings) {
        auto pos = w.find("Skipped ");
        if (pos != std::string::npos) {
            try { malformed = std::stoull(w.substr(pos + 8)); } catch (...) {}
        }
    }

    // Every 3DFACE entity must be accounted for: either parsed or flagged degenerate.
    // This verifies no entities were silently dropped by the state machine.
    // Note: this file uses the alternate triangle convention (v3=v0, not v3=v2).
    REQUIRE(result.faceCount + malformed == 47287);

    // 1374 entities in this file have genuinely degenerate (zero-area) first triangles.
    REQUIRE(result.faceCount == 45913);

    // Progress reached 1.0.
    REQUIRE(progress.load() == 1.0f);

    // At least one well-formed .bin tile file must exist.
    REQUIRE(hasBinFiles(cacheDir));

    // Verify .bin header fields for one tile file.
    for (auto& entry : fs::directory_iterator(cacheDir)) {
        auto name = entry.path().filename().string();
        if (name.find("_lod0.bin") == std::string::npos) continue;
        std::ifstream f(entry.path(), std::ios::binary);
        uint32_t magic, version, lod, vertCount, indexCount;
        REQUIRE(f.read(reinterpret_cast<char*>(&magic),      4));
        REQUIRE(f.read(reinterpret_cast<char*>(&version),    4));
        REQUIRE(f.read(reinterpret_cast<char*>(&lod),        4));
        REQUIRE(f.read(reinterpret_cast<char*>(&vertCount),  4));
        REQUIRE(f.read(reinterpret_cast<char*>(&indexCount), 4));
        REQUIRE(magic     == 0x544C4554u);
        REQUIRE(version   == 1u);
        REQUIRE(lod       == 0u);
        REQUIRE(vertCount == indexCount);        // vertCount = 3 * faceCount
        REQUIRE(vertCount % 3 == 0);
        break;  // one tile is enough
    }

    // No leftover .tmp files.
    for (auto& entry : fs::directory_iterator(cacheDir))
        REQUIRE(entry.path().extension() != ".tmp");

    fs::remove_all(cacheDir);
}

// ── Slow test: terrain.dxf (~2.4 GB, ~6.3 M faces) ──────────────────────────
// Tagged [slow] — excluded from regular runs; run explicitly with:
//   parser_tests.exe [slow]

TEST_CASE("Parse terrain.dxf into tile cache (slow)", "[slow][dxf_parser]") {
    const fs::path dxfPath = kDataDir / "terrain.dxf";
    if (!fs::exists(dxfPath))
        SKIP("terrain.dxf not found in TEST_DATA_DIR");

    fs::path cacheDir = fs::temp_directory_path() / "tv_test_terrain";
    fs::remove_all(cacheDir);

    std::atomic<float> progress{0.0f};
    auto result = dxf::parseToCache(dxfPath, cacheDir, progress);

    // The production file has ~6.3 M 3DFACE entities.
    // (CLAUDE.md spec shows "10 900" which is incorrect for this file.)
    REQUIRE(result.faceCount > 6000000);
    REQUIRE(result.origin[0] != 0.0f);
    REQUIRE(hasBinFiles(cacheDir));

    fs::remove_all(cacheDir);
}
