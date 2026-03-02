# CLAUDE.md — Terrain Viewer Bootstrap
# READ THIS FILE FIRST. Every session. No exceptions.
# After reading, summarise current state before writing any code.

---

## System architecture

This is a three-tier system. You are building Tier 3 (the client viewer).
Tiers 1 and 2 are built in Phases 10–12 — do not implement them during Phases 0–9.

```
TIER 1 — Data Pipeline (Python)
  Pulls point data from external survey APIs
  Incrementally re-triangulates affected tiles
  Writes per-tile DXF to Tier 2 source/ directory

TIER 2 — Terrain Server (Python/FastAPI)
  Watches source/ for updated DXFs
  Bakes DXF → binary .bin tiles (same TLET format as local cache)
  Serves /manifest and /tiles/{x}/{y}/{lod}.bin to clients

TIER 3 — Client Viewer (C++/DX11) ← YOU ARE HERE
  Polls server manifest, downloads stale/missing tiles
  Renders from local disk cache (unchanged from single-machine design)
  Shows freshness overlay and offline indicator
```

---

## What this project is

Windows C++17 / DirectX 11 desktop application for viewing civil/survey terrain
surfaces in interactive 3D. Target deployment: Panasonic Toughpad FZ-G1 MK4
field tablet (Intel HD Graphics 520, 8 GB RAM, Windows 10/11, 10.1" touch).

Full specification:      docs/scope_v0.6.docx
Development process:     docs/dev_guide_v1.2.docx

---

## Current state
*Update this block at the end of every session.*

```
Phase:           5 — COMPLETE  /  Phase 6 — not started
Last completed:  Phase 5 — LineworkPass GS quad expansion, tagged v5.0
Next task:       Phase 6 (or as scoped by next brief)
Known issues:    None
Broken:          Nothing
```

---

## Build

```powershell
# Configure (run once, or after CMakeLists changes)
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build/release

# Run
.\build\release\TerrainViewer.exe

# Parser unit tests
cmake --build build/release --target parser_tests
.\build\release\dxf_parser\parser_tests.exe
```

---

## Architecture rules — NEVER violate

```
INDEX BUFFERS    uint32 only. Never uint16.
GPU VERTICES     Always origin-offset before upload. Never raw MGA coordinates on GPU.
DXF PARSER       dxf_parser/ has ZERO dependency on DX11 headers. Must compile standalone.
GPS INTERFACE    Camera only receives ScenePosition {float x, y, z, heading; bool valid}.
                 All coordinate translation is internal to gps/ and crs/ only.
DX11 RESOURCES   Microsoft::WRL::ComPtr<T> from <wrl/client.h> for all DX11 objects.
CONSTANTS        TILE_SIZE_M, GPU_BUDGET_MB, LOD_RATIOS, LOD_DISTANCES_M defined
                 ONLY in src/terrain/Config.h. No magic numbers anywhere else.
SHADERS          D3DCompile runtime (d3dcompiler_47.dll) for Phases 0–8.
                 Offline .cso compilation in Phase 9 only.
DISK TILE CACHE  No full-mesh RAM retention. Parser streams in 50k-face chunks.
                 Tile data always on disk. GPU eviction releases GPU buffers only.
DEAR IMGUI       v1.91.6 vendored in third_party/imgui/. Win32+DX11 backends only.
SCENE ORIGIN     Local mode:  terrain $EXTMIN as scene origin (unchanged).
                 Server mode: use server project_origin from session.server.project_origin.
                 Never use raw $EXTMIN as origin when server project_origin is available.
PROJECT ORIGIN   Computed ONCE by chunk.py: floor($EXTMIN.X/1000)*1000, same for Y, Z=0.
                 Stored in config.json as project_origin. NEVER recomputed or overwritten.
                 chunk.py must check: if project_origin exists in config.json, do not write.
                 Client fetches via GET /config on first server connection.
                 If server project_origin changes: purge entire local tile cache.
TILE CLIPPING    chunk.py MUST clip triangles at tile boundaries using Shapely.
                 NEVER assign by centroid (causes holes).
                 NEVER duplicate to all overlapping tiles (causes Z-fighting).
                 New boundary vertices: Z interpolated barycentrically from original
                 triangle plane. Shapely guarantees identical coords on shared edges.
SERVER TILES     Downloaded .bin files land in the same local cache directory as parsed
                 tiles. TileGrid and GpuBudget code do not change.
TIER SEPARATION  Phases 0–9: client only. Never implement server or pipeline code
                 in the client codebase.
```

---

## Config.h constants

```cpp
// src/terrain/Config.h — ONLY place these values are defined
constexpr float    TILE_SIZE_M             = 50.0f;
constexpr int      GPU_BUDGET_MB            = 192;
constexpr float    LOD_RATIOS[]             = { 1.0f, 0.15f, 0.02f };
constexpr float    LOD_DISTANCES_M[]        = { 150.0f, 400.0f };
constexpr float    LINEWORK_WIDTH_PX        = 2.0f;
constexpr int      PARSE_CHUNK_FACES        = 50000;
constexpr int      MAX_TILE_LOADS_PER_FRAME = 3;
constexpr float    GPS_HEADING_ALPHA        = 0.15f;
constexpr float    GPS_MOVE_THRESHOLD_M     = 1.0f;
constexpr float    GPS_MIN_SPEED_MS         = 0.5f;
constexpr int      OFFLINE_WARN_HOURS       = 4;
constexpr int      MANIFEST_POLL_SECONDS    = 60;
constexpr int      PREFETCH_RADIUS_TILES    = 2;
constexpr int      MAX_CONCURRENT_DOWNLOADS = 2;
```

---

## GPS internal abstraction

```cpp
struct ScenePosition {
    float x, y, z;   // scene-space, origin-offset applied
    float heading;   // degrees from north, smoothed
    bool  valid;
};

class IGpsSource {
public:
    virtual ScenePosition poll() = 0;
    virtual bool isConnected() = 0;
    virtual ~IGpsSource() = default;
};
// Translation pipeline internal to gps/ only:
// NMEA → WGS84 → MGA (PROJ) → subtract terrain origin → ScenePosition
```

---

## Disk tile cache

```
Location:  %TEMP%\TerrainViewer\tiles\{file_hash}\
Per tile:  tile_{x}_{y}_lod{0|1|2}.bin

Binary format:
  uint32  magic     = 0x544C4554  ('TLET')
  uint32  version   = 1
  uint32  lod
  uint32  vertCount
  uint32  indexCount
  TerrainVertex[vertCount]   (28 bytes each: float3 pos, float3 normal, uint32 color)
  uint32[indexCount]

cache.meta (JSON):
  {
    "source_path":          string,
    "file_mtime":           string,
    "server_last_modified": string | null,   ← null for local-only
    "downloaded_at":        string | null,   ← null for local-only
    "tile_count":           int,
    "origin":               [float, float, float]
  }
```

---

## Server endpoints (Phase 10+)

```
GET /config     → { project_origin[3], tile_size_m, mga_zone, tile_grid_w, tile_grid_h }
                  Returns HTTP 503 if chunk.py has not been run yet.
GET /health     → { status: "ok", tile_count: N }
GET /manifest   → JSON array (see below)
GET /tiles/{x}/{y}/{lod}.bin  → binary TLET file
GET /tiles/{x}/{y}/info       → { source_file, survey_date, last_modified, size_bytes }
```

## Server config.json schema (Phase 10+)

```json
{
  "project_origin":  [436000.0, 7563000.0, 0.0],
  "tile_size_m":     50.0,
  "tile_grid_w":     20,
  "tile_grid_h":     20,
  "mga_zone":        54,
  "port":            8765,
  "source_dir":      "./source",
  "cache_dir":       "./cache",
  "log_level":       "INFO"
}
```

project_origin write rule: chunk.py writes it ONCE on first run.
Any subsequent run must check if it exists and NOT overwrite it.

## Server manifest format (Phase 11)

```json
[
  {
    "tile_x": 3,
    "tile_y": 7,
    "last_modified": "2026-03-01T14:23:00Z",
    "lod_count": 3,
    "size_bytes": 4915200,
    "source_file": "tile_3_7.dxf",
    "survey_date": "2026-02-28"
  }
]
```

---

## Freshness indicator (Phase 11)

```
Offline banner: shown when time_since_last_server_contact > OFFLINE_WARN_HOURS
  "⚠ Offline 5h 23m — terrain data may be outdated"
  Persistent, non-blocking, clears on reconnect.
  Store last_connected_at in session.json.

Freshness overlay (F key toggle):
  Colour tiles by server last_modified age:
    Green  = < 24 hours
    Yellow = 1–7 days
    Orange = 7–30 days
    Red    = > 30 days or never downloaded from server
  Tap tile → tooltip: source_file, survey_date, last_modified, downloaded_at
```

---

## XDATA in linework

```cpp
struct XDataEntry {
    std::string appName;
    std::vector<std::string> values;
};
// ParsedPolyline.xdata — may be empty. Parsed Phase 1, displayed Phase 2+.
```

---

## Dependencies

```
vcpkg.json:  meshoptimizer, proj, nlohmann-json, catch2
             + libcurl (added Phase 11 for tile downloads)
third_party/imgui/  Dear ImGui v1.91.6 (vendored)
```

---

## Sample files

```
docs/sample_data/terrain.dxf        10,900  3DFACE triangles
docs/sample_data/0210_SL_TRI.dxf    47,762  3DFACE triangles
docs/sample_data/0217_SL_CAD.dxf       446  3D POLYLINE + 1,550 LWPOLYLINE
```

---

## Session protocol

```
START:   Read CLAUDE.md and DEVLOG.md. State current phase and next task.
         Do not write code until confirmed.

END:     1. Green build (cmake --build succeeds)
         2. Run parser_tests if parser touched
         3. Append DEVLOG.md entry
         4. git add . && git commit -m "[Phase N] Description"
         5. Phase complete: git tag -a vN.0 -m "Phase N complete"

NEVER:   - Leave a broken build
         - Modify docs/scope_v0.6.docx or past DEVLOG entries
         - uint16 index buffers
         - Raw MGA coordinates in GPU buffers
         - DX11 headers in dxf_parser/
         - Magic numbers instead of Config.h constants
         - CoordTransform outside gps/ or crs/
         - Full parsed mesh in RAM
         - Server or pipeline code in the client repo (Phases 0–9)
         - Use terrain $EXTMIN as origin when server project_origin is available
         - Overwrite project_origin in config.json if it already exists
         - Assign boundary-crossing triangles by centroid or by duplication
```

---

## Target hardware

```
Device:  Panasonic Toughpad FZ-G1 MK4
CPU:     Intel Core i5-6300U (Skylake, 2c/4t, 2.4 GHz)
GPU:     Intel HD Graphics 520 (Gen 9, 24 EU, 900 MHz, shared VRAM)
RAM:     8 GB
Display: 10.1" 1920×1200 (224 ppi), multi-touch
OS:      Windows 10/11 Pro
Target:  30 fps both surfaces + GPS + tile streaming active
```
