// dxf_parser/DxfParser.cpp
// Streams a DXF file into a per-tile binary cache on disk, then generates
// LOD1/LOD2 tiles via meshoptimizer and writes a cache.meta sidecar.
//
// Parse flow:
//   1. Check cache.meta for a cache hit (source_path + mtime match).
//   2. On miss: delete stale bins, then single-pass stream-parse the DXF.
//      - Extract $EXTMIN as scene origin.
//      - Stream 3DFACE entities in 50k-face chunks → .tmp tile files.
//      - Parse POLYLINE / LWPOLYLINE + XDATA into ParseResult.polylines.
//      - Finalize each .tmp → tile_X_Y_lod0.bin with TLET header.
//   3. generateLODs: deduplicate vertices, meshopt_simplify to LOD1/LOD2.
//   4. Write cache.meta JSON sidecar.
//
// Architecture rules enforced:
//   - uint32 indices only.
//   - Origin-offset applied before any tile/GPU data is written.
//   - NO DX11 headers included.

#include "DxfParser.h"
#include "DxfTypes.h"
#include "terrain/Config.h"  // TILE_SIZE_M, PARSE_CHUNK_FACES, LOD_RATIOS

#include <meshoptimizer.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace dxf {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ── Text I/O helpers ─────────────────────────────────────────────────────────

static void trimRight(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
}

static bool readPair(std::istream& in, int& code, std::string& value) {
    std::string codeLine;
    if (!std::getline(in, codeLine)) return false;
    trimRight(codeLine);
    try { code = std::stoi(codeLine); }
    catch (...) { code = -1; }
    if (!std::getline(in, value)) return false;
    trimRight(value);
    return true;
}

// ── Geometry helpers ─────────────────────────────────────────────────────────

static bool computeNormal(const float sv[3][3], float n[3]) {
    float e1[3] = { sv[1][0]-sv[0][0], sv[1][1]-sv[0][1], sv[1][2]-sv[0][2] };
    float e2[3] = { sv[2][0]-sv[0][0], sv[2][1]-sv[0][1], sv[2][2]-sv[0][2] };
    n[0] = e1[1]*e2[2] - e1[2]*e2[1];
    n[1] = e1[2]*e2[0] - e1[0]*e2[2];
    n[2] = e1[0]*e2[1] - e1[1]*e2[0];
    float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    if (len < 1e-12f) return false;
    n[0] /= len; n[1] /= len; n[2] /= len;
    return true;
}

static std::pair<int,int> tileKey(float sx, float sy) {
    return { (int)std::floor(sx / terrain::TILE_SIZE_M),
             (int)std::floor(sy / terrain::TILE_SIZE_M) };
}

// ── Intermediate face format (streaming .tmp) ─────────────────────────────────
#pragma pack(push, 1)
struct RawFace {
    float v[3][3];
    float n[3];
};
#pragma pack(pop)
static_assert(sizeof(RawFace) == 48, "RawFace must be 48 bytes");

// ── Tile buffer management ────────────────────────────────────────────────────

using TileKey = std::pair<int,int>;

static void flushTileBuffers(
    const fs::path&                          cacheDir,
    std::map<TileKey, std::vector<RawFace>>& tileBufs,
    std::map<TileKey, uint32_t>&             tileCounts)
{
    for (auto& [key, faces] : tileBufs) {
        if (faces.empty()) continue;
        auto tmpPath = cacheDir / ("tile_" + std::to_string(key.first) +
                                   "_" + std::to_string(key.second) + ".tmp");
        std::ofstream fout(tmpPath, std::ios::binary | std::ios::app);
        fout.write(reinterpret_cast<const char*>(faces.data()),
                   static_cast<std::streamsize>(faces.size() * sizeof(RawFace)));
        tileCounts[key] += static_cast<uint32_t>(faces.size());
        faces.clear();
    }
    tileBufs.clear();
}

static void finalizeTile(const fs::path& cacheDir, TileKey key, uint32_t faceCount) {
    auto tmpPath = cacheDir / ("tile_" + std::to_string(key.first) +
                               "_" + std::to_string(key.second) + ".tmp");
    auto binPath = cacheDir / ("tile_" + std::to_string(key.first) +
                               "_" + std::to_string(key.second) + "_lod0.bin");

    std::ifstream fin(tmpPath, std::ios::binary);
    if (!fin) return;

    uint32_t vertCount  = faceCount * 3;
    uint32_t indexCount = faceCount * 3;

    std::ofstream fout(binPath, std::ios::binary);
    constexpr uint32_t MAGIC   = 0x544C4554u;
    constexpr uint32_t VERSION = 1u;
    constexpr uint32_t LOD     = 0u;

    fout.write(reinterpret_cast<const char*>(&MAGIC),      4);
    fout.write(reinterpret_cast<const char*>(&VERSION),    4);
    fout.write(reinterpret_cast<const char*>(&LOD),        4);
    fout.write(reinterpret_cast<const char*>(&vertCount),  4);
    fout.write(reinterpret_cast<const char*>(&indexCount), 4);

    RawFace rf;
    while (fin.read(reinterpret_cast<char*>(&rf), sizeof(rf))) {
        for (int vi = 0; vi < 3; ++vi) {
            TerrainVertex tv{};
            tv.px = rf.v[vi][0]; tv.py = rf.v[vi][1]; tv.pz = rf.v[vi][2];
            tv.nx = rf.n[0];     tv.ny = rf.n[1];     tv.nz = rf.n[2];
            tv.color = 0.0f;
            fout.write(reinterpret_cast<const char*>(&tv), sizeof(tv));
        }
    }
    for (uint32_t i = 0; i < indexCount; ++i)
        fout.write(reinterpret_cast<const char*>(&i), sizeof(uint32_t));

    fout.close(); fin.close();
    fs::remove(tmpPath);
}

// ── Emit one triangle ─────────────────────────────────────────────────────────

static void emitTriangle(
    const float                              wv[3][3],
    const std::array<float,3>&               origin,
    std::map<TileKey, std::vector<RawFace>>& tileBufs,
    std::map<TileKey, uint32_t>&             tileCounts,
    size_t&                                  chunkCount,
    size_t&                                  totalFaces,
    size_t&                                  malformed,
    const fs::path&                          cacheDir)
{
    float sv[3][3];
    for (int vi = 0; vi < 3; ++vi) {
        sv[vi][0] = wv[vi][0] - origin[0];
        sv[vi][1] = wv[vi][1] - origin[1];
        sv[vi][2] = wv[vi][2] - origin[2];
    }

    float n[3];
    if (!computeNormal(sv, n)) { ++malformed; return; }

    if (n[2] < 0.0f) {
        std::swap(sv[1][0], sv[2][0]);
        std::swap(sv[1][1], sv[2][1]);
        std::swap(sv[1][2], sv[2][2]);
        n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
    }

    float cx = (sv[0][0] + sv[1][0] + sv[2][0]) / 3.0f;
    float cy = (sv[0][1] + sv[1][1] + sv[2][1]) / 3.0f;
    auto  key = tileKey(cx, cy);

    RawFace rf;
    for (int vi = 0; vi < 3; ++vi) {
        rf.v[vi][0] = sv[vi][0]; rf.v[vi][1] = sv[vi][1]; rf.v[vi][2] = sv[vi][2];
    }
    rf.n[0] = n[0]; rf.n[1] = n[1]; rf.n[2] = n[2];

    tileBufs[key].push_back(rf);
    ++chunkCount;
    ++totalFaces;

    if (chunkCount >= static_cast<size_t>(terrain::PARSE_CHUNK_FACES)) {
        flushTileBuffers(cacheDir, tileBufs, tileCounts);
        chunkCount = 0;
    }
}

// ── LOD bin write helper ──────────────────────────────────────────────────────

static void writeLodBin(const fs::path&                  binPath,
                        uint32_t                          lod,
                        const std::vector<TerrainVertex>& verts,
                        const std::vector<uint32_t>&      indices)
{
    std::ofstream fout(binPath, std::ios::binary);
    constexpr uint32_t MAGIC   = 0x544C4554u;
    constexpr uint32_t VERSION = 1u;
    auto vertCount  = static_cast<uint32_t>(verts.size());
    auto indexCount = static_cast<uint32_t>(indices.size());
    fout.write(reinterpret_cast<const char*>(&MAGIC),      4);
    fout.write(reinterpret_cast<const char*>(&VERSION),    4);
    fout.write(reinterpret_cast<const char*>(&lod),        4);
    fout.write(reinterpret_cast<const char*>(&vertCount),  4);
    fout.write(reinterpret_cast<const char*>(&indexCount), 4);
    fout.write(reinterpret_cast<const char*>(verts.data()),
               static_cast<std::streamsize>(vertCount * sizeof(TerrainVertex)));
    fout.write(reinterpret_cast<const char*>(indices.data()),
               static_cast<std::streamsize>(indexCount * sizeof(uint32_t)));
}

// ── LOD generation ────────────────────────────────────────────────────────────

void generateLODs(const fs::path& cacheDir) {
    if (!fs::is_directory(cacheDir)) return;

    for (auto& entry : fs::directory_iterator(cacheDir)) {
        const auto name = entry.path().filename().string();
        if (name.find("_lod0.bin") == std::string::npos) continue;

        const auto lod0Path = entry.path();
        // Derive lod1 / lod2 paths by replacing the "_lod0.bin" suffix.
        const std::string base = name.substr(0, name.size() - 9); // strip "_lod0.bin"
        const auto lod1Path = cacheDir / (base + "_lod1.bin");
        const auto lod2Path = cacheDir / (base + "_lod2.bin");

        // Skip tile if both LOD bins already exist and are newer than LOD0.
        std::error_code ec;
        const auto lod0Mtime = fs::last_write_time(lod0Path, ec);
        if (!ec &&
            fs::exists(lod1Path, ec) && fs::last_write_time(lod1Path, ec) >= lod0Mtime &&
            fs::exists(lod2Path, ec) && fs::last_write_time(lod2Path, ec) >= lod0Mtime) {
            continue;
        }

        // ── Read LOD0 vertex buffer ──────────────────────────────────────
        std::ifstream fin(lod0Path, std::ios::binary);
        if (!fin) continue;

        uint32_t magic, version, lod0, vertCount, indexCount;
        if (!fin.read(reinterpret_cast<char*>(&magic),      4) || magic != 0x544C4554u) continue;
        if (!fin.read(reinterpret_cast<char*>(&version),    4)) continue;
        if (!fin.read(reinterpret_cast<char*>(&lod0),       4)) continue;
        if (!fin.read(reinterpret_cast<char*>(&vertCount),  4)) continue;
        if (!fin.read(reinterpret_cast<char*>(&indexCount), 4)) continue;

        if (vertCount == 0 || vertCount % 3 != 0) continue;

        std::vector<TerrainVertex> rawVerts(vertCount);
        if (!fin.read(reinterpret_cast<char*>(rawVerts.data()),
                      static_cast<std::streamsize>(vertCount * sizeof(TerrainVertex))))
            continue;
        fin.close();

        // LOD0 uses sequential indices (0,1,2,...); no need to read the index buffer.
        // indexCount == vertCount for LOD0.

        // ── Deduplicate vertices for proper edge connectivity ─────────────
        // meshopt_generateVertexRemap with nullptr indices treats the mesh as
        // unindexed (implicitly: index[i] = i for i in [0, vertCount)).
        std::vector<uint32_t> remap(vertCount);
        const size_t uniqueCount = meshopt_generateVertexRemap(
            remap.data(),
            nullptr,           // unindexed input
            vertCount,
            rawVerts.data(), vertCount, sizeof(TerrainVertex));

        std::vector<TerrainVertex> uVerts(uniqueCount);
        meshopt_remapVertexBuffer(
            uVerts.data(), rawVerts.data(), vertCount, sizeof(TerrainVertex), remap.data());

        std::vector<uint32_t> baseIdx(vertCount);
        meshopt_remapIndexBuffer(baseIdx.data(), nullptr, vertCount, remap.data());

        rawVerts.clear();
        rawVerts.shrink_to_fit();  // free unindexed buffer before allocating LOD buffers

        // ── Simplify to LOD1 and LOD2 ────────────────────────────────────
        // target_error = 1.0 lets the simplifier reach the target count without an
        // error budget constraint.  Both LODs are derived from the same base mesh.
        auto simplify = [&](uint32_t lodN, float ratio, const fs::path& lodPath) {
            const size_t target = std::max(static_cast<size_t>(3),
                                           static_cast<size_t>(vertCount * static_cast<double>(ratio)));
            std::vector<uint32_t> lodIdx(vertCount); // capacity = index_count, per API contract
            const size_t lodCount = meshopt_simplify(
                lodIdx.data(),
                baseIdx.data(), vertCount,
                &uVerts[0].px, uniqueCount, sizeof(TerrainVertex),
                target, 1.0f, 0, nullptr);
            lodIdx.resize(lodCount);
            writeLodBin(lodPath, lodN, uVerts, lodIdx);
        };

        simplify(1, terrain::LOD_RATIOS[1], lod1Path);
        simplify(2, terrain::LOD_RATIOS[2], lod2Path);
    }
}

// ── Cache cleanup ─────────────────────────────────────────────────────────────

void clearTileCache(const fs::path& cacheDir) {
    if (!fs::is_directory(cacheDir)) return;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(cacheDir, ec)) {
        const auto ext  = entry.path().extension().string();
        const auto name = entry.path().filename().string();
        if (ext == ".bin" || name == "cache.meta")
            fs::remove(entry.path(), ec);
    }
}

// ── cache.meta helpers ────────────────────────────────────────────────────────

static std::string mtimeString(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return "0";
    // Store raw tick count as a decimal string to avoid JSON integer precision issues.
    return std::to_string(t.time_since_epoch().count());
}

static void writeCacheMeta(const fs::path&          cacheDir,
                           const fs::path&          sourcePath,
                           const std::array<float,3>& origin,
                           size_t                   tileCount,
                           size_t                   faceCount)
{
    json meta;
    meta["source_path"]          = sourcePath.string();
    meta["file_mtime"]           = mtimeString(sourcePath);
    meta["server_last_modified"] = nullptr;
    meta["downloaded_at"]        = nullptr;
    meta["tile_count"]           = tileCount;
    meta["face_count"]           = faceCount;
    meta["origin"]               = { origin[0], origin[1], origin[2] };

    std::ofstream fout(cacheDir / "cache.meta");
    fout << meta.dump(2);
}

// Returns true if cache.meta exists and matches sourcePath + current mtime.
// On hit, populates result with cached values.
static bool checkCacheValid(const fs::path& cacheDir,
                            const fs::path& sourcePath,
                            ParseResult&    result)
{
    const auto metaPath = cacheDir / "cache.meta";
    std::ifstream fin(metaPath);
    if (!fin) return false;

    json meta;
    try { fin >> meta; }
    catch (...) { return false; }

    if (!meta.contains("source_path") ||
        !meta.contains("file_mtime")  ||
        !meta.contains("origin")      ||
        !meta.contains("tile_count"))
        return false;

    if (meta["source_path"].get<std::string>() != sourcePath.string())
        return false;

    const std::string currentMtime = mtimeString(sourcePath);
    if (meta["file_mtime"].get<std::string>() != currentMtime)
        return false;

    // Cache hit — populate result from meta.
    const auto& orig = meta["origin"];
    result.origin      = { orig[0].get<float>(), orig[1].get<float>(), orig[2].get<float>() };
    result.tileCount   = meta["tile_count"].get<size_t>();
    result.faceCount   = meta.value("face_count", size_t{0});
    result.polylineCount = 0;  // not stored in Phase 1 cache
    return true;
}

// ── Main entry point ─────────────────────────────────────────────────────────

ParseResult parseToCache(const fs::path& dxfPath,
                         const fs::path& cacheDir,
                         std::atomic<float>& progress)
{
    ParseResult result{};

    fs::create_directories(cacheDir);

    // ── Cache hit check ───────────────────────────────────────────────────
    if (checkCacheValid(cacheDir, dxfPath, result)) {
        // Even on a hit, run generateLODs to fill any LOD bins that are missing
        // (e.g., first-run LOD generation was interrupted).
        generateLODs(cacheDir);
        progress.store(1.0f);
        return result;
    }

    // Cache miss: purge stale tile bins and cache.meta before re-parsing.
    clearTileCache(cacheDir);

    // ── File size for progress ────────────────────────────────────────────
    std::error_code ec;
    double fileSize = static_cast<double>(fs::file_size(dxfPath, ec));
    if (ec || fileSize == 0.0) {
        result.warnings.push_back("Cannot stat DXF file: " + dxfPath.string());
        return result;
    }

    std::ifstream in(dxfPath, std::ios::binary);
    if (!in) {
        result.warnings.push_back("Cannot open: " + dxfPath.string());
        return result;
    }

    // ── State machine ─────────────────────────────────────────────────────
    enum class Sec { None, Header, Entities, Other };
    Sec  sec          = Sec::None;
    bool awaitSecName = false;

    std::array<float,3> origin{0.0f, 0.0f, 0.0f};
    bool originFound = false;
    bool awaitExtmin = false;
    int  extminBits  = 0;

    float fv[4][3] = {};
    bool  in3DFace = false;

    std::map<TileKey, std::vector<RawFace>> tileBufs;
    std::map<TileKey, uint32_t>             tileCounts;
    size_t chunkCount = 0;
    size_t totalFaces = 0;
    size_t malformed  = 0;
    size_t pairCount  = 0;

    // ── Polyline accumulator ──────────────────────────────────────────────
    enum class PolyState { None, Poly3DHead, Poly3DVert, LW };
    PolyState polyState = PolyState::None;

    struct PolyAccum {
        bool                              is3D     = false;
        std::vector<std::array<float,3>> verts;
        std::string                       layer;
        int                               colorAci = 256; // 256 = BYLAYER
        std::vector<XDataEntry>           xdata;
        float                             lwElev = 0.0f;
        float vx = 0.0f, vy = 0.0f, vz = 0.0f;
        bool  vHasX = false;
        float lwX   = 0.0f;
        bool  lwHasX = false;
        std::string              xApp;
        std::vector<std::string> xVals;
        bool                     inXdata = false;
    } pa;

    auto flushXdata = [&]() {
        if (pa.inXdata) {
            pa.xdata.push_back({ pa.xApp, std::move(pa.xVals) });
            pa.xApp.clear(); pa.xVals.clear(); pa.inXdata = false;
        }
    };

    auto emitPolyline = [&]() {
        flushXdata();
        if (!pa.verts.empty()) {
            ParsedPolyline pl;
            pl.is3D     = pa.is3D;
            pl.layer    = pa.layer;
            pl.colorAci = pa.colorAci;
            pl.xdata    = std::move(pa.xdata);
            for (auto& v : pa.verts) {
                v[0] -= origin[0]; v[1] -= origin[1]; v[2] -= origin[2];
            }
            pl.verts = std::move(pa.verts);
            result.polylines.push_back(std::move(pl));
            ++result.polylineCount;
        }
        pa = PolyAccum{}; polyState = PolyState::None;
    };

    // ── Main parse loop ───────────────────────────────────────────────────
    int code; std::string val;

    while (readPair(in, code, val)) {
        if ((++pairCount & 0x1FFF) == 0) {
            auto pos = static_cast<double>(static_cast<std::streamoff>(in.tellg()));
            if (pos >= 0.0 && fileSize > 0.0)
                progress.store(static_cast<float>(pos / fileSize));
        }

        if (code == 0) {
            // Complete any in-progress 3DFACE.
            if (in3DFace && sec == Sec::Entities) {
                emitTriangle(fv, origin, tileBufs, tileCounts,
                             chunkCount, totalFaces, malformed, cacheDir);
                bool v3EqV2 = (fv[3][0]==fv[2][0] && fv[3][1]==fv[2][1] && fv[3][2]==fv[2][2]);
                bool v3EqV0 = (fv[3][0]==fv[0][0] && fv[3][1]==fv[0][1] && fv[3][2]==fv[0][2]);
                if (!v3EqV2 && !v3EqV0) {
                    float qv[3][3] = {
                        {fv[0][0],fv[0][1],fv[0][2]}, {fv[2][0],fv[2][1],fv[2][2]},
                        {fv[3][0],fv[3][1],fv[3][2]}
                    };
                    emitTriangle(qv, origin, tileBufs, tileCounts,
                                 chunkCount, totalFaces, malformed, cacheDir);
                }
            }
            in3DFace = false;

            // End / transition any in-progress polyline.
            if (sec == Sec::Entities && polyState != PolyState::None) {
                if (polyState == PolyState::Poly3DVert) {
                    if (pa.vHasX) pa.verts.push_back({ pa.vx, pa.vy, pa.vz });
                    if (val == "VERTEX") {
                        pa.vx = pa.vy = pa.vz = 0.0f; pa.vHasX = false;
                    } else {
                        emitPolyline();
                    }
                } else if (polyState == PolyState::Poly3DHead) {
                    if (val == "VERTEX") {
                        flushXdata();
                        polyState = PolyState::Poly3DVert;
                        pa.vx = pa.vy = pa.vz = 0.0f; pa.vHasX = false;
                    } else {
                        emitPolyline();
                    }
                } else { // LW
                    emitPolyline();
                }
            }

            if (val == "SECTION") { awaitSecName = true; continue; }
            if (val == "ENDSEC")  { sec = Sec::None; awaitSecName = false; continue; }
            if (val == "EOF")     { break; }

            if (sec == Sec::Entities) {
                if (val == "3DFACE") {
                    in3DFace = true;
                    for (auto& v : fv) v[0] = v[1] = v[2] = 0.0f;
                } else if (val == "POLYLINE") {
                    polyState = PolyState::Poly3DHead; pa = PolyAccum{}; pa.is3D = true;
                } else if (val == "LWPOLYLINE") {
                    polyState = PolyState::LW; pa = PolyAccum{}; pa.is3D = false;
                }
            }
            continue;
        }

        if (awaitSecName && code == 2) {
            awaitSecName = false;
            if      (val == "HEADER")   sec = Sec::Header;
            else if (val == "ENTITIES") sec = Sec::Entities;
            else                         sec = Sec::Other;
            continue;
        }

        if (sec == Sec::Header) {
            if (awaitExtmin) {
                if      (code == 10) { origin[0] = std::stof(val); extminBits |= 1; }
                else if (code == 20) { origin[1] = std::stof(val); extminBits |= 2; }
                else if (code == 30) { origin[2] = std::stof(val); extminBits |= 4; }
                if (extminBits == 7) { originFound = true; awaitExtmin = false; }
                continue;
            }
            if (code == 9 && val == "$EXTMIN") { awaitExtmin = true; extminBits = 0; }
            continue;
        }

        if (sec == Sec::Entities) {
            if (in3DFace) {
                if      (code >= 10 && code <= 13) { try { fv[code-10][0] = std::stof(val); } catch (...) {} }
                else if (code >= 20 && code <= 23) { try { fv[code-20][1] = std::stof(val); } catch (...) {} }
                else if (code >= 30 && code <= 33) { try { fv[code-30][2] = std::stof(val); } catch (...) {} }
            } else if (polyState == PolyState::Poly3DHead) {
                if      (code == 8)    { pa.layer = val; }
                else if (code == 62)   { try { pa.colorAci = std::stoi(val); } catch (...) {} }
                else if (code == 1001) { flushXdata(); pa.xApp = val; pa.inXdata = true; }
                else if (code >= 1000 && pa.inXdata) { pa.xVals.push_back(val); }
            } else if (polyState == PolyState::Poly3DVert) {
                if      (code == 10) { try { pa.vx = std::stof(val); pa.vHasX = true; } catch (...) {} }
                else if (code == 20) { try { pa.vy = std::stof(val); } catch (...) {} }
                else if (code == 30) { try { pa.vz = std::stof(val); } catch (...) {} }
            } else if (polyState == PolyState::LW) {
                if      (code == 8)  { pa.layer = val; }
                else if (code == 62) { try { pa.colorAci = std::stoi(val); } catch (...) {} }
                else if (code == 38) { try { pa.lwElev = std::stof(val); } catch (...) {} }
                else if (code == 10) { try { pa.lwX = std::stof(val); pa.lwHasX = true; } catch (...) {} }
                else if (code == 20) {
                    if (pa.lwHasX) {
                        try { pa.verts.push_back({ pa.lwX, std::stof(val), pa.lwElev }); } catch (...) {}
                        pa.lwHasX = false;
                    }
                } else if (code == 1001) { flushXdata(); pa.xApp = val; pa.inXdata = true; }
                else if (code >= 1000 && pa.inXdata) { pa.xVals.push_back(val); }
            }
        }
    } // while readPair

    // Post-loop: emit any in-progress polyline.
    if (polyState != PolyState::None) {
        if (polyState == PolyState::Poly3DVert && pa.vHasX)
            pa.verts.push_back({ pa.vx, pa.vy, pa.vz });
        emitPolyline();
    }

    // Flush remaining terrain faces.
    if (!tileBufs.empty()) flushTileBuffers(cacheDir, tileBufs, tileCounts);

    progress.store(1.0f);

    // Finalize .tmp → lod0.bin.
    for (auto& [key, count] : tileCounts) finalizeTile(cacheDir, key, count);

    result.origin    = origin;
    result.faceCount = totalFaces;
    result.tileCount = tileCounts.size();

    if (!originFound)
        result.warnings.push_back("$EXTMIN not found — origin set to (0, 0, 0)");
    if (malformed)
        result.warnings.push_back("Skipped " + std::to_string(malformed) +
                                   " degenerate face(s)");

    // ── LOD generation + cache.meta ───────────────────────────────────────
    generateLODs(cacheDir);
    writeCacheMeta(cacheDir, dxfPath, result.origin, result.tileCount, result.faceCount);

    return result;
}

} // namespace dxf
