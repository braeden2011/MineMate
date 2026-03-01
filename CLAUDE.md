# CLAUDE.md — Terrain Viewer Bootstrap
# READ THIS FILE FIRST. Every session. No exceptions.
# After reading, summarise current state before writing any code.

---

## What this project is

Windows C++17 / DirectX 11 desktop application for viewing civil/survey terrain
surfaces in interactive 3D. Target deployment: Panasonic Toughpad FZ-G1 MK4
field tablet (Intel HD Graphics 520, 8 GB RAM, Windows 10/11, 10.1" touch).

Full specification:      docs/scope_v0.5.docx
Development process:     docs/dev_guide_v1.1.docx

---

## Current state
*Update this block at the end of every session.*

```
Phase:           1 — In progress
Last completed:  Phase 1 Session 1 — DXF types, 3DFACE parser, streaming disk cache
Next task:       Phase 1 Session 2 — Polyline parser, XDATA, LOD generation
Known issues:    None
Broken:          Nothing
```

---

## Build

```powershell
# cmake.exe is inside the VS2022 install — use the full path:
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

# Configure (run once, or after CMakeLists changes)
& $cmake -B build\release -G "Visual Studio 17 2022" -A x64 `
         -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake

# Build all targets
& $cmake --build build\release --config Release

# Run
.\build\release\Release\TerrainViewer.exe

# Parser unit tests (fast — excludes [slow] tag)
& $cmake --build build\release --config Release --target parser_tests
.\build\release\dxf_parser\Release\parser_tests.exe "~[slow]"
```

---

## Architecture rules — NEVER violate

```
INDEX BUFFERS    uint32 only. Never uint16. A single 50m tile can exceed 65535 vertices.
GPU VERTICES     Always origin-offset before upload. Never raw MGA coordinates on GPU.
DXF PARSER       dxf_parser/ has ZERO dependency on DX11 headers. Must compile standalone.
GPS INTERFACE    Camera only receives ScenePosition {float x, y, z}.
                 All coordinate translation (NMEA→WGS84→MGA→scene) is internal to gps/.
                 CoordTransform.h is never included outside gps/ and crs/.
DX11 RESOURCES   Microsoft::WRL::ComPtr<T> from <wrl/client.h> for all DX11 objects.
                 No raw COM pointer ownership anywhere.
CONSTANTS        TILE_SIZE_M, GPU_BUDGET_MB, LOD_RATIOS, LOD_DISTANCES_M are defined
                 ONLY in src/terrain/Config.h. No magic numbers anywhere else.
SHADERS          D3DCompile runtime (d3dcompiler_47.dll) for Phases 0–8.
                 Switch to offline .cso compilation in Phase 9 (release build only).
SAME ZONE        All surfaces (terrain, design, linework) are guaranteed to be in the
                 same MGA zone. Single origin from terrain $EXTMIN. No per-object
                 world matrix offset required.
DEAR IMGUI       Version 1.91.6, vendored in third_party/imgui/.
                 Backends: imgui_impl_win32.cpp + imgui_impl_dx11.cpp only.
DISK TILE CACHE  Parser streams in 50k-face chunks. Tile data serialised to disk
                 immediately. GPU eviction releases GPU buffer only — tile data
                 is always on disk. No full-mesh RAM retention.
```

---

## Config.h constants

```cpp
// src/terrain/Config.h — ONLY place these values are defined
constexpr float    TILE_SIZE_M        = 50.0f;
constexpr int      GPU_BUDGET_MB      = 192;
constexpr float    LOD_RATIOS[]       = { 1.0f, 0.15f, 0.02f };
constexpr float    LOD_DISTANCES_M[]  = { 150.0f, 400.0f };
constexpr float    LINEWORK_WIDTH_PX  = 2.0f;
constexpr int      PARSE_CHUNK_FACES  = 50000;
```

---

## GPS internal abstraction

```cpp
// Camera and render code ONLY see this:
struct ScenePosition {
    float x, y, z;   // scene-space, origin-offset applied
    float heading;    // degrees from north, smoothed
    bool  valid;
};

// IGpsSource interface — in src/gps/IGpsSource.h
class IGpsSource {
public:
    virtual ScenePosition poll() = 0;
    virtual bool isConnected() = 0;
    virtual ~IGpsSource() = default;
};

// Translation pipeline (internal to gps/ only):
// NMEA sentence → WGS84 decimal degrees + altitude
//   → MGA easting/northing/elevation (via PROJ, configured CRS)
//   → subtract terrain origin ($EXTMIN)
//   → ScenePosition
```

---

## Disk tile cache

```
Location:  %TEMP%\TerrainViewer\tiles\{file_hash}\
           e.g. C:\Users\...\AppData\Local\Temp\TerrainViewer\tiles\a3f7c2\

Per tile:  tile_{x}_{y}_lod{0|1|2}.bin

Binary format:
  uint32  magic     = 0x544C4554  ('TLET')
  uint32  version   = 1
  uint32  lod       (0, 1, or 2)
  uint32  vertCount
  uint32  indexCount
  float[vertCount*8]  vertex data  (pos.xyz, normal.xyz, color as float)
  uint32[indexCount]  index data

Parse flow:
  1. Read $EXTMIN from DXF header → origin
  2. Scan $EXTMAX → compute tile grid dimensions
  3. Stream entities in PARSE_CHUNK_FACES batches:
       a. Parse faces from current chunk
       b. Bin faces into tiles by XY world position
       c. Append each tile's faces to its .bin file on disk (LOD0 only)
       d. Clear chunk from RAM
  4. After all chunks: generate LOD1 and LOD2 per tile using meshoptimizer
     (read LOD0 bin → simplify → write LOD1/LOD2 bins)
  5. Report complete. RAM usage during parse: one chunk only.

GPU tile load: read .bin file → upload VB + IB → mark tile GPU-resident
GPU eviction:  release VB + IB → tile marked disk-only (bin file always present)
Cache cleanup: delete tile cache dir on clean app exit (optional, configurable)
```

---

## XDATA in linework

```cpp
struct XDataEntry {
    std::string appName;          // group code 1001
    std::vector<std::string> values;  // group codes 1000, 1005, 1010, etc.
};

// ParsedPolyline gets:
std::vector<XDataEntry> xdata;   // may be empty if no XDATA present

// Phase 1: parse and store
// Phase 2+: display on tap/hover (future)
```

---

## Dependencies

```json
vcpkg.json:
  "meshoptimizer"   LOD generation
  "proj"            WGS84 <-> GDA94/MGA (internal to gps/ only)
  "nlohmann-json"   session config
  "catch2"          parser unit tests

third_party/imgui/  Dear ImGui v1.91.6 (vendored — NOT via vcpkg)
                    Backends: imgui_impl_win32, imgui_impl_dx11
```

---

## Sample files

```
docs/sample_data/terrain.dxf        ~6,339,775  3DFACE triangles  (2.4 GB — the large production file)
docs/sample_data/0217_SL_TRI.dxf       47,287  3DFACE entities (45,913 valid + 1,374 degenerate)
docs/sample_data/0217_SL_CAD.dxf          446  3D POLYLINE
                                         1,550  LWPOLYLINE

Note: the spec doc (scope_v0.5.docx) lists different counts and filenames — the above
are the actual values confirmed by running the parser. Spec numbers were wrong.
```

---

## Session protocol

```
START:   Read CLAUDE.md and DEVLOG.md. State current phase and next task.
         Do not write code until you have done this.

END:     1. Confirm build is green (cmake --build succeeds, no errors)
         2. Run parser_tests if parser code was touched
         3. Append to DEVLOG.md: what was done, decisions made, current state
         4. git add . && git commit -m "[Phase N] Description"
         5. If phase is complete: git tag -a vN.0 -m "Phase N complete"

NEVER:   - Leave a broken build
         - Modify docs/scope_v0.5.docx or past DEVLOG.md entries
         - Change uint32 index buffers to uint16
         - Put raw MGA coordinates in GPU vertex buffers
         - Include DX11 headers in dxf_parser/
         - Use magic numbers instead of Config.h constants
         - Include CoordTransform outside gps/ or crs/
         - Retain the full parsed mesh in RAM (use disk tile cache)
         - Add features not in the current session brief
```

---

## Phase completion tags

```powershell
# Run at the end of each phase (after final green build commit):
git tag -a v0.0 -m "Phase 0 complete — scaffold"
git tag -a v1.0 -m "Phase 1 complete — DXF parser"
git tag -a v2.0 -m "Phase 2 complete — basic renderer"
# ... and so on

# To restore to a specific phase:
git checkout v3.0
```

---

## Target hardware reference

```
Device:  Panasonic Toughpad FZ-G1 MK4
CPU:     Intel Core i5-6300U (Skylake, 2c/4t, 2.4 GHz)
GPU:     Intel HD Graphics 520 (Gen 9, 24 EU, 900 MHz)
RAM:     8 GB (shared with GPU — GPU budget 192 MB)
Display: 10.1" 1920×1200 (224 ppi)
OS:      Windows 10/11 Pro
Touch:   Digitizer + multi-touch (WM_POINTER)

Performance target: 30 fps with both 5M-triangle surfaces visible simultaneously.
File size: unknown upper bound — disk tile cache is mandatory, not optional.
```
