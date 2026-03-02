// dxf_parser/DxfParser.cpp
// Streams a DXF file into a per-tile binary cache on disk.
//
// Parse flow:
//   1. Single forward pass: extract $EXTMIN then process ENTITIES section.
//   2. For every 3DFACE: subtract origin, compute flat normal, bin to 50 m tile.
//   3. Every PARSE_CHUNK_FACES faces: flush tile buffers to .tmp files, clear RAM.
//   4. After all faces: finalize each .tmp → tile_X_Y_lod0.bin with proper header.
//   5. For every POLYLINE / LWPOLYLINE: collect vertices + XDATA, subtract origin,
//      store in ParseResult.polylines (disk cache for linework is a later phase).
//
// Architecture rules enforced here:
//   - uint32 indices only.
//   - Origin-offset applied before any tile/GPU data is written.
//   - NO DX11 headers included.

#include "DxfParser.h"
#include "DxfTypes.h"
#include "terrain/Config.h"  // TILE_SIZE_M, PARSE_CHUNK_FACES

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace dxf {

namespace fs = std::filesystem;

// ── Text I/O helpers ─────────────────────────────────────────────────────────

// Strip trailing CR, space, tab.
static void trimRight(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
}

// Read one group-code / value pair.  Returns false at EOF or error.
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

// Compute flat normal for triangle (sv[0], sv[1], sv[2]).
// Returns false if the face is degenerate (collinear vertices).
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

// Tile (x, y) index from a scene-space XY centroid.
static std::pair<int,int> tileKey(float sx, float sy) {
    return { (int)std::floor(sx / terrain::TILE_SIZE_M),
             (int)std::floor(sy / terrain::TILE_SIZE_M) };
}

// ── Intermediate (streaming) face format ─────────────────────────────────────
// Written to .tmp files during parsing; converted to .bin on finalisation.
// 12 floats × 4 bytes = 48 bytes per entry; trivially appendable.
#pragma pack(push, 1)
struct RawFace {
    float v[3][3];  // three origin-offset vertices
    float n[3];     // outward flat normal
};
#pragma pack(pop)

static_assert(sizeof(RawFace) == 48, "RawFace must be 48 bytes");

// ── Tile buffer management ────────────────────────────────────────────────────

using TileKey = std::pair<int,int>;

// Append all buffered tile faces to their .tmp files; clear the in-RAM buffers.
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

// Convert a tile's .tmp file to the final .bin format, then delete .tmp.
//
// Binary format (CLAUDE.md spec):
//   uint32  magic     = 0x544C4554  ('TLET')
//   uint32  version   = 1
//   uint32  lod       = 0
//   uint32  vertCount = faceCount * 3
//   uint32  indexCount = faceCount * 3
//   TerrainVertex[vertCount]   (28 bytes each)
//   uint32[indexCount]         (sequential: 0, 1, 2, 3, 4, 5, …)
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

    // Vertex data: 3 TerrainVertex per raw face.
    RawFace rf;
    while (fin.read(reinterpret_cast<char*>(&rf), sizeof(rf))) {
        for (int vi = 0; vi < 3; ++vi) {
            TerrainVertex tv{};
            tv.px    = rf.v[vi][0];
            tv.py    = rf.v[vi][1];
            tv.pz    = rf.v[vi][2];
            tv.nx    = rf.n[0];
            tv.ny    = rf.n[1];
            tv.nz    = rf.n[2];
            tv.color = 0.0f;  // elevation tint assigned in Phase 2
            fout.write(reinterpret_cast<const char*>(&tv), sizeof(tv));
        }
    }

    // Sequential index buffer (uint32, per architecture rules).
    for (uint32_t i = 0; i < indexCount; ++i)
        fout.write(reinterpret_cast<const char*>(&i), sizeof(uint32_t));

    fout.close();
    fin.close();
    fs::remove(tmpPath);
}

// ── Emit one triangle ─────────────────────────────────────────────────────────
// Takes raw world-space vertices, origin-offsets them, computes & corrects
// normal, then bins into the appropriate tile buffer.  Flushes chunk if full.
static void emitTriangle(
    const float                              wv[3][3],  // world-space vertices
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

    // Ensure normal points outward (positive Z hemisphere).
    if (n[2] < 0.0f) {
        std::swap(sv[1][0], sv[2][0]);
        std::swap(sv[1][1], sv[2][1]);
        std::swap(sv[1][2], sv[2][2]);
        n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
    }

    // Bin by centroid XY.
    float cx = (sv[0][0] + sv[1][0] + sv[2][0]) / 3.0f;
    float cy = (sv[0][1] + sv[1][1] + sv[2][1]) / 3.0f;
    auto  key = tileKey(cx, cy);

    RawFace rf;
    for (int vi = 0; vi < 3; ++vi) {
        rf.v[vi][0] = sv[vi][0];
        rf.v[vi][1] = sv[vi][1];
        rf.v[vi][2] = sv[vi][2];
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

// ── Main entry point ─────────────────────────────────────────────────────────

ParseResult parseToCache(const fs::path& dxfPath,
                         const fs::path& cacheDir,
                         std::atomic<float>& progress)
{
    ParseResult result{};
    result.origin        = {0.0f, 0.0f, 0.0f};
    result.faceCount     = 0;
    result.polylineCount = 0;

    // File size for progress denominator.
    std::error_code ec;
    double fileSize = static_cast<double>(fs::file_size(dxfPath, ec));
    if (ec || fileSize == 0.0) {
        result.warnings.push_back("Cannot stat DXF file: " + dxfPath.string());
        return result;
    }

    fs::create_directories(cacheDir);

    // Open in binary mode so tellg() returns byte offsets on Windows.
    std::ifstream in(dxfPath, std::ios::binary);
    if (!in) {
        result.warnings.push_back("Cannot open: " + dxfPath.string());
        return result;
    }

    // ── State machine ─────────────────────────────────────────────────────
    enum class Sec { None, Header, Entities, Other };
    Sec  sec          = Sec::None;
    bool awaitSecName = false;  // saw "0 SECTION", waiting for "2 <name>"

    // $EXTMIN accumulation.
    std::array<float,3> origin{0.0f, 0.0f, 0.0f};
    bool originFound = false;
    bool awaitExtmin = false;   // saw "9 $EXTMIN", collecting 10/20/30
    int  extminBits  = 0;       // bitmask: bit0=X, bit1=Y, bit2=Z

    // 3DFACE accumulation.
    float fv[4][3] = {};
    bool  in3DFace = false;

    // Tile buffers and counters.
    std::map<TileKey, std::vector<RawFace>> tileBufs;
    std::map<TileKey, uint32_t>             tileCounts;
    size_t chunkCount = 0;
    size_t totalFaces = 0;
    size_t malformed  = 0;
    size_t pairCount  = 0;  // for progress updates

    // ── Polyline accumulator ──────────────────────────────────────────────
    // Holds all state for the polyline entity currently being assembled.
    // Reset to default on every new entity start.
    enum class PolyState { None, Poly3DHead, Poly3DVert, LW };
    PolyState polyState = PolyState::None;

    struct PolyAccum {
        bool                              is3D   = false;
        std::vector<std::array<float,3>> verts;
        std::string                       layer;
        std::vector<XDataEntry>           xdata;
        float                             lwElev = 0.0f;  // gc38: constant Z for LWPOLYLINE
        // 3D POLYLINE: current VERTEX being assembled
        float vx = 0.0f, vy = 0.0f, vz = 0.0f;
        bool  vHasX = false;
        // LWPOLYLINE: pending X waiting for its gc20 pair
        float lwX    = 0.0f;
        bool  lwHasX = false;
        // XDATA: accumulator for one XDataEntry
        std::string              xApp;
        std::vector<std::string> xVals;
        bool                     inXdata = false;
    } pa;

    // Finalise the pending XDataEntry (if any) into pa.xdata.
    auto flushXdata = [&]() {
        if (pa.inXdata) {
            pa.xdata.push_back({ pa.xApp, std::move(pa.xVals) });
            pa.xApp.clear();
            pa.xVals.clear();
            pa.inXdata = false;
        }
    };

    // Origin-offset pa.verts, push completed ParsedPolyline into result,
    // increment polylineCount, then reset the accumulator.
    auto emitPolyline = [&]() {
        flushXdata();
        if (!pa.verts.empty()) {
            ParsedPolyline pl;
            pl.is3D  = pa.is3D;
            pl.layer = pa.layer;
            pl.xdata = std::move(pa.xdata);
            for (auto& v : pa.verts) {
                v[0] -= origin[0];
                v[1] -= origin[1];
                v[2] -= origin[2];
            }
            pl.verts = std::move(pa.verts);
            result.polylines.push_back(std::move(pl));
            ++result.polylineCount;
        }
        pa        = PolyAccum{};
        polyState = PolyState::None;
    };

    // ── Main parse loop ───────────────────────────────────────────────────
    int code; std::string val;

    while (readPair(in, code, val)) {
        // Progress: update every 8192 pairs.
        if ((++pairCount & 0x1FFF) == 0) {
            auto pos = static_cast<double>(static_cast<std::streamoff>(in.tellg()));
            if (pos >= 0.0 && fileSize > 0.0)
                progress.store(static_cast<float>(pos / fileSize));
        }

        // ── Entity boundary (code 0) ──────────────────────────────────────
        if (code == 0) {
            // Complete any in-progress 3DFACE.
            if (in3DFace && sec == Sec::Entities) {
                // Triangle: use v0, v1, v2.
                emitTriangle(fv, origin,
                             tileBufs, tileCounts,
                             chunkCount, totalFaces, malformed,
                             cacheDir);

                // Quad: v3 is a genuine fourth vertex only if it equals neither v2
                // (standard DXF triangle) nor v0 (alternate DXF triangle convention).
                bool v3EqV2 = (fv[3][0] == fv[2][0] && fv[3][1] == fv[2][1] && fv[3][2] == fv[2][2]);
                bool v3EqV0 = (fv[3][0] == fv[0][0] && fv[3][1] == fv[0][1] && fv[3][2] == fv[0][2]);
                if (!v3EqV2 && !v3EqV0) {
                    float qv[3][3] = {
                        { fv[0][0], fv[0][1], fv[0][2] },
                        { fv[2][0], fv[2][1], fv[2][2] },
                        { fv[3][0], fv[3][1], fv[3][2] }
                    };
                    emitTriangle(qv, origin,
                                 tileBufs, tileCounts,
                                 chunkCount, totalFaces, malformed,
                                 cacheDir);
                }
            }
            in3DFace = false;

            // Handle end / transition of any in-progress polyline.
            if (sec == Sec::Entities && polyState != PolyState::None) {
                if (polyState == PolyState::Poly3DVert) {
                    // Commit the vertex we were accumulating (if any).
                    if (pa.vHasX)
                        pa.verts.push_back({ pa.vx, pa.vy, pa.vz });
                    if (val == "VERTEX") {
                        // Next sub-entity: reset vertex buffer, stay in Poly3DVert.
                        pa.vx = pa.vy = pa.vz = 0.0f;
                        pa.vHasX = false;
                    } else {
                        // SEQEND or any unexpected entity: emit the polyline.
                        emitPolyline();
                    }
                } else if (polyState == PolyState::Poly3DHead) {
                    if (val == "VERTEX") {
                        // Normal transition: close header XDATA, start collecting vertices.
                        flushXdata();
                        polyState    = PolyState::Poly3DVert;
                        pa.vx = pa.vy = pa.vz = 0.0f;
                        pa.vHasX = false;
                    } else {
                        // Malformed POLYLINE with no vertices, or ENDSEC/EOF.
                        emitPolyline();
                    }
                } else {
                    // PolyState::LW — any new entity ends it.
                    emitPolyline();
                }
            }

            // ── Section / EOF handling ──
            if (val == "SECTION") { awaitSecName = true; continue; }
            if (val == "ENDSEC")  { sec = Sec::None; awaitSecName = false; continue; }
            if (val == "EOF")     { break; }

            // ── Start new entity ──
            if (sec == Sec::Entities) {
                if (val == "3DFACE") {
                    in3DFace = true;
                    for (auto& v : fv) v[0] = v[1] = v[2] = 0.0f;
                } else if (val == "POLYLINE") {
                    polyState  = PolyState::Poly3DHead;
                    pa         = PolyAccum{};
                    pa.is3D    = true;
                } else if (val == "LWPOLYLINE") {
                    polyState  = PolyState::LW;
                    pa         = PolyAccum{};
                    pa.is3D    = false;
                }
                // VERTEX / SEQEND transitions are already handled above.
            }
            continue;
        }

        // ── Section name (code 2 after SECTION marker) ────────────────────
        if (awaitSecName && code == 2) {
            awaitSecName = false;
            if      (val == "HEADER")   sec = Sec::Header;
            else if (val == "ENTITIES") sec = Sec::Entities;
            else                         sec = Sec::Other;
            continue;
        }

        // ── HEADER section: capture $EXTMIN ───────────────────────────────
        if (sec == Sec::Header) {
            if (awaitExtmin) {
                if      (code == 10) { origin[0] = std::stof(val); extminBits |= 1; }
                else if (code == 20) { origin[1] = std::stof(val); extminBits |= 2; }
                else if (code == 30) { origin[2] = std::stof(val); extminBits |= 4; }
                if (extminBits == 7) { originFound = true; awaitExtmin = false; }
                continue;
            }
            if (code == 9 && val == "$EXTMIN") {
                awaitExtmin = true; extminBits = 0;
            }
            continue;
        }

        // ── ENTITIES section: accumulate entity-specific data ─────────────
        if (sec == Sec::Entities) {
            if (in3DFace) {
                // 3DFACE: accumulate up to 4 vertices (codes 10–13, 20–23, 30–33).
                if (code >= 10 && code <= 13) {
                    try { fv[code - 10][0] = std::stof(val); } catch (...) {}
                } else if (code >= 20 && code <= 23) {
                    try { fv[code - 20][1] = std::stof(val); } catch (...) {}
                } else if (code >= 30 && code <= 33) {
                    try { fv[code - 30][2] = std::stof(val); } catch (...) {}
                }
            } else if (polyState == PolyState::Poly3DHead) {
                // POLYLINE entity header: capture layer and XDATA.
                // gc10/20/30 in the header are dummy zeros — ignored.
                if (code == 8) {
                    pa.layer = val;
                } else if (code == 1001) {
                    flushXdata();
                    pa.xApp   = val;
                    pa.inXdata = true;
                } else if (code >= 1000 && pa.inXdata) {
                    pa.xVals.push_back(val);
                }
            } else if (polyState == PolyState::Poly3DVert) {
                // VERTEX sub-entity: accumulate one 3D point.
                if      (code == 10) { try { pa.vx = std::stof(val); pa.vHasX = true; } catch (...) {} }
                else if (code == 20) { try { pa.vy = std::stof(val); } catch (...) {} }
                else if (code == 30) { try { pa.vz = std::stof(val); } catch (...) {} }
                // VERTEX sub-entities do not carry XDATA — ignore codes >= 1000.
            } else if (polyState == PolyState::LW) {
                // LWPOLYLINE: capture layer, constant elevation, XY vertex pairs, XDATA.
                if (code == 8) {
                    pa.layer = val;
                } else if (code == 38) {
                    try { pa.lwElev = std::stof(val); } catch (...) {}
                } else if (code == 10) {
                    try { pa.lwX = std::stof(val); pa.lwHasX = true; } catch (...) {}
                } else if (code == 20) {
                    if (pa.lwHasX) {
                        try {
                            pa.verts.push_back({ pa.lwX, std::stof(val), pa.lwElev });
                        } catch (...) {}
                        pa.lwHasX = false;
                    }
                } else if (code == 1001) {
                    flushXdata();
                    pa.xApp   = val;
                    pa.inXdata = true;
                } else if (code >= 1000 && pa.inXdata) {
                    pa.xVals.push_back(val);
                }
            }
        }
    } // while readPair

    // ── Post-loop cleanup ─────────────────────────────────────────────────

    // Emit any polyline that was in progress when the file ended.
    if (polyState != PolyState::None) {
        if (polyState == PolyState::Poly3DVert && pa.vHasX)
            pa.verts.push_back({ pa.vx, pa.vy, pa.vz });
        emitPolyline();
    }

    // Flush any terrain faces still in RAM.
    if (!tileBufs.empty())
        flushTileBuffers(cacheDir, tileBufs, tileCounts);

    progress.store(1.0f);

    // Finalise: convert every .tmp → tile_X_Y_lod0.bin.
    for (auto& [key, count] : tileCounts)
        finalizeTile(cacheDir, key, count);

    result.origin    = origin;
    result.faceCount = totalFaces;

    if (!originFound)
        result.warnings.push_back("$EXTMIN not found — origin set to (0, 0, 0)");
    if (malformed)
        result.warnings.push_back("Skipped " + std::to_string(malformed) +
                                   " degenerate face(s)");

    return result;
}

} // namespace dxf
