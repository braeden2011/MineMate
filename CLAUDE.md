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
Phase:           9 — Session 4 complete
Last completed:  Phase 9 S4 — perf (60fps cap), RAM (polylines evicted on uncheck), popup bottom-left
Next task:       Phase 10 (server) or F1 (cross-section) backlog
Known issues:    - GPU_BUDGET_MB raised to 512 (was 192 in spec) — intentional for scale
                 - MAX_TILE_LOADS_PER_FRAME raised to 100 (was 3) — effectively unlimited
                 - Offline fxc compilation adopted in Phase 8 S3 (spec said Phase 9)
                 - All UI lives in main.cpp — src/ui/ directory never created
                 - TerrainVertex.color is float (elevation tint), not uint32 as specced
                 - GPS_HEIGHT_OFFSET_M in Config.h may be dead (superseded by session.json)
                 - Dead POST_BUILD copy: 0210_SL_TRI.dxf (file is 0217_SL_TRI.dxf)
                 - Stale comment in linework.hlsl re: per-layer colour
                 - Stale comment in DesignPass.h re: depth bias (removed in 530c41e)
                 - No GPS position marker rendered on screen (camera follows but no icon)
                 - Old individual DXF file-picker paths removed; folder-based loading only now
                 - GPS_MGA_ZONE = 55 in Config.h (local default; Phase 10 will use server)
                 - Opacity < 1 does not allow surfaces behind to show through (depth write
                   not disabled for transparent surfaces — see BUG-2 in feature backlog)
                 - On design uncheck, polylines cleared from RAM; re-enable re-parses from
                   cache (fast) rather than restoring from in-memory cache
Broken:          Nothing — clean build
```

## Actual Config.h values (deviations from spec)

```
GPU_BUDGET_MB            = 512    (spec: 192 — raised for production DXF scale)
MAX_TILE_LOADS_PER_FRAME = 100    (spec: 3   — effectively unlimited per frame)
GPS_VIEW_DISTANCE_M      = 50.0f  (not in spec — added for GPS camera mode)
GPS_MGA_ZONE             = 55     (not in spec — local default, Phase 10 uses server)
SESSION_AUTOSAVE_SECONDS = 60     (not in spec — added Phase 8 S1)
```
All other constants match spec exactly.

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

## Incremental update pipeline (Phase 12)

**Step 1 — Identify dirty tiles**
Bin incoming API points to tiles by XY. Result: set D of dirty tiles.

**Step 2 — Point update per dirty tile**
Sub-grid approach (configurable via point_replace_cell_m in config.json):
- Divide tile into sub-cells (e.g. 5m × 5m = 10×10 grid within 50m tile)
- For each sub-cell with at least one new point: drop ALL existing points in that sub-cell
- Add new points for that sub-cell
- Sub-cells with no new data: existing points untouched
- Write updated point set back to tile source DXF

**Step 3 — Expand to triangulation region**
For each connected group of dirty tiles (contiguous, including diagonals):
- Add triangulation_buffer_rings (default 1) of buffer tiles around the entire group
- Triangulation region T = dirty group + buffer ring(s)

**Step 4 — Triangulate each region**
For each region T:
- Load all points from ALL tiles in T (dirty + buffer)
- Run single Delaunay triangulation on combined point set
- Produces one continuous seamless surface across entire region

**Step 5 — Clip and write (dirty AND buffer tiles)**
For each tile in T:
- Clip triangulation output to tile boundary (same Shapely algorithm as chunk.py)
- Write new source DXF
- Baker re-bakes lod0/lod1/lod2 .bin files
- Manifest updated with new last_modified
- Buffer tiles are re-baked as a side effect — this is intentional and correct

**Step 6 — Client picks up changes**
Manifest poll detects all re-baked tiles. Client downloads dirty + buffer tiles.
All seams between updated and unchanged areas are geometrically consistent.

**Shared code rule:**
chunk.py and triangulate.py share the same core clip-and-write function.
baker.py accepts a REGION (list of tiles) not a single tile.
The only difference between initial setup and incremental update is how the
input point set is assembled.

**config.json additions for Phase 12:**
  point_replace_cell_m:       5.0  (sub-cell size for point replacement, TBD)
  triangulation_buffer_rings: 1    (buffer tile rings around dirty region, TBD)

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
Client (vcpkg.json):
  meshoptimizer   — local mode LOD generation (see LOD paths below)
  proj            — GPS coordinate transform
  nlohmann-json   — session.json and manifest parsing
  catch2          — parser unit tests
  libcurl         — tile downloads (added Phase 11)

third_party/imgui/  Dear ImGui v1.91.6 (vendored)

Server (requirements.txt):
  fastapi         — REST endpoints
  uvicorn         — ASGI server
  watchdog        — filesystem monitor on source/
  shapely         — triangle boundary clipping in chunk.py
  numpy           — point array operations
  trimesh         — LOD generation in baker.py (simplify_quadric_decimation)
```

## LOD generation — two paths

LOCAL MODE (client loads DXF directly, no server):
  Client parser generates LOD1 and LOD2 locally using meshoptimizer.
  meshoptimizer stays in vcpkg.json permanently — it is NOT redundant.
  This is the path Phase 1 implemented. Do not remove it.

SERVER MODE (client downloads pre-baked tiles):
  Server baker.py generates LOD1 and LOD2 using trimesh at bake time.
  Client receives pre-baked lod0/lod1/lod2.bin — no local LOD generation.
  trimesh lives on the server only. Never add it to the client.

Both paths produce .bin files in identical TLET format.
TileGrid and GpuBudget code do not know or care which path was used.

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

## Feature backlog

### F3 — Modern UI overhaul

**Scope:** Visual polish pass on all Dear ImGui panels. No functional changes.

**ImGui style:**
- Base: ImGui::StyleColorsDark()
- WindowRounding, FrameRounding, PopupRounding, ScrollbarRounding = 6.0f
- Increase ItemSpacing, FramePadding, WindowPadding for breathing room
- Accent colour (e.g. #2E75B6) for active states, selected items, progress bars
- Colour used only for status: green=good, orange=warning, red=error/cut
- Replace CollapsingHeader sections in sidebar with simple bold label + separator divider
- Sidebar: no title bar, no resize handle, fixed right edge, no scrollbar if fits

**Font:**
- Load a clean sans-serif via ImGui::GetIO().Fonts->AddFontFromFileTTF()
- Segoe UI (available on all Windows 10/11) at 16px for body, 13px for labels
- Fall back to ImGui default if font file not found (no hard dependency)

**LHS button bar:**
- Icons or single glyphs instead of text where unambiguous (+, -, O, =, X)
- Consistent size and spacing, subtle hover highlight

**Offline banner:**
- Slim top bar, not a floating panel — consistent height, non-intrusive

---

### F4 — Start fullscreen

**Behaviour:**
- On first launch (no saved window state): SW_SHOWMAXIMIZED
- On subsequent launches: restore saved window state from session.json
- Save maximized/restored state in session.json window block
- Maximized state does not save pixel dimensions — just the maximized flag

**Implementation:**
- Check session.json window.maximized on startup
- If true or first run: ShowWindow(hwnd, SW_SHOWMAXIMIZED)
- Save window.maximized = IsZoomed(hwnd) on session save

---

### F5 — Coord popup: click-outside dismiss + simplified content

**Dismissal change:**
- Current: explicit close button required
- New: clicking anywhere outside the popup rect dismisses it immediately
- Drag threshold still applies for the initial pick (0.5s hold, < 8px drag)
- Touch: tap anywhere outside popup dismisses

**Content simplification — remove:**
- WGS84 lat/lon
- Zone display
- Datum display
- "MGA" label

**Content to keep — new format:**
```
Surface    Terrain
E          436 234.5
N        7 563 891.2
Z            148.3 m
Cut/Fill      +2.1 m  ← green if fill, orange if cut. Hidden if only one surface.
Data       2h 14m ago ← from F2 / server_last_modified. "Local" if no server data.
```

Numbers right-aligned. Thousands separator on E and N for readability.
No close button — click outside to dismiss.

---

### F6 — Folder-based design loading

**Replaces:** Individual file pickers for terrain, design surface, linework.

**Terrain folder:**
- User selects one folder (IFileOpenDialog, pick folder mode)
- Software scans for .dxf files, loads the first one found as terrain
- Always loaded on startup if folder path is set — no checkbox
- TBD: multiple terrain DXFs in one folder (load all or just first — decide later)

**Design folder:**
- User selects one folder
- Software scans folder and groups files into design SETS by base name

**Design set detection — naming convention:**
```
BASENAME_TRI.dxf  → surface (3DFACE entities)       ← Case A paired
BASENAME_CAD.dxf  → linework (POLYLINE entities)     ← Case A paired
BASENAME.dxf      → single file, both surface + linework ← Case B single
```

Detection logic (in order):
1. Files ending _TRI.dxf → paired surface. Look for matching _CAD.dxf.
2. Files ending _CAD.dxf without matching _TRI → linework-only set.
3. Files with no _TRI/_CAD suffix → single DXF (parse for both 3DFACE and POLYLINE).
4. If both paired AND single exist for same base name → warn user, prefer paired.

Edge cases handled:
- _TRI only (no _CAD) → surface loads, no linework, no error
- _CAD only (no _TRI) → linework loads, no surface, no error
- Single DXF with no 3DFACE entities → linework only
- Single DXF with no POLYLINE entities → surface only

**UI in sidebar:**
- "Terrain folder:" path display + Browse button
- "Designs folder:" path display + Browse button
- Terrain row: visibility toggle checkbox + "Terrain" label (folder name or filename)
- Design rows: one row per detected design set. Each row has:
  - Checkbox: controls both loaded AND visible. Unchecking unloads from GPU,
    frees budget. Re-checking reloads from disk cache.
  - Design base name label
- Multiple designs can be active simultaneously
- Each active design: loads surface + linework based on detected case

**Visibility and coord pick interaction:**
- Coord pick (click-and-hold) only ray-casts against VISIBLE surfaces.
  Hidden terrain → no terrain Z returned. Hidden design → not included in pick.
- Cut/Fill: only computed if BOTH terrain AND at least one design surface are visible.
  If terrain is hidden: show design Z only, no cut/fill.
  If design is hidden: show terrain Z only, no cut/fill.
- RL (elevation) reported in popup is always from the visible surface hit.

**Multiple design surfaces at same pick point:**
- If ray hits surfaces from 2 or more active design sets at the same XY:
  - Show each surface as a separate row in the coord popup
  - Label each row with the design base name it came from
  - Example:
    ```
    Surface    Terrain
    Z            148.3 m

    Surface    DesignA
    Z            150.1 m
    Cut/Fill      +1.8 m fill

    Surface    DesignB
    Z            149.4 m
    Cut/Fill      +1.1 m fill

    Data       2h 14m ago
    ```
  - Each design row shows its own cut/fill relative to terrain
  - If terrain is hidden: design rows show Z only, no cut/fill
  - Rows ordered by Z descending (highest surface first)

**Session persistence:**
- Save terrain_folder path
- Save terrain_visible bool
- Save designs_folder path
- Save list of active design names (not file paths — re-scan on restore)
- Save visibility state per design name
- On restore: re-scan folder, re-match by name, load active + visible sets

**GPU budget:**
- Each active design surface + linework consumes GPU budget alongside terrain
- Unchecked (hidden) designs are evicted from GPU — budget freed
- GpuBudget already tracks total — no change needed

---

### BUG-1 — Coord pick crosshair locks to empty air

**Symptom:**
Rare. Click-and-hold pick gesture completes but crosshair appears at a position
in empty air. Only becomes obvious when camera is rotated — crosshair moves with
the view as if projected correctly but has no surface beneath it.

**Root cause (likely):**
The pick IS hitting triangles but the returned world position is wrong.
The crosshair appears at a plausible but incorrect 3D location — visible only
when rotating because a correctly-placed marker would stay on the surface.

Most likely causes in order of probability:

1. Ray construction from screen coordinates is wrong in some camera orientations.
   The ray origin or direction is computed incorrectly when azimuth/elevation
   put the camera in certain quadrants. Möller–Trumbore finds a real intersection
   but on the wrong ray → position is geometrically valid but not under the cursor.

2. Origin offset applied twice or not at all to the hit position.
   RayCastDetailed works in scene space but the returned hit position gets
   origin offset applied a second time before the crosshair is placed — or
   the offset is missing — shifting it off the surface by a fixed amount.
   This would explain why it appears at a consistent wrong offset that looks
   plausible from the pick angle but floats in air when rotated.

3. t value from Möller–Trumbore is correct but hit position is reconstructed
   as ray_origin + t * ray_dir using the wrong ray (e.g. NDC ray instead of
   world ray), producing a position on the correct triangle plane but in the
   wrong coordinate space.

4. Multi-tile front-to-back ordering issue: tiles sorted incorrectly, a further
   tile is tested before a closer one, returns a hit on a triangle that is
   behind the correct hit from the cursor's perspective.

**Fix:**
1. Audit ray construction: unproject cursor position through inverse VP matrix.
   Verify ray origin = camera eye position in scene space (not world/MGA space).
   Verify ray direction is normalised. Log ray origin + direction on pick and
   confirm they match expected values at known camera orientations.

2. Audit hit position reconstruction: confirm it is computed as
   ray_origin + t * ray_dir entirely in scene space, then origin offset is
   added back ONCE for display only (MGA coord readout). Never apply origin
   offset to the scene-space crosshair position.

3. Audit tile sort order in RayCastDetailed: tiles should be sorted by AABB
   near-hit t value ascending before triangle testing. Early exit on first hit.
   If sort is wrong, a far tile could be tested first.

4. Add debug logging (ImGui overlay, toggle with key): on each pick, display
   ray origin, ray direction, hit tile index, hit triangle index, t value,
   scene-space hit position, and the projected screen position of that hit.
   Compare projected screen position to actual cursor position — they must match.
   This will immediately isolate which step is wrong.

**Priority:** Fix in Phase 9 S2 before other UX work. Use debug overlay first
to isolate the exact step before changing any math.

---

### BUG-2 — Opacity < 1 does not reveal surfaces behind

**Symptom:**
When terrain or design opacity is reduced below 1.0, the surface becomes
semi-transparent visually but surfaces behind it (e.g. terrain behind a
design surface) are not visible through it.

**Root cause (likely):**
Alpha blending is enabled when opacity < 1, but depth write (D3D11_DEPTH_WRITE_MASK)
is not fully disabled. The transparent surface writes to the depth buffer, occluding
any geometry drawn after it at the same pixel.

Additionally, render order matters: back surfaces must be drawn BEFORE front surfaces
for correct alpha compositing. Currently terrain and design passes are independent
and not sorted by depth relative to camera.

**Fix (when prioritised):**
1. When opacity < 1: disable depth write entirely (D3D11_DEPTH_WRITE_MASK_ZERO).
   This is already partially implemented but may not be applied to all geometry.
2. Sort draw calls back-to-front when any surface is semi-transparent.
   Simplest: draw terrain first (always), then design surfaces front-to-back from
   camera. Since terrain is behind design by definition in most layouts this may
   be sufficient without explicit sorting.
3. Alternatively, add OIT (order-independent transparency) — but this is complex
   and not needed for Phase 9.

**Priority:** Low. Defer to after Phase 10 / F1.

---

### F1 — Cross-section tool

**Entry point:** Button in LHS button bar (alongside zoom +/-, reset view).
**Trigger:** User taps/clicks the cross-section button.

**Flow:**
1. Camera state (pivot, radius, azimuth, elevation) saved to a restore snapshot.
2. Tool enters PICK_A state. User clicks/taps first point on terrain surface
   (reuse existing RayCastDetailed pick mechanism). Point A marker placed.
3. Tool enters PICK_B state. User clicks/taps second point. Point B marker placed.
   A line is drawn on screen between A and B (foreground drawlist, same style as
   coord pick marker).
4. Tool enters CONFIRM state. User can:
   - Drag either marker to adjust position (snap to terrain surface on drag).
   - Confirm: proceed to cross-section view.
   - Cancel: restore camera snapshot, discard markers, exit tool.
5. On confirm: switch to 2D cross-section view.
   - Sample terrain (and design surface if loaded) along the A→B line at regular
     intervals (interval TBD — config or adaptive based on line length).
   - Render as a 2D profile: horizontal axis = distance along line (metres),
     vertical axis = elevation (metres). Both axes labelled.
   - Show terrain profile and design profile as separate lines if both loaded.
   - Cut/fill area between profiles filled green (fill) or orange (cut) if both present.
   - Dear ImGui window, full-width, docked to bottom or shown as overlay.
6. User closes cross-section panel: camera restored to saved snapshot exactly.

**Coordinate display in cross-section view:**
- Hover over profile: show distance from A, MGA E/N/Z at that point.
- Export button: save profile as CSV (distance, E, N, Z terrain, Z design, cut_fill_m).

**State:** Cross-section tool state persisted in session.json (last A/B points, open/closed).

**Out of scope for v1:** multiple cross-sections, 3D section plane visualisation,
volume calculation.

---

### F2 — Data freshness in coord pick popup

**Current behaviour:** Click-and-hold (0.5s, drag < 8px) shows coord pick popup
with surface label, MGA E/N/Z, WGS84 lat/lon, cut/fill.

**Addition:** Include data freshness information in the popup.

**Display:**
- "Data: 2h 14m ago" — time since server_last_modified for the tile under the pick point.
- If server_last_modified is null (local-only tile): show "Data: local" or "Data: unknown".
- If tile has never been downloaded from server: show "Data: local file".
- Freshness sourced from cache.meta server_last_modified for the tile containing the
  pick point XY.
- TileGrid already knows which tile a ray hits — read cache.meta for that tile.

**Implementation note:** TileGrid::RayCastDetailed already returns the hit tile index.
Add a GetTileFreshness(tile_idx) method that reads server_last_modified from that
tile's cache.meta and returns a human-readable age string.

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
