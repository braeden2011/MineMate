#pragma once
// dxf_parser/DxfTypes.h
// DXF parser data types.
// NO DX11 HEADERS — this file must compile standalone.

#include <array>
#include <string>
#include <vector>

namespace dxf {

// A single triangular face in scene-space (origin already subtracted).
struct ParsedFace {
    float v[3][3];    // three vertices: v[vertex][xyz]
    float normal[3];  // outward flat normal, z >= 0
};

// XDATA entry attached to a polyline entity (Phase 1: parse & store).
struct XDataEntry {
    std::string appName;              // group code 1001
    std::vector<std::string> values;  // group codes 1000, 1005, 1010, etc.
};

// A parsed polyline entity (POLYLINE or LWPOLYLINE).
struct ParsedPolyline {
    bool is3D;                                // true = 3D POLYLINE, false = LWPOLYLINE
    std::vector<std::array<float, 3>> verts;  // scene-space vertices (origin-offset applied)
    std::string layer;
    int colorAci = 256;                       // AutoCAD Color Index from group code 62;
                                              // 256 = BYLAYER (no explicit entity colour)
    std::vector<XDataEntry> xdata;  // may be empty
};

// Summary returned by parseToCache().
struct ParseResult {
    std::array<float, 3>        origin;         // $EXTMIN raw MGA coords
    size_t                      faceCount;
    size_t                      polylineCount;
    size_t                      tileCount;      // number of LOD0 tile bins written (or loaded from cache)
    std::vector<ParsedPolyline> polylines;      // all parsed polylines (origin-offset); empty on cache hit
    std::vector<std::string>    warnings;
};

// Packed GPU vertex as stored in .bin tile cache files.
// 7 floats = 28 bytes per vertex.
struct TerrainVertex {
    float px, py, pz;   // scene-space position (origin-offset applied)
    float nx, ny, nz;   // flat face normal
    float color;        // elevation-based tint (Phase 2+); 0.0f in Phase 1
};

static_assert(sizeof(TerrainVertex) == 28, "TerrainVertex must be 28 bytes");

} // namespace dxf
