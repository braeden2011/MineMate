#include <catch2/catch_test_macros.hpp>

#include "DxfParser.h"
#include "DxfTypes.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

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

    // This file is pure triangulation — no polylines expected.
    REQUIRE(result.polylineCount == 0);
    REQUIRE(result.polylines.empty());

    fs::remove_all(cacheDir);
}

// ── Fast test: 0217_SL_CAD.dxf (446 POLYLINE + 1550 LWPOLYLINE) ─────────────

TEST_CASE("Parse 0217_SL_CAD.dxf polylines and XDATA", "[dxf_parser]") {
    const fs::path dxfPath = kDataDir / "0217_SL_CAD.dxf";
    if (!fs::exists(dxfPath))
        SKIP("0217_SL_CAD.dxf not found in TEST_DATA_DIR");

    fs::path cacheDir = fs::temp_directory_path() / "tv_test_cad";
    fs::remove_all(cacheDir);

    std::atomic<float> progress{0.0f};
    auto result = dxf::parseToCache(dxfPath, cacheDir, progress);

    // Count by kind.
    size_t poly3dCount = 0, lwCount = 0;
    for (auto& pl : result.polylines) {
        if (pl.is3D) ++poly3dCount;
        else         ++lwCount;
    }
    REQUIRE(poly3dCount == 446);
    REQUIRE(lwCount     == 1550);
    REQUIRE(result.polylineCount == 1996);

    // Every parsed polyline must have at least one vertex.
    for (auto& pl : result.polylines)
        REQUIRE(!pl.verts.empty());

    // At least some polylines must carry XDATA (the CAD file has ~26k GC1001 entries).
    size_t withXdata = 0;
    for (auto& pl : result.polylines)
        if (!pl.xdata.empty()) ++withXdata;
    REQUIRE(withXdata > 0);

    // Vertices must be origin-offset: raw MGA easting ~434000–437000;
    // after subtraction they should be much smaller (within a few km of origin).
    for (auto& pl : result.polylines) {
        for (auto& v : pl.verts) {
            // Raw MGA X ~ 433000–438000; origin-offset X should be < 10000.
            REQUIRE(std::abs(v[0]) < 10000.0f);
        }
    }

    // This file has no 3DFACE entities — no tile .bin files and no faces.
    REQUIRE(result.faceCount == 0);

    // Progress reached 1.0.
    REQUIRE(progress.load() == 1.0f);

    // Origin found ($EXTMIN present in file).
    REQUIRE(result.origin[0] != 0.0f);

    fs::remove_all(cacheDir);
}

// Read the TLET binary header from a .bin file.
// Returns false if the file is missing or malformed.
struct BinHeader { uint32_t magic, version, lod, vertCount, indexCount; };
static bool readBinHeader(const fs::path& p, BinHeader& h) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    return f.read(reinterpret_cast<char*>(&h.magic),      4) &&
           f.read(reinterpret_cast<char*>(&h.version),    4) &&
           f.read(reinterpret_cast<char*>(&h.lod),        4) &&
           f.read(reinterpret_cast<char*>(&h.vertCount),  4) &&
           f.read(reinterpret_cast<char*>(&h.indexCount), 4) &&
           h.magic == 0x544C4554u;
}

// ── LOD generation + cache.meta test: 0217_SL_TRI.dxf ───────────────────────
// The dev guide brief calls this file "0210_SL_TRI.dxf"; the actual file in
// docs/sample_data/ is 0217_SL_TRI.dxf (same data, spec had a typo).

TEST_CASE("LOD generation and cache.meta for 0217_SL_TRI.dxf", "[dxf_parser]") {
    const fs::path dxfPath = kDataDir / "0217_SL_TRI.dxf";
    if (!fs::exists(dxfPath))
        SKIP("0217_SL_TRI.dxf not found in TEST_DATA_DIR");

    fs::path cacheDir = fs::temp_directory_path() / "tv_test_lod";
    fs::remove_all(cacheDir);

    // ── First parse: full pipeline (parse → LOD0 → LOD1/LOD2 → cache.meta) ──
    std::atomic<float> progress{0.0f};
    auto result = dxf::parseToCache(dxfPath, cacheDir, progress);

    REQUIRE(result.faceCount == 45913);
    REQUIRE(result.tileCount > 0);
    REQUIRE(progress.load() == 1.0f);

    // Find one tile that has a lod0.bin, lod1.bin, and lod2.bin.
    bool foundLod1 = false;
    bool foundLod2 = false;
    for (auto& entry : fs::directory_iterator(cacheDir)) {
        const auto name = entry.path().filename().string();
        if (name.find("_lod0.bin") == std::string::npos) continue;

        // Derive lod1 / lod2 paths.
        const std::string base = name.substr(0, name.size() - 9);
        const auto lod1Path = cacheDir / (base + "_lod1.bin");
        const auto lod2Path = cacheDir / (base + "_lod2.bin");

        BinHeader h0, h1, h2;
        if (!readBinHeader(entry.path(), h0)) continue;
        if (h0.indexCount < 9) continue;  // skip tiny tiles with too few triangles to simplify

        REQUIRE(h0.lod == 0u);
        REQUIRE(h0.indexCount % 3 == 0);

        // lod1.bin must exist and have fewer indices than lod0.
        REQUIRE(fs::exists(lod1Path));
        REQUIRE(readBinHeader(lod1Path, h1));
        REQUIRE(h1.magic   == 0x544C4554u);
        REQUIRE(h1.version == 1u);
        REQUIRE(h1.lod     == 1u);
        REQUIRE(h1.indexCount < h0.indexCount);
        REQUIRE(h1.indexCount % 3 == 0);
        foundLod1 = true;

        // lod2.bin must exist and have fewer indices than lod0.
        REQUIRE(fs::exists(lod2Path));
        REQUIRE(readBinHeader(lod2Path, h2));
        REQUIRE(h2.magic   == 0x544C4554u);
        REQUIRE(h2.version == 1u);
        REQUIRE(h2.lod     == 2u);
        REQUIRE(h2.indexCount < h0.indexCount);
        REQUIRE(h2.indexCount % 3 == 0);
        foundLod2 = true;

        break;  // one tile is sufficient
    }
    REQUIRE(foundLod1);
    REQUIRE(foundLod2);

    // ── cache.meta must exist with required fields ────────────────────────
    const auto metaPath = cacheDir / "cache.meta";
    REQUIRE(fs::exists(metaPath));
    {
        std::ifstream mf(metaPath);
        REQUIRE(mf.is_open());
        // Read raw JSON text and check for required keys.
        std::string metaText((std::istreambuf_iterator<char>(mf)),
                              std::istreambuf_iterator<char>());
        REQUIRE(metaText.find("\"source_path\"")          != std::string::npos);
        REQUIRE(metaText.find("\"file_mtime\"")           != std::string::npos);
        REQUIRE(metaText.find("\"tile_count\"")           != std::string::npos);
        REQUIRE(metaText.find("\"origin\"")               != std::string::npos);
        REQUIRE(metaText.find("\"server_last_modified\"") != std::string::npos);
        REQUIRE(metaText.find("\"downloaded_at\"")        != std::string::npos);
    }

    // ── No leftover .tmp files ────────────────────────────────────────────
    for (auto& entry : fs::directory_iterator(cacheDir))
        REQUIRE(entry.path().extension() != ".tmp");

    // ── Cache hit: second parseToCache must reuse cached data ────────────
    // Save lod1 mtime for one tile before the second call.
    fs::path anyLod1;
    for (auto& entry : fs::directory_iterator(cacheDir)) {
        if (entry.path().filename().string().find("_lod1.bin") != std::string::npos) {
            anyLod1 = entry.path(); break;
        }
    }
    REQUIRE(!anyLod1.empty());
    const auto lod1MtimeBefore = fs::last_write_time(anyLod1);

    std::atomic<float> progress2{0.0f};
    auto result2 = dxf::parseToCache(dxfPath, cacheDir, progress2);

    // Origin must match first parse.
    REQUIRE(result2.origin[0] == result.origin[0]);
    REQUIRE(result2.origin[1] == result.origin[1]);
    REQUIRE(result2.origin[2] == result.origin[2]);
    REQUIRE(result2.tileCount == result.tileCount);

    // LOD1 mtime must be unchanged — cache was hit, no tiles were regenerated.
    REQUIRE(fs::last_write_time(anyLod1) == lod1MtimeBefore);

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
    // (CLAUDE.md spec shows "10 900" which is incorrect for this file — see DEVLOG.)
    REQUIRE(result.faceCount > 6000000);
    REQUIRE(result.origin[0] != 0.0f);
    REQUIRE(hasBinFiles(cacheDir));

    // Regression: adding polyline parsing must not steal terrain faces.
    // terrain.dxf is a pure triangulation — polyline count should be 0.
    REQUIRE(result.polylineCount == 0);

    fs::remove_all(cacheDir);
}
