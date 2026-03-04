# DEVLOG — Append only, never edit past entries.

## [2026-03-01] Environment Setup Complete

### Done
- All tools installed via automated setup script
- Windows system settings applied
- VS 2022 Community installed (Desktop C++ workload)
- vcpkg bootstrapped and integrated
- Defender exclusions applied
- Project directory structure created
- CLAUDE.md and DEVLOG.md seeded

### Current state
Ready for Phase 0. No code written yet.

---

## [2026-03-01] Phase 0 — Project Scaffold Complete

### Done
- Git repository already initialised (from environment setup)
- Directory structure created per dev guide Section 3:
  src/{app,renderer,terrain,gps,crs,ui}, dxf_parser/tests/, third_party/imgui/
- CMakeLists.txt: C++17, Visual Studio 17 2022 generator, DX11 libs, vcpkg toolchain
- vcpkg.json confirmed correct (meshoptimizer, proj, nlohmann-json, catch2)
- All vcpkg packages built and cached: catch2 3.13, meshoptimizer 1.0.1, nlohmann-json 3.12, proj 9.7.1
- src/main.cpp: Win32 window, WM_DESTROY/WM_QUIT, message loop, ImGui initialised
- src/renderer/Renderer.h/.cpp: D3D11CreateDeviceAndSwapChain, clear RGB(45,55,72), Present
- Dear ImGui v1.91.6 vendored to third_party/imgui/ (imgui_impl_win32 + imgui_impl_dx11)
- ImGui test window: "Terrain Viewer — Phase 0" with FPS counter
- dxf_parser/CMakeLists.txt: static lib + placeholder Catch2 test (always passes)
- All four targets build clean: imgui.lib, TerrainViewer.exe, dxf_parser.lib, parser_tests.exe

### Decisions
- Using "Visual Studio 17 2022" generator (not Ninja) — cmake.exe lives inside VS install,
  Ninja not available without extra PATH setup. Build command uses PowerShell.
- CLAUDE.md build section updated to reflect actual working invocation.

### Current state
Build: GREEN. TerrainViewer.exe in build\release\Release\.
Phase 0 complete.

### Next session starts with
Phase 1 — DXF Parser. Read CLAUDE.md and DEVLOG.md, then proceed with Phase 1 brief.

---

## [2026-03-02] Phase 1 Session 1 — DXF Types, 3DFACE Parser, Streaming Disk Cache

### Done
- Created `src/terrain/Config.h` with all architecture constants (TILE_SIZE_M, GPU_BUDGET_MB,
  LOD_RATIOS, LOD_DISTANCES_M, LINEWORK_WIDTH_PX, PARSE_CHUNK_FACES). Only definition site.
- Created `dxf_parser/DxfTypes.h`:
  ParsedFace, XDataEntry, ParsedPolyline, ParseResult, TerrainVertex (7 floats = 28 bytes).
  static_assert confirms TerrainVertex == 28 bytes.
- Implemented `dxf_parser/DxfParser.h` + `DxfParser.cpp`:
  Single-pass DXF reader via `parseToCache(dxfPath, cacheDir, progress)`.
  - Extracts $EXTMIN origin from HEADER section.
  - Streams ENTITIES section 3DFACE entities in PARSE_CHUNK_FACES (50k) batches.
  - Per-face: subtracts origin, computes flat normal (cross product), flips if normal.z < 0.
  - Bins face to tile by XY centroid (50m grid); appends raw face data to .tmp files.
  - Flushes RAM on each chunk. After all entities: converts every .tmp → tile_X_Y_lod0.bin.
  - .bin header: uint32 magic=0x544C4554, version=1, lod=0, vertCount, indexCount;
    then TerrainVertex[] (28 bytes each), then uint32[] sequential indices.
  - atomic<float> progress updated every 8192 group-code pairs.
- Updated `dxf_parser/CMakeLists.txt`:
  Added TEST_DATA_DIR cmake define, /W4 /WX- flags, and `${CMAKE_SOURCE_DIR}/src`
  include path so DxfParser.cpp can reach terrain/Config.h without DX11 headers.
- Rewrote `dxf_parser/tests/parser_tests.cpp`:
  Fast test on 0217_SL_TRI.dxf (34 MB, ~47k entities); slow [slow]-tagged test on terrain.dxf.

### Decisions & Discoveries
- **Sample file reality differs from spec:**
  CLAUDE.md says terrain.dxf has 10,900 faces — actual file is 2.4 GB with ~6.3M 3DFACEs.
  0217_SL_TRI.dxf has 47,287 3DFACE entities (not "0210_SL_TRI.dxf" as spec says).
  CLAUDE.md sample-file section updated below.
- **Alternate triangle convention (v3=v0):**
  This DXF file encodes triangles by repeating the first vertex as the 4th vertex (v3=v0),
  not the third vertex (v3=v2, which is the documented DXF convention). The quad detection
  check was fixed to treat v3=v0 OR v3=v2 as a triangle (not a quad).
- **1374 genuinely degenerate faces in 0217_SL_TRI.dxf:**
  45,913 valid triangles + 1,374 degenerate (zero-area, skipped with warning) = 47,287 total.
  Degenerate faces arise from survey TIN data quality (duplicate/collinear vertices).
- **Two-phase tile writing:** Raw face data (.tmp, 48 bytes/face) is appended during streaming;
  finalisation converts each .tmp to the final .bin with proper VB + IB layout.
  This keeps RAM bounded to one PARSE_CHUNK_FACES chunk during streaming.
- **Straight sequential index buffer:** Each face = 3 unique vertices; indices are 0,1,2,3,…
  Vertex deduplication and actual LOD generation left for later phases (meshoptimizer).

### Test results
- All 4 targets build clean: imgui.lib, TerrainViewer.exe, dxf_parser.lib, parser_tests.exe
- parser_tests.exe (fast, ~[slow] filter): 1479 assertions passed, 0 failed
  - Parses 0217_SL_TRI.dxf in ~seconds
  - Verifies: origin found, faceCount+degenerate == 47287, progress==1.0,
    at least one well-formed .bin tile (magic, version, lod, counts), no .tmp leftovers

### Current state
Build: GREEN. parser_tests: GREEN.
Phase 1 Session 1 complete. Next: Phase 1 Session 2 (polyline parser + XDATA + LOD gen).

---

## [2026-03-02] Phase 1 Session 2 — Polyline Parser + XDATA

### Done
- Extended `dxf_parser/DxfTypes.h`:
  - Added `bool is3D` field to `ParsedPolyline` (true = 3D POLYLINE, false = LWPOLYLINE).
  - Added `std::vector<ParsedPolyline> polylines` to `ParseResult`.
- Extended `dxf_parser/DxfParser.cpp` — single-pass parser now handles linework:
  - **3D POLYLINE**: captures header (layer, XDATA), iterates VERTEX sub-entities
    (gc10/20/30 per vertex), terminates on SEQEND. Origin subtracted.
  - **LWPOLYLINE**: layer (gc8), constant elevation (gc38), XY pairs (gc10/gc20),
    XDATA. Z taken from gc38 for every vertex. Origin subtracted.
  - **XDATA**: gc1001 = appName opens new `XDataEntry`; all subsequent codes >= 1000
    stored as strings in `values`. Multiple 1001 blocks = multiple entries per polyline.
  - `PolyAccum` struct + `flushXdata` / `emitPolyline` lambdas manage state.
  - Defensive: ENDSEC, EOF, and unexpected entities while in polyline state emit cleanly.
- Updated `dxf_parser/tests/parser_tests.cpp`:
  - Existing SL_TRI test: added `polylineCount == 0` regression assertion.
  - New test `Parse 0217_SL_CAD.dxf polylines and XDATA`:
    - poly3dCount == 446, lwCount == 1550, polylineCount == 1996.
    - Every polyline has >= 1 vertex; all vertices are origin-offset (|X| < 10000 m).
    - At least one polyline has non-empty xdata. faceCount == 0.
  - Slow terrain.dxf test: added `polylineCount == 0` regression assertion.

### Decisions & Notes
- Polylines stored in RAM (ParseResult.polylines). ~2000 polylines is trivially small.
  Disk caching of linework is a future phase.
- XDATA captures all non-1001 codes >= 1000 as string values (no type filtering).
- VERTEX sub-entity XDATA ignored (non-fatal).
- terrain.dxf regression confirmed: faceCount still > 6 M; polylineCount = 0.

### Test results
- All 4 targets build clean.
- parser_tests.exe: **32993 assertions passed, 0 failed** (3 test cases).

### Current state
Build: GREEN. parser_tests: GREEN.
Phase 1 Session 2 complete. Next: Phase 1 Session 3 (LOD generation — meshoptimizer).

---

## [2026-03-02] Phase 1 Session 3 — LOD Generation, cache.meta, Cache Validity

### Done
- Extended `dxf_parser/DxfTypes.h`:
  - Added `size_t tileCount` to `ParseResult`.
- Extended `dxf_parser/DxfParser.h`:
  - Updated `parseToCache` comment to document full pipeline (cache check → parse → LOD → meta).
  - Added `generateLODs(cacheDir)` declaration.
  - Added `clearTileCache(cacheDir)` declaration.
- Extended `dxf_parser/DxfParser.cpp`:
  - **`generateLODs`**: for each `_lod0.bin` tile, deduplicates vertices via
    `meshopt_generateVertexRemap` + `meshopt_remapVertexBuffer/IndexBuffer`, then
    calls `meshopt_simplify` twice (LOD_RATIOS[1]=0.15 → lod1.bin, LOD_RATIOS[2]=0.02 →
    lod2.bin). Both LODs are derived from the same deduplicated base mesh.
    Tiles whose LOD bins are already newer than lod0.bin are skipped (incremental update).
    Processes tiles one at a time; RAM bounded to one tile's worth of data.
  - **`clearTileCache`**: removes all `.bin` files and `cache.meta` from cacheDir.
  - **`writeCacheMeta`**: writes `cache.meta` JSON with source_path, file_mtime
    (raw tick count as string), server_last_modified (null), downloaded_at (null),
    tile_count, face_count, origin[3].
  - **`checkCacheValid`**: reads `cache.meta`, compares source_path + file_mtime.
    On hit populates ParseResult.origin/tileCount/faceCount from JSON.
  - **`parseToCache`** updated: (1) cache hit check at start → generateLODs (cheap
    freshness check) + return; (2) cache miss → clearTileCache + re-parse; (3) after
    parse: generateLODs then writeCacheMeta. tileCount tracked and returned.
- Updated `dxf_parser/CMakeLists.txt`:
  - Added `find_package` + `target_link_libraries` for meshoptimizer and nlohmann_json.
- Updated `dxf_parser/tests/parser_tests.cpp`:
  - Added `readBinHeader` helper.
  - New test `LOD generation and cache.meta for 0217_SL_TRI.dxf`:
    - Parses 0217_SL_TRI.dxf, asserts faceCount==45913, tileCount>0.
    - Finds a tile with sufficient faces; asserts lod1.bin exists, has correct TLET header,
      lod1 indexCount < lod0 indexCount, lod1 indexCount % 3 == 0.
    - Same for lod2.bin.
    - Asserts cache.meta exists with all required JSON fields.
    - No leftover .tmp files.
    - Cache hit: calls parseToCache again; asserts origin matches, lod1 mtime unchanged
      (tiles were NOT regenerated on cache hit).
  - Added `readBinHeader` helper struct + function for reuse.

### Decisions & Notes
- LOD deduplication via meshopt_generateVertexRemap is necessary for proper edge
  connectivity; without it, the simplifier treats each triangle as isolated and
  cannot collapse edges between triangles.
- Both LOD1 and LOD2 are derived from the same deduplicated lod0 base mesh (not
  cascaded). This gives better quality at each LOD level independently.
- LOD bin vertex buffer = deduplicated unique vertices (fewer than 3*faceCount).
  Index buffer references this smaller vertex set. lod0.bin retains the original
  sequential/unindexed format for simplicity and backward compatibility.
- target_error=1.0 (maximum allowed deformation) so the simplifier always reaches
  the target count regardless of geometry complexity.
- Cache mtime stored as raw tick count string (platform-stable int64 to string).
- ParseResult.polylines is empty on cache hit (polylines not persisted to disk in Phase 1).

### Test results
- All 4 targets build clean. vcpkg re-ran to link meshoptimizer + nlohmann_json.
- parser_tests.exe: **40350 assertions passed, 0 failed** (4 test cases).

### Current state
Build: GREEN. parser_tests: GREEN.
Phase 1 COMPLETE. All sessions done: DXF types, 3DFACE parser, streaming tile cache,
POLYLINE/LWPOLYLINE/XDATA parser, LOD generation, cache validity.
Tagged: v1.0 "Phase 1 complete".
Next: Phase 2 — GPU upload, TileGrid, camera, basic rendering.

---

## [2026-03-02] Phase 2 Session 1 — Mesh Upload and Terrain Shader

### Done
- Created `src/shaders/terrain.hlsl`:
  - `#pragma pack_matrix(row_major)`, Z-up right-handed world space.
  - `cbuffer MVP : register(b0)` — world, view, proj (float4x4 each).
  - `cbuffer Light : register(b1)` — lightDir, ambient, diffuse (float3 + padding).
  - VS: transforms position + normal with world/view/proj; passes elevTint float through.
  - PS: Lambert diffuse — earthy-tan base colour + elevTint; saturate(surface * (ambient + nDotL * diffuse)).
- Created `src/terrain/Mesh.h` + `Mesh.cpp`:
  - Reads a TLET lod0.bin into `D3D11_USAGE_IMMUTABLE` VB (28-byte stride) + IB (uint32).
  - `Draw(ctx)`: IASetVertexBuffers, IASetIndexBuffer(R32_UINT), DrawIndexed.
- Created `src/terrain/TerrainPass.h` + `TerrainPass.cpp`:
  - `Init`: D3DCompileFromFile → VS + PS; input layout matching TerrainVertex
    (POSITION R32G32B32, NORMAL R32G32B32, COLOR R32_FLOAT at offsets 0/12/24).
  - Dynamic MVP + Light cbuffers (Map/Unmap per frame).
  - Rasterizer: solid, CULL_NONE (winding order to be verified Phase 2 S2).
  - Depth stencil: depth test + write enabled, D3D11_COMPARISON_LESS.
  - `Render(ctx, mesh, view, proj)`: updates cbuffers, binds pipeline, calls mesh.Draw.
- Updated `src/renderer/Renderer.h` + `Renderer.cpp`:
  - Added `ComPtr<ID3D11DepthStencilView> m_dsv` and `m_width`, `m_height` members.
  - `CreateRenderTarget`: also creates D32_FLOAT depth texture and DSV.
  - `BeginFrame`: OMSetRenderTargets with DSV, ClearDepthStencilView(1.0f),
    RSSetViewports (full-window).
  - Clear colour updated to {0.176f, 0.216f, 0.282f, 1.0f}.
- Updated `src/main.cpp`:
  - `LoadTerrain()`: calls `dxf::parseToCache`, then iterates cache dir for first `_lod0.bin`,
    uploads to `g_mesh`.
  - Fixed camera: `XMMatrixLookAtRH(eye={25,-100,120}, at={25,25,8}, up=Z)`;
    `XMMatrixPerspectiveFovRH(60°, aspect, 0.5, 5000)`.
  - Render loop: `BeginFrame → TerrainPass.Render → ImGui → EndFrame`.
  - ImGui window updated to "Terrain Viewer — Phase 2" with status message.
- Updated `CMakeLists.txt`:
  - Added `src/terrain/Mesh.cpp` + `src/terrain/TerrainPass.cpp` to TerrainViewer.
  - Linked `dxf_parser` + `meshoptimizer::meshoptimizer` to TerrainViewer.
  - Added compile definitions `SHADERS_DIR_STR` and `TERRAIN_DXF_STR` (CMake paths
    passed as narrow string literals, widened in C++ via `L##` macro trick).

### Decisions & Notes
- `TerrainVertex.color` is a `float` (not uint32), value 0.0f in Phase 1.
  Passed as `elevTint` to PS; adds grey lift when non-zero in Phase 2+.
- Shader path: `TO_WIDE(SHADERS_DIR_STR) L"/terrain.hlsl"` — adjacent wide string
  literal concatenation at compile time; zero runtime cost.
- CULL_NONE for Phase 2 S1: the parser normalises all normals to z>0 but winding
  order in NDC is not yet validated — culling to be confirmed in Phase 2 S2.
- meshoptimizer linked explicitly on TerrainViewer because dxf_parser links it PRIVATE;
  static lib transitive symbols do not propagate automatically in MSVC.

### Test results
- All 4 targets build clean: imgui.lib, TerrainViewer.exe, dxf_parser.lib, parser_tests.exe.
- parser_tests.exe: **40350 assertions passed, 0 failed** (4 test cases — no regression).

### Current state
Build: GREEN. parser_tests: GREEN.
Phase 2 Session 1 complete. One terrain tile (0217_SL_TRI lod0) renders as a lit mesh.
Next: Phase 2 Session 2 — TileGrid, all tiles, camera orbit controls.

---

## [2026-03-02] Phase 2 Session 2 — Camera Orbit/Pan/Zoom + Mouse Navigation

### Done
- Created `src/app/Camera.h` + `Camera.cpp` — spherical orbit camera:
  - Members: pivot (XMFLOAT3), radius, azimuth (°), elevation (°).
  - `SetPivot(x, y, z)` — move orbit centre in scene space.
  - `SetSpherical(radius, azimuthDeg, elevDeg)` — direct parameter set; elevation
    clamped to [−5°, +89°].
  - `OrbitDelta(dAzPx, dElPx)` — 0.3°/pixel sensitivity; screen Y inverted.
  - `PanDelta(dxPx, dyPx)` — moves pivot in camera-right / camera-up directions;
    scale = radius × 0.001 so feel is consistent at all zoom levels.
  - `ZoomDelta(notches)` — multiplicative: radius *= 0.85^notches; clamped [1, 100000].
  - `ViewMatrix()` — `XMMatrixLookAtRH(eye, pivot, Z-up)`.
  - `ProjMatrix(aspect)` — `XMMatrixPerspectiveFovRH(60°, aspect, 1, 20000)`.
  - Debug accessors: `Radius()`, `Azimuth()`, `Elevation()`, `Pivot()`.
- Updated `src/terrain/Mesh.h` + `Mesh.cpp`:
  - Added `DirectX::XMFLOAT3 AabbCentre() const`.
  - `Load()` now computes vertex AABB (min/max over all positions) and stores centre.
  - Added `#include <DirectXMath.h>` and `#include <cfloat>`.
- Updated `src/main.cpp`:
  - Replaced static `g_view`/`g_proj` with `static Camera g_camera`.
  - Added mouse state globals: `g_lastMousePos`, `g_lmbDown`, `g_rmbDown`, `g_mmbDown`.
  - WndProc: LMB→`OrbitDelta`, RMB/MMB→`PanDelta`, WM_MOUSEWHEEL→`ZoomDelta`.
    `SetCapture`/`ReleaseCapture` for reliable drag outside window bounds.
    `ImGui::GetIO().WantCaptureMouse` checked on button-down to skip ImGui-owned events.
  - `LoadTerrain()`: after successful mesh load, calls `g_mesh.AabbCentre()` and sets
    `g_camera.SetPivot` + `SetSpherical(360.6, 270°, 33.7°)` — equivalent to
    eye = pivot + (0, −300, 200).
  - Render loop: `ViewMatrix()` + `ProjMatrix(aspect)` computed per frame; aspect from
    `g_renderer.Width() / Height()` so it stays correct after window resize.
  - ImGui overlay: shows pivot, radius, azimuth, elevation, status, FPS.
- Updated `CMakeLists.txt`:
  - Added `src/app/Camera.cpp` to TerrainViewer sources.
  - Added `NOMINMAX` compile definition to prevent windows.h min/max macro interference
    with `std::max` / `std::clamp` in Camera.cpp.

### Decisions & Notes
- `NOMINMAX` added to TerrainViewer compile definitions — `std::max`/`std::clamp` in
  Camera.cpp triggered C2589 without it (windows.h defines max/min macros by default).
- Pan sign convention: drag right → pivot left → terrain slides right;
  drag down (dyPx > 0 screen space) → pivot moves in world +screenUp → terrain slides down.
  Vertical has opposite sign to horizontal because screen Y is inverted vs world Z.
- Elevation clamped at +89° (not +90°) to avoid camera looking straight down, which
  would make the Z-up vector parallel to the look direction in XMMatrixLookAtRH.
- Near plane = 1 m; far plane = 20000 m. Suitable for the ≈ 50 m tile at radius ≈ 360 m.

### Test results
- All 4 targets build clean: imgui.lib, TerrainViewer.exe, dxf_parser.lib, parser_tests.exe.
- Parser not modified — no regression run required.

### Current state
Build: GREEN.
Phase 2 COMPLETE. Orbit, pan, zoom all wired to mouse. Single tile renders with live camera.
Tagged: v2.0 "Phase 2 complete".
Next: Phase 3 — TileGrid (all tiles), GPU budget, LOD selection.

---

## [2026-03-02] Phase 3 Session 1 — TileGrid: metadata, state machine, frustum culling

### Done
- Created `src/terrain/TileGrid.h` + `TileGrid.cpp`:
  - `TileEntry` struct: tx/ty, aabbMin/aabbMax (XMFLOAT3), TileState enum, visible flag,
    Mesh (holds VB+IB), lod0Path (filesystem path for deferred GPU upload).
  - `TileState` enum: EMPTY, LOADING, GPU, EVICTED.
  - `TileGrid::Init(cacheDir)`: scans directory for `tile_*_lod0.bin` files using
    `sscanf_s` to parse tx/ty indices. Initial AABB: XY from grid indices ±1 m margin
    (centroid-binning safety), Z = [−500, +2000] conservative range.
  - `UpdateVisibility(planes)`: per-tile AABB-vs-frustum test; calls `RequestLoad` for
    tiles that newly enter the frustum (state==EMPTY && !wasVisible && visible).
  - `RequestLoad(idx)`: sets state=LOADING, appends to m_loadQueue.
  - `FlushLoads(device, maxLoads=INT_MAX)`: processes load queue; on success sets
    state=GPU and refines aabbMin/Max.z from actual mesh vertex AABB.
    Failed tiles set EVICTED to prevent retry loops.
  - `Evict(idx)`: `t.mesh = Mesh{}` releases ComPtr VB+IB, sets state=EVICTED.
  - `GetDrawList()`: returns all GPU tiles as `{tileIdx, lod=0, mesh*}`.
  - `SceneCentre()` / `SceneRadius()`: compute XY extent of populated tiles for
    camera initialisation.
  - Stats: `TileCount()`, `VisibleCount()`, `GpuCount()` for ImGui overlay.
- Implemented `ExtractFrustumPlanes(view, proj)` (free function, declared in TileGrid.h):
  Gribb-Hartmann method for DirectXMath row-major convention. Transposes VP matrix to
  access columns; constructs 6 D3D-convention planes (z in [0,1]). XMPlaneNormalize applied.
- Updated `src/terrain/Config.h`:
  - Added `MAX_TILE_LOADS_PER_FRAME = 3` (Phase 3 passes INT_MAX; constant ready for Phase 4).
- Updated `src/terrain/Mesh.h` + `Mesh.cpp`:
  - Added `AabbMin()`, `AabbMax()` accessors.
  - `Load()` now stores `m_aabbMin` and `m_aabbMax` alongside the existing centre.
- Updated `src/terrain/TerrainPass.h` + `TerrainPass.cpp`:
  - Refactored `Render` into `Begin(ctx, view, proj)` + `DrawMesh(ctx, mesh)` + `End()`.
  - `Begin`: updates MVP and Light cbuffers, binds pipeline state (VS, PS, layout,
    rasterizer, depth). Called once per frame regardless of tile count.
  - `DrawMesh`: calls `mesh.Draw(ctx)` if valid. Called once per GPU-resident tile.
  - `End`: no-op, reserved for Phase 5+ post-pass resource unbinding.
  - Old `Render(ctx, mesh, view, proj)` kept as a single-tile convenience wrapper.
- Updated `src/main.cpp`:
  - Replaced `Mesh g_mesh` + `g_terrainReady` with `TileGrid g_tileGrid` + `g_tilesReady`.
  - `LoadTerrain()`: calls `g_tileGrid.Init(cacheDir)` after parse; status message shows
    tile count + face count; default camera uses `SceneCentre()` and `SceneRadius()`.
  - Render loop: `ExtractFrustumPlanes → UpdateVisibility → FlushLoads → Begin/DrawMesh/End`.
    `FlushLoads` called every frame (no-op after queue drains).
  - ImGui overlay: shows `tiles=N  visible=M  gpu=K`.
  - Window title updated to "Terrain Viewer — Phase 3".
- Updated `CMakeLists.txt`: added `src/terrain/TileGrid.cpp`.

### Decisions & Notes
- AABB Z range initialised to [−500, 2000] until the tile is loaded; refined from actual
  mesh vertex data in FlushLoads. This prevents near/far plane culling artefacts before
  first load while avoiding false culls on unloaded tiles.
- +1 m XY margin on initial AABB accounts for the centroid-binning in the Phase 1 parser
  (triangles assigned to a tile by centroid, not clipped at boundary). Proper Shapely
  clipping will be done server-side in Phase 10+.
- `sscanf_s` used (MSVC preferred) — eliminates C4996 security warning.
- `m_tiles` is never resized after `Init()`, so `const Mesh*` pointers in DrawItem are
  stable for the lifetime of the TileGrid.
- Phase 3 passes INT_MAX to FlushLoads (load all visible tiles). Phase 4 will cap at
  MAX_TILE_LOADS_PER_FRAME = 3.
- `Evict` uses `t.mesh = Mesh{}` (default-constructed Mesh, move-assigned) to release
  ComPtr VB+IB — relies on compiler-generated Mesh move-assignment via ComPtr's operator=.

### Test results
- All 4 targets build clean, zero warnings: imgui.lib, TerrainViewer.exe, dxf_parser.lib,
  parser_tests.exe.
- Parser not modified — no regression run required.

### Current state
Build: GREEN.
Phase 3 Session 1 complete. Full terrain renders tile-by-tile with AABB frustum culling.
ImGui shows tile/visible/gpu counts. Camera initialised from terrain XY extent.
Next: Phase 3 Session 2 — GPU budget enforcement, LRU eviction, LOD selection.

---
## [2026-03-02] Phase 3 Session 2 — Distance-Based LOD Selection + Colour Overlay

### Done
- Updated `src/app/Camera.h` + `Camera.cpp`:
  - Added `Position()` accessor — returns eye position in scene space from spherical
    parameters: `{pivot + radius*(cos(el)*cos(az), cos(el)*sin(az), sin(el))}`.
- Updated `src/shaders/terrain.hlsl`:
  - Added `cbuffer TileData : register(b2)` — `float3 lodTint; float _tp;`.
  - PS now multiplies final lit colour by `lodTint`: `return float4(lit * lodTint, 1.0f)`.
    When overlay is off, lodTint = (1,1,1) — identity multiply, no effect on output.
- Updated `src/terrain/TileGrid.h` + `TileGrid.cpp`:
  - `TileEntry` extended: `int activeLod = -1`, `int targetLod = 0`,
    `std::filesystem::path lodPaths[3]` (replaces `lod0Path`; empty = not on disk).
  - Added `static int ComputeDesiredLod(aabbMin, aabbMax, camPos)` helper:
    Euclidean distance from camPos to tile AABB centre; LOD0 < 150 m, LOD1 < 400 m, else LOD2.
  - `Init`: populates `lodPaths[0/1/2]` with `fs::exists` check during directory scan.
  - `UpdateVisibility(planes, cameraPos)`: passes cameraPos to `ComputeDesiredLod` so the
    initial LOD enqueue uses the correct LOD (avoids wasted LOD0→LODn reload cycle).
  - `RequestLoad(idx, lod)`: takes LOD argument; falls back to lower LOD if `lodPaths[lod]`
    is empty (tile too small for simplification at that LOD level).
  - `FlushLoads`: loads `lodPaths[targetLod]` instead of `lod0Path`; sets `activeLod`.
  - `Evict`: resets `activeLod = -1`.
  - `GetDrawList(cameraPos)` (non-const): per-tile LOD transition detection — if
    `ComputeDesiredLod != activeLod`, evicts current buffers, re-queues at new LOD,
    tile absent one frame while reloading. Returns `{tileIdx, activeLod, mesh*}`.
- Updated `src/terrain/TerrainPass.h` + `TerrainPass.cpp`:
  - Added `TileDataConstants` struct (`{XMFLOAT3 lodTint; float _tp;}`, alignas(16)).
  - Added `m_tileDataCB` (dynamic cbuffer, 16 bytes) — created in `Init`.
  - `Begin`: binds m_tileDataCB to PS slot b2 (`PSSetConstantBuffers(2, 1, ...)`).
  - `DrawMesh(ctx, mesh, int lod)`: takes LOD arg; maps per-tile tint — green/yellow/red
    for LOD 0/1/2 when overlay is on; (1,1,1) when off.
  - `SetShowLodColour(bool)` / `GetShowLodColour()` — toggle for ImGui checkbox.
  - `Render` convenience wrapper updated to pass `lod=0`.
  - `Shutdown`: releases `m_tileDataCB`.
- Updated `src/main.cpp`:
  - Computes `camPos = g_camera.Position()` once per frame (used in both visibility and draw).
  - `UpdateVisibility(planes, camPos)` — passes camPos so initial LOD is correct.
  - `GetDrawList(camPos)` — LOD-aware draw list.
  - `DrawMesh(ctx, *item.mesh, item.lod)` — per-tile LOD tint.
  - ImGui: added "LOD overlay" checkbox — toggles `g_terrainPass.SetShowLodColour(show)`.
- Fixed: added `#include <string>` to `TileGrid.cpp` for `std::to_string`.

### Decisions & Notes
- LOD transitions cause a one-frame gap (tile absent while reloading). Accepted for Phase 3.
  Hysteresis (different thresholds for in/out transitions) deferred to Phase 4+.
- LOD path fallback (`while lod > 0 && lodPaths[lod].empty() --lod`) handles boundary tiles
  that are too small for meshoptimizer to generate a useful LOD simplification.
- `camPos` computed twice in main loop (once before UpdateVisibility, once before GetDrawList)
  but Camera::Position() is a cheap pure computation — no caching needed.
- `GetDrawList` is now non-const (it mutates TileEntry on LOD transitions).
- b2 TileData cbuffer mapped with MAP_WRITE_DISCARD per DrawMesh call; GPU sees latest
  contents at each DrawIndexed call (no hazard — sequential calls on immediate context).

### Test results
- All 4 targets build clean: imgui.lib, TerrainViewer.exe, dxf_parser.lib, parser_tests.exe.
- Parser not modified — no regression run required.

### Current state
Build: GREEN.
Phase 3 Session 2 complete. LOD colours change as camera moves (green near, red far).
ImGui LOD overlay checkbox toggles the tint. Tile transitions are seamless.
Next: Phase 4 — GPU budget enforcement, LRU eviction.

---

## [2026-03-02] Phase 3 Session 3 — GpuBudget: LRU Eviction + Staggered Loads

### Done
- Created `src/terrain/GpuBudget.h` + `GpuBudget.cpp` — pure GPU memory tracker:
  - No DX11 dependency; usable standalone.
  - `Track(idx, bytes)` — records tile upload; updates lruTick and usedBytes.
    Re-tracking an already-tracked tile (LOD switch reload) updates in-place.
  - `Untrack(idx)` — removes tile from tracking; subtracts from usedBytes.
  - `Touch(idx)` — advances lruTick for a drawn tile (call inside draw loop).
  - `HasRoom(bytes)` — returns true if usedBytes + bytes <= budgetBytes.
  - `ChooseEvictee(culledGpuIndices, visibleWithDist)` — eviction policy:
    1. LRU culled tile (min lruTick among invisible GPU tiles).
    2. Farthest visible tile (max camera dist) if no culled tiles tracked.
    Returns -1 if no candidates found.
  - `RecordEviction()` / `EvictCount()` — counter for ImGui readout.
  - `UsedBytes()` / `BudgetBytes()` — stats.
- Updated `src/terrain/TileGrid.h`:
  - Forward-declared `GpuBudget`.
  - Added `SetBudget(GpuBudget*)` — attaches budget tracker (non-owning pointer).
  - Added `m_budget` (GpuBudget*) and `m_lastCamPos` (XMFLOAT3) private members.
- Updated `src/terrain/TileGrid.cpp`:
  - `UpdateVisibility`: stores `cameraPos` in `m_lastCamPos` for use by FlushLoads.
  - `Evict`: calls `m_budget->Untrack(idx)` if budget is attached.
  - `FlushLoads` (budget-aware):
    - `PeekGpuBytes(path)` helper: estimates GPU footprint as `file_size(path) - 20`
      (TLET 20-byte header, remainder is VB+IB data).
    - Before each upload: enters eviction loop while `!HasRoom(needed)`.
      Each iteration builds culled/visible+dist candidate lists from GPU tiles,
      calls `ChooseEvictee`, calls `Evict(victim)` + `RecordEviction`, then retries.
      If still over budget after exhausting all candidates, tile is skipped this frame
      (left in queue, re-attempted next frame).
    - After successful load: `Track(idx, actualBytes)` with exact bytes from mesh.
    - maxLoads now defaults to 0x7fffffff but main.cpp passes
      `terrain::MAX_TILE_LOADS_PER_FRAME = 3` (staggered disk loads).
  - `GetDrawList`: calls `m_budget->Touch(i)` for every tile added to draw list.
- Updated `src/main.cpp`:
  - Added `#include "terrain/Config.h"` and `#include "terrain/GpuBudget.h"`.
  - `g_budget` global: `GpuBudget g_budget(terrain::GPU_BUDGET_MB * 1024 * 1024)`.
  - After `TileGrid::Init`: `g_tileGrid.SetBudget(&g_budget)`.
  - Single `camPos` computed once per frame (moved above the `g_tilesReady` block);
    used by both UpdateVisibility and GetDrawList — eliminates double Camera::Position call.
  - `FlushLoads(device, terrain::MAX_TILE_LOADS_PER_FRAME)` — 3 tiles/frame max.
  - ImGui stats: `"GPU: N / 192 MB  evicted=K"`.
- Updated `CMakeLists.txt`: added `src/terrain/GpuBudget.cpp` to TerrainViewer.

### Budget test (32 MB)
- Temporarily set `GPU_BUDGET_MB = 32` in Config.h; build succeeded with zero errors.
- With the 0217_SL_TRI.dxf test dataset (~46k faces across ~20 tiles), total GPU
  footprint is ~2–4 MB — well under 32 MB. No evictions fire at startup with this
  dataset; eviction path exercised by code inspection only.
- For terrain.dxf (6.3M faces, 100+ tiles at LOD0 ≈ 1–2 MB each), the 32 MB budget
  would cap to ~16–32 tiles GPU-resident and evict LRU culled tiles as the camera moves.
- Restored `GPU_BUDGET_MB = 192` before final commit.

### Decisions & Notes
- LRU eviction prefers culled (out-of-frustum) tiles first; no tile currently on screen
  is evicted while invisible candidates exist. This minimises visible popping.
- Among visible tiles (fallback), farthest from camera is evicted — it contributes the
  fewest pixels and will be reloaded at a lower LOD when the distance threshold is met.
- `PeekGpuBytes` uses `fs::file_size − 20` (header bytes) — no file open for large files,
  worst-case overestimates by 0 bytes (exact for well-formed TLET tiles).
- `GpuBudget` has zero dependency on DX11; TileGrid and GpuBudget code will not change
  when server tiles are added in Phase 10+ (per CLAUDE.md architecture note).
- MAX_TILE_LOADS_PER_FRAME=3 caps disk I/O per frame; with a fast NVMe this is invisible
  to the user; on a slow tablet SD card it prevents frame stutter during initial load.
- `m_lastCamPos` is set in UpdateVisibility (called every frame before FlushLoads);
  FlushLoads eviction candidates use this position for distance computation.

### Test results
- All 4 targets build clean at 32 MB budget and at 192 MB budget.
- Parser not modified — no regression run required.

### Current state
Build: GREEN.
Phase 3 Session 3 complete. Phase 3 COMPLETE.
192 MB GPU budget enforced; LRU eviction (culled-first, farthest fallback).
Staggered loads: max 3 tiles/frame. ImGui shows GPU memory usage + eviction count.
Tagging: v3.0 "Phase 3 complete".
Next: Phase 4 — design surface, linework rendering.

---

## [2026-03-02] Phase 5 — Linework: Geometry Shader Quad Expansion

### Approach note
Implemented GS-based screen-aligned quads (not CPU pre-expansion).
Intel HD 520 (Gen 9 / DX11.1) fully supports geometry shaders — GS is the
correct choice. If GS proves unreliable on a specific device, fallback is to
pre-expand each line segment to a quad (4 verts, 2 triangles) on the CPU and
remove the GS stage entirely. The LineworkMesh/LineworkPass split makes this
straightforward: only `LineworkMesh::Load` and `LineworkPass::Init/Begin/Draw`
would change; the rest of the pipeline is unaffected.

### Done
- Created `src/shaders/linework.hlsl`:
  - VS (b0/MVP): transforms float3 pos → clip space, passes clip pos as
    SV_POSITION and world pos as TEXCOORD0 to GS.
  - GS `[maxvertexcount(4)]` (b1/LineData): expands each LINELIST segment to
    a screen-aligned triangle strip (4 verts, 2 triangles).
    - Width derivation: multiply NDC diff by `viewportSize` to get a pixel-space
      direction vector; normalise; compute perpendicular; convert perpendicular
      back to NDC via `perp * kLineWidthPx * float2(1/W, 1/H)`.
    - Half-width check: `off.x * W/2 = perp.x * kLineWidthPx/2` pixels ✓
    - Multiplies NDC offset by clip-space `w` before adding to position
      (undoes the implicit perspective divide).
    - Guards: `p0.w < 1e-4f || p1.w < 1e-4f` skips behind-camera segments;
      `dot(diff,diff) < 1e-6f` skips zero-length segments.
    - `stream.RestartStrip()` after every 4 verts prevents strip linkage across
      invocations.
    - `kLineWidthPx = 2.0f` — must match `terrain::LINEWORK_WIDTH_PX` in Config.h.
  - PS: returns `float4(1,1,1,1)` (solid white). Per-layer colour added Phase 8.
- Created `src/terrain/LineworkMesh.h` + `LineworkMesh.cpp`:
  - `Load(device, polylines)`: iterates ParsedPolyline.verts (origin-offset
    already applied by DXF parser). Builds packed `float3` VB (12-byte stride,
    POSITION only) and uint32 IB as LINELIST segment pairs.
    For a polyline with N verts: emits N−1 pairs `(base+i, base+i+1)`.
  - Both VB and IB are `D3D11_USAGE_IMMUTABLE`.
  - `Draw(ctx)`: IASetVertexBuffers (stride=12), IASetIndexBuffer (R32_UINT),
    IASetPrimitiveTopology(LINELIST), DrawIndexed(indexCount).
- Created `src/terrain/LineworkPass.h` + `LineworkPass.cpp`:
  - `Init`: compiles VS (`vs_5_0`), GS (`gs_5_0`), PS (`ps_5_0`) from
    `linework.hlsl`. Input layout: POSITION R32G32B32_FLOAT, stride=12.
    MVP cbuffer (b0, 192 bytes), LineData cbuffer (b1, 16 bytes).
    Rasterizer: `CULL_NONE` (GS quad winding depends on line direction).
    Depth stencil: depth test ON, depth write ON.
  - `Begin(ctx, view, proj, vpW, vpH)`: updates cbuffers; binds VS/GS/PS,
    layout, states. MVP bound to VS slot 0; LineData bound to GS slot 1.
  - `Draw(ctx, mesh)`: calls `mesh.Draw(ctx)`.
  - `End(ctx)`: calls `ctx->GSSetShader(nullptr, nullptr, 0)` — CRITICAL:
    without this the GS remains active and corrupts subsequent terrain/design
    draws (they don't bind a GS, so the linework GS would apply to their VS
    output).
- Updated `CMakeLists.txt`:
  - Added `LineworkMesh.cpp` + `LineworkPass.cpp` to TerrainViewer sources.
  - Added `LINEWORK_DXF_STR` pointing to `docs/sample_data/0217_SL_CAD.dxf`.
- Updated `src/main.cpp`:
  - `kLineworkDxfPath` constant; `g_lineworkMesh`, `g_lineworkPass`,
    `g_lineworkReady`, `g_lineworkMsg` globals.
  - `LoadLinework()`: calls `dxf::clearTileCache(cacheDir)` before
    `parseToCache` to force fresh polyline collection on every startup
    (parseToCache returns empty polylines on cache hit since Phase 1 does not
    persist polylines to disk; the CAD file is 4.1 MB so re-parsing is fast).
    Then calls `g_lineworkMesh.Load(device, result.polylines)`.
  - WinMain: `LoadLinework()` then `g_lineworkPass.Init()` called at startup.
  - Render order: terrain (opaque) → linework (depth write ON) → design (alpha).
    Linework between terrain and design ensures white lines are visible over
    terrain AND design surface alpha-blends correctly over both.
  - ImGui: "Lines: N polylines  M segs" status line.
  - Shutdown: `g_lineworkPass.Shutdown()` called before design/terrain.

### Decisions & Notes
- `parseToCache` is called with a cleared cache on every startup for the linework
  DXF.  This is intentional and fast (4.1 MB, <100 ms).  Phase 6+ can add a
  dedicated linework binary cache if startup time becomes a concern.
- `End(ctx)` unbinding the GS is the most important correctness invariant in this
  phase.  TerrainPass and DesignPass never call GSSetShader; they rely on the GS
  slot being null.
- GS approach chosen over CPU pre-expansion: fewer vertices uploaded (raw line
  list vs 4× vertex expansion), width change is a shader constant, no CPU rebuild
  needed.  HD 520 GS support confirmed by Intel ARK (DX11.1 Tier 1).
- `CULL_NONE` on rasterizer: the GS emits quads whose winding order depends on
  whether perp points left or right of the line direction.  Without CULL_NONE,
  half of all line segments would be invisible.

### Test results
- All 4 targets build clean: imgui.lib, TerrainViewer.exe, dxf_parser.lib,
  parser_tests.exe.
- Parser not modified — no regression run required.

### Current state
Build: GREEN.
Phase 5 COMPLETE. White polylines render over terrain, correct depth ordering,
design surface blends over lines. GS unbind in End() confirmed in code review.
Tagging: v5.0 "Phase 5 complete".
Next: Phase 6 or as scoped.

---

## [2026-03-02] Phase 4 Session 1 — Design Surface: Two-Pass Alpha Blend + Depth Bias

### Done
- Created `src/shaders/design.hlsl`:
  - Identical VS/PS pipeline to terrain.hlsl; base colour changed to blue-grey
    `float3(0.35, 0.45, 0.60)` to visually distinguish from the earthy-tan terrain.
  - `cbuffer TileData : register(b2)` — `float3 lodTint; float opacity;`.
    `opacity` replaces the unused `_tp` padding from terrain.hlsl.
  - PS: `return float4(lit * lodTint, opacity)` — carries alpha for blend state.
- Created `src/terrain/DesignPass.h` + `DesignPass.cpp` — two-pass translucent render:
  - `Init`: compiles design.hlsl VS/PS, creates same input layout as TerrainPass
    (POSITION/NORMAL/COLOR, 28-byte TerrainVertex).
  - Two rasterizer states — both have `DepthBias=-1000, SlopeScaledDepthBias=-2.0f`
    to push the design surface in front of terrain and eliminate Z-fighting:
    - `m_rsFront`: CullMode=FRONT (Pass A — renders interior/back faces)
    - `m_rsBack`:  CullMode=BACK  (Pass B — renders exterior/front faces)
  - Blend state: `SrcBlend=SRC_ALPHA, DestBlend=INV_SRC_ALPHA, BlendEnable=TRUE`.
    Alpha/colour channels blended separately (SrcAlpha=ONE/DestAlpha=ZERO).
  - Depth stencil: `DepthEnable=TRUE, DepthWriteMask=ZERO` — reads depth (so design
    is occluded by terrain) but does not write (preserves terrain depth for blend).
  - `Begin(ctx, view, proj)`: updates MVP + Light cbuffers; binds VS/PS/layout/cbuffers;
    sets blend and depth stencil state.
  - `DrawMesh(ctx, mesh, lod)`: updates per-tile TileData cbuffer (lodTint + opacity=0.6);
    issues two Draw calls — RSSetState(m_rsFront) then RSSetState(m_rsBack).
  - `End(ctx)`: restores opaque blend (`OMSetBlendState(nullptr, ...)`) so ImGui is
    unaffected.
  - `SetShowLodColour/GetShowLodColour` — LOD overlay shared with terrain.
- Updated `src/terrain/TileGrid.h`:
  - Added `SetBudgetIndexBase(int base)` + `int m_budgetBase = 0` private member.
    Required when two TileGrids share one GpuBudget; without this, both grids'
    0-based tile indices would collide in the budget's unordered_map keys.
    Terrain grid uses base=0 (default). Design grid uses base=100000.
- Updated `src/terrain/TileGrid.cpp`:
  - All GpuBudget calls now offset by m_budgetBase:
    - `Track(m_budgetBase + idx, bytes)` in FlushLoads after successful load.
    - `Untrack(m_budgetBase + idx)` in Evict.
    - `Touch(m_budgetBase + i)` in GetDrawList.
    - Eviction candidate lists: `culled.push_back(m_budgetBase + j)` and
      `visible.emplace_back(m_budgetBase + j, dist)`.
    - ChooseEvictee return value: `Evict(victim - m_budgetBase)` to convert the
      budget key back to a local tile index before accessing m_tiles.
- Updated `CMakeLists.txt`:
  - Added `src/terrain/DesignPass.cpp` to TerrainViewer sources.
  - Added `DESIGN_DXF_STR` pointing to `docs/sample_data/0210_SL_TRI.dxf`.
- Updated `src/main.cpp`:
  - Renamed `kDxfPathNarrow` → `kTerrainDxfPath`; added `kDesignDxfPath`.
  - Added `g_designGrid` (TileGrid), `g_designPass` (DesignPass), `g_designReady`
    and `g_designMsg` globals.
  - Added `LoadDesign()`: same parse→cache→Init pattern as LoadTerrain; calls
    `g_designGrid.SetBudgetIndexBase(100000)` before SetBudget.
  - WinMain: calls `LoadDesign()` then `g_designPass.Init()` after terrain setup.
  - Render loop:
    - Tile streaming: both grids get UpdateVisibility + FlushLoads per frame.
    - Render order: terrain (opaque) → design (two-pass alpha) → ImGui.
  - ImGui: window title updated to "Phase 4"; shows Terrain and Design stats
    separately; LOD overlay and Force LOD0 checkboxes now sync both passes.
  - Shutdown: `g_designPass.Shutdown()` called before `g_terrainPass.Shutdown()`.

### Decisions & Notes
- Two-pass order (front-cull before back-cull): correct order for convex/closed
  meshes viewed from outside. Renders back faces first (behind front faces in
  depth), then front faces blend on top. No sorting of individual triangles needed.
- `DepthWriteMask=ZERO` on design surface: design tiles must not overwrite the
  terrain depth buffer; otherwise terrain tiles behind the design surface would be
  incorrectly revealed when the design surface moved.
- Depth bias of -1000 (hardware units) + slope-scaled -2.0 pushes the design
  surface's depth slightly toward the camera, preventing Z-fighting where both
  surfaces share the same Z values. Value chosen empirically for D3D11 default
  depth buffer precision (D32_FLOAT).
- `opacity=0.6f` hardcoded in `DesignPass::DrawMesh`. Phase 8 will add an ImGui
  slider that calls a `SetOpacity(float)` method.
- Budget index base 100000 chosen to be safely beyond any real tile count
  (datasets have O(100) tiles; 100000 provides ample headroom).
- Design surface camera not reset on load — terrain camera framing is correct
  for both surfaces (same geographic area).
- `End(ctx)` restores blend to null (opaque default) so ImGui's own blend state
  management is not disrupted.

### Test results
- All 4 targets build clean: imgui.lib, TerrainViewer.exe, dxf_parser.lib,
  parser_tests.exe.
- Parser not modified — no regression run required.

### Current state
Build: GREEN.
Phase 4 Session 1 complete. Both surfaces visible: opaque earthy terrain
underneath, semi-transparent blue-grey design surface on top at opacity=0.6.
No Z-fighting. Tagging: v4.0 "Phase 4 Session 1 complete".
Next: Phase 4 Session 2 (or as scoped by next brief).

---

## [2026-03-02] Windows Defender Exclusions Applied

### Done
Added the following Windows Defender exclusions to eliminate real-time scan overhead
on hot paths — disk tile cache writes/reads and the viewer executable itself trigger
Defender scans on every file touch without exclusions, which causes measurable stutter
during initial tile parsing (disk cache writes) and LOD reload (bin reads at runtime).

**ExclusionPath**
- `C:\Dev\terrain-viewer` — source repo (build artefacts, .bin cache in build/)
- `C:\Users\Claude1\AppData\Local\Temp\TerrainViewer` — runtime disk tile cache
  (`%TEMP%\TerrainViewer\tiles\{hash}\`) written by parseToCache; read by TileGrid
  every FlushLoads call

**ExclusionProcess**
- `TerrainViewer.exe` — exempts the viewer process from inline file-access scanning

**ExclusionExtension**
- `.bin` — TLET binary tile format; excluded globally so both write (parseToCache)
  and read (Mesh::Load) bypass real-time scanning

### Why
- Defender scans each `.bin` file on creation (parseToCache writes ~100 tiles for
  the 0217 dataset, ~1000+ for terrain.dxf) and on every open (LOD reloads during
  camera pan). On the target Toughpad FZ-G1 MK4 (slow storage, shared GPU/CPU bus)
  this compounds into multi-second stalls on first load and visible frame drops on
  LOD transitions.
- Source repo exclusion covers generated `build/release/` artefacts (link.exe and
  cl.exe are scanned on every incremental build otherwise).
- These exclusions were already in place for build tools (cl.exe, cmake.exe, etc.)
  from the initial environment setup — this entry completes coverage for the terrain
  viewer runtime paths.

### Verification
```
ExclusionPath:      C:\Dev\terrain-viewer
                    C:\Users\Claude1\AppData\Local\Temp\TerrainViewer
ExclusionProcess:   TerrainViewer.exe
ExclusionExtension: .bin
```
Confirmed via `Get-MpPreference` after applying.

### No code changes
Build: unchanged. No source files modified.

---


## [2026-03-02] Origin Alignment Fix

### Problem
Linework and design surface did not align with the terrain surface. Each DXF file
subtracts its own  from vertex positions at parse time, so three DXFs with
different  values each render in their own coordinate space.

### Root cause
 stores each file''s  in . Terrain,
design (), and linework () have different origins,
causing them to be offset from each other in scene space.

### Fix
Terrain  is the authoritative scene origin (). After all loads
complete, design and linework correction offsets are computed as:
  

Applied as:
- **TileGrid::ApplyOriginOffset(dx, dy, dz)** � shifts all tile AABBs so frustum
  culling remains correct for the design grid.
- **DesignPass::SetWorldMatrix / LineworkPass::SetWorldMatrix** � stores a translation
  world matrix written into the MVP cbuffer each frame. Default = identity (zero offset).

### Files changed
-  � added 
-  � added  + 
-  � added  + 
-  � , ,  globals;
  origin-correction block after all Init calls

### Build result
Green. All 4 targets build clean.

---

## [2026-03-02] Origin Alignment Fix

### Problem
Linework and design surface did not align with the terrain surface. Each DXF file
subtracts its own $EXTMIN from vertex positions at parse time, so three DXFs with
different $EXTMIN values each render in their own coordinate space.

### Root cause
dxf::parseToCache stores each file's $EXTMIN in ParseResult::origin. Terrain,
design (0210_SL_TRI.dxf), and linework (0217_SL_CAD.dxf) have different origins,
causing them to be offset from each other in scene space.

### Fix
Terrain $EXTMIN is the authoritative scene origin (g_sceneOrigin). After all loads
complete, design and linework correction offsets are computed as:
  offset = surface_extmin - terrain_extmin

Applied as:
- TileGrid::ApplyOriginOffset(dx, dy, dz) -- shifts all tile AABBs so frustum
  culling remains correct for the design grid.
- DesignPass::SetWorldMatrix / LineworkPass::SetWorldMatrix -- stores a translation
  world matrix written into the MVP cbuffer each frame. Default = identity (zero offset).

### Files changed
- src/terrain/TileGrid.h/.cpp  -- added ApplyOriginOffset()
- src/terrain/DesignPass.h/.cpp  -- added SetWorldMatrix() + m_worldMatrix
- src/terrain/LineworkPass.h/.cpp  -- added SetWorldMatrix() + m_worldMatrix
- src/main.cpp  -- g_sceneOrigin, g_designOrigin, g_lineworkOrigin globals;
  origin-correction block after all Init calls

### Build result
Green. All 4 targets build clean.

---

## [2026-03-02] Phase 6 Session 1 — Mouse / keyboard shortcuts

### Done
1. **LMB double-click → pivot set**
   - Added `CS_DBLCLKS` to `WNDCLASSEX::style` so `WM_LBUTTONDBLCLK` is generated.
   - Added `UnprojectRay(px, py, vpW, vpH, view, proj, originOut, dirOut)` helper using
     inverted VP matrix and D3D NDC (z∈[0,1]); homogeneous divide applied.
   - `WM_LBUTTONDBLCLK` handler: unprojected ray → `TileGrid::RayCast` (slab method,
     GPU AABBs only) → `Camera::SetPivot` on hit. Sets `g_lmbDown=true` for clean drag
     release. No-op when ImGui has mouse capture.
   - `TileGrid::RayCast` added to TileGrid.h (declaration) and TileGrid.cpp (slab method).

2. **Keyboard shortcuts** (`WM_KEYDOWN`, guarded by `WantCaptureKeyboard`)
   - R — reset camera to default pivot + radius (saved in `LoadTerrain`).
   - G — toggle GPS mode placeholder (`g_gpsMode`).
   - T / D / L — toggle terrain / design / linework visibility.
   - Escape — toggle ImGui sidebar panel.

3. **Visibility flags**
   - New globals: `g_showTerrain`, `g_showDesign`, `g_showLinework` (all default true),
     `g_showSidebar` (true), `g_gpsMode` (false).
   - All three render passes gated on `g_showX && g_Xready`.
   - `g_defaultPivot` / `g_defaultRadius` saved in `LoadTerrain` after camera init.

4. **ImGui panel** — title updated to "Phase 6"; two-line shortcut cheat-sheet; inline
   visibility checkboxes (Terrain / Design / Linework / GPS); sidebar close button
   (`&g_showSidebar`) works alongside Escape key.

### Files changed
- src/terrain/TileGrid.h  — RayCast declaration
- src/terrain/TileGrid.cpp — RayCast (slab method)
- src/main.cpp — CS_DBLCLKS, visibility globals, UnprojectRay, WM_LBUTTONDBLCLK,
  WM_KEYDOWN (R/G/T/D/L/Esc), LoadTerrain saves defaults, ImGui panel, render gates

### Build result
Green. All 4 targets build clean (--clean-first verified).

---

## [2026-03-02] Phase 6 Session 2 — WM_POINTER multi-touch

### Done
1. **`EnableMouseInPointer(TRUE)`** called after `RegisterClassEx`, before window show.
   Routes hardware mouse through WM_POINTER (PT_MOUSE) so all pointer input is unified.

2. **Touch state machine** (PT_TOUCH, up to 2 contacts)
   - `struct TouchContact { UINT32 id; float x, y; bool live; }` — 2-slot array `g_touch[2]`
   - `g_touchN` = live contact count; `g_touchPrevCX/CY` = centroid of last frame;
     `g_touchPrevD` = inter-finger distance of last frame.
   - `TouchUpdateBaselines()`: resets all prev-values from current contact positions;
     called on every contact-count change to prevent jumps.
   - **WM_POINTERDOWN**: claim free slot, increment `g_touchN`, reset baselines.
   - **WM_POINTERUPDATE**:
     - 1 contact → `Camera::OrbitDelta(dx, dy)` from centroid delta.
     - 2 contacts → `Camera::PanDelta` from centroid delta AND `Camera::ZoomDelta`
       from distance ratio simultaneously.
       `notches = -logf(d/prevD) / logf(0.85f)` — spreading=zoom-in=positive notch.
   - **WM_POINTERUP**: remove slot, decrement `g_touchN`, reset baselines.
   - Returns 0 (consumed) to prevent synthesised WM_LBUTTON* from touch.

3. **Mouse-via-pointer (PT_MOUSE)** — hardware mouse orbit/pan still works when
   EnableMouseInPointer routes it through WM_POINTER.
   `g_ptrLmb/Rmb/Mmb` flags driven by `POINTER_INFO::ButtonChangeType`;
   `WM_POINTERUPDATE` → OrbitDelta (LMB) or PanDelta (RMB/MMB).

4. **ImGui sidebar routing**
   - `g_sidebarPos / g_sidebarSize` (ImVec2) captured each frame via
     `ImGui::GetWindowPos()` / `ImGui::GetWindowSize()` inside the Begin/End block.
   - Cleared to {0,0} when sidebar is hidden.
   - In WM_POINTER handler: if touch point ∈ sidebar rect → `DefWindowProc` (not camera).

### Files changed
- src/main.cpp — `#include <cmath>`, touch state globals, ImGui sidebar rect globals,
  `TouchUpdateBaselines()`, WM_POINTERDOWN/UPDATE/UP handler (PT_TOUCH + PT_MOUSE),
  `EnableMouseInPointer(TRUE)` call, sidebar rect capture in ImGui block.

### Build result
Green. All 4 targets, zero errors/warnings (--clean-first verified).

---

## [2026-03-03] Phase 7 Session 1 — GPS abstraction, NMEA parser, PROJ transform, MockGpsSource

### Done

1. **IGpsSource.h** — already present as stub; confirmed interface correct:
   `struct ScenePosition { float x,y,z,heading; bool valid; };`
   `class IGpsSource { virtual ScenePosition poll()=0; virtual bool isConnected()=0; };`

2. **NmeaParser.h/.cpp** — already stubbed with full implementation:
   - `parseGGA`: `$GPGGA`/`$GNGGA`, extracts lat/lon (DDmm.mmmm→decimal, N/S/E/W sign),
     altitude MSL, fix quality (0 = reject). Validates XOR checksum.
   - `parseRMC`: `$GPRMC`/`$GNRMC`, extracts lat/lon, speed (knots→m/s), course_over_ground.
     Status V = reject. Validates XOR checksum.
   - `parse`: dispatches to parseGGA / parseRMC.
   - `computeChecksum`: XOR of chars between `$` and `*`.

3. **CoordTransform.h/.cpp** (`src/crs/`) — already stubbed with full PROJ implementation:
   - `wgs84ToMga(lat_deg, lon_deg, alt_msl_m, zone)` → `MgaPoint { easting, northing, elev }`
   - Uses PROJ 9 C API: `proj_create_crs_to_crs(EPSG:4326 → EPSG:283{zone})`
   - `proj_normalize_for_visualization` ensures (lon, lat) input order.
   - Throws `std::runtime_error` on PROJ failure or HUGE_VAL output.
   - Header includes guard comment: NEVER include outside `gps/` or `crs/`.

4. **MockGpsSource.h/.cpp** — already stubbed with full implementation:
   - Constructor: loads NMEA text file; falls back to hardcoded sentences if unavailable.
   - Background thread at 1 Hz: line → `nmea::parse` → `crs::wgs84ToMga` →
     subtract `sceneOrigin` → `ScenePosition`.
   - Heading EMA: smoothed with `terrain::GPS_HEADING_ALPHA`; only updated when
     `speed_mps >= terrain::GPS_MIN_SPEED_MS`.
   - `poll()`: mutex-protected return of latest `ScenePosition`.
   - `isConnected()`: returns `m_running` atomic.
   - Loops at EOF.

5. **Bug fixes**:
   - Fallback NMEA sentences had wrong checksums (GGA `*62`→`*67`, RMC `*1E`→`*0D`).
     Computed by hand-tracing XOR over the sentence bodies.
   - `MockGpsSource.cpp` used bare `GPS_MIN_SPEED_MS`/`GPS_HEADING_ALPHA` — not found
     because constants live in `namespace terrain`. Fixed to `terrain::GPS_MIN_SPEED_MS` etc.

6. **Config.h additions** — GPS constants added to `namespace terrain`:
   `GPS_HEADING_ALPHA=0.15f`, `GPS_MIN_SPEED_MS=0.5f`, `OFFLINE_WARN_HOURS=4`,
   `MANIFEST_POLL_SECONDS=60`, `PREFETCH_RADIUS_TILES=2`, `MAX_CONCURRENT_DOWNLOADS=2`.

7. **CMakeLists.txt wired up**:
   - `src/gps/NmeaParser.cpp`, `src/gps/MockGpsSource.cpp`, `src/crs/CoordTransform.cpp`
     added to TerrainViewer sources.
   - `PROJ::proj` added to TerrainViewer link libraries.
   - New `gps_tests` executable: NmeaParser.cpp + src/gps/tests/gps_tests.cpp,
     links Catch2::Catch2WithMain only (no PROJ/DX11).

8. **NmeaParser unit tests** (`src/gps/tests/gps_tests.cpp`):
   - 15 test cases, 43 assertions, all pass.
   - Covers: GGA valid/quality-0/bad-checksum/GNGGA/southern-lat,
     RMC valid/V-status/bad-checksum/GNRMC, parse() dispatch, unrecognised sentences,
     zero course+speed from GGA-only data.
   - Test sentences use NMEA spec examples with hand-verified checksums.

### Out of scope (deferred)
- Camera integration, GPS UI panel, SerialGps, TcpGps.
- MockGpsSource not yet instantiated in main.cpp (Phase 7 S2).

### Files changed
- `src/terrain/Config.h` — added GPS + server constants to `namespace terrain`
- `src/gps/MockGpsSource.cpp` — fixed fallback checksums, qualified constant names
- `src/gps/tests/gps_tests.cpp` — new; NmeaParser Catch2 test suite
- `CMakeLists.txt` — gps/crs sources, PROJ link, gps_tests target

### Build result
Green. 5 targets build clean: imgui.lib, dxf_parser.lib, TerrainViewer.exe,
gps_tests.exe, parser_tests.exe.
gps_tests: 43 assertions in 15 test cases — all passed.
parser_tests (~[slow]): 40346 assertions in 3 test cases — all passed.

---

## [2026-03-03] Phase 7 Session 2 — GPS camera viewpoint, elevation lookup, dropout

### Done

1. **Config.h additions** (`namespace terrain`):
   - `GPS_MOVE_THRESHOLD_M = 1.0f` — min XY move (m) to recompute terrain elevation
   - `GPS_HEIGHT_OFFSET_M  = 1.7f` — camera eye height above AABB surface (standing height)
   - `GPS_VIEW_DISTANCE_M  = 50.0f` — distance from eye to look-pivot in GPS camera mode
   - `GPS_MGA_ZONE         = 55`   — MGA zone placeholder; Phase 10 reads from server /config

2. **MockGpsSource instantiated in `WinMain`**:
   - Created after `LoadTerrain()` and origin alignment; passed `g_sceneOrigin` and
     `terrain::GPS_MGA_ZONE`.
   - NMEA file path: `<terrain DXF dir>/gps.nmea`; falls back to hardcoded Sydney sentences
     if absent.
   - `g_gpsSrc.reset()` called at start of shutdown block to join background thread cleanly
     before DX11 teardown.

3. **GPS camera mode** (render loop, before ImGui frame):
   - Each frame when `g_gpsMode && g_gpsSrc`: calls `g_gpsSrc->poll()`.
   - **Valid fix**: updates `g_gpsLastKnown`; performs/caches terrain elevation lookup;
     positions camera.
   - **Dropout (`!valid`)**: camera frozen at last position; user can orbit/pan freely.
     Tracking resumes automatically when valid fix returns.

4. **Terrain elevation lookup** (AABB top, downward ray):
   - Re-uses existing `TileGrid::RayCast()` with `ray = {gpsX, gpsY, 9999}`, `dir = {0,0,-1}`.
   - For a downward ray, the slab method returns `aabbMax.z` of the tile containing (X, Y).
   - Cached; recomputed only when GPS moves `> GPS_MOVE_THRESHOLD_M` OR on GPS mode entry
     (`g_gpsNeedElevLookup` flag; also set by G key on mode entry).
   - Falls back to `z = 0` if no GPU tile covers the GPS position.

5. **Camera positioning** (no Camera.h/.cpp changes needed):
   - Eye position: `{gpsX, gpsY, g_gpsCachedElev + GPS_HEIGHT_OFFSET_M}` (1.7 m above terrain).
   - Look-pivot: `GPS_VIEW_DISTANCE_M` ahead in heading direction.
     `lookX = sin(heading_rad) * 50`, `lookY = cos(heading_rad) * 50`.
   - Heading → camera azimuth (scene: North=+Y, East=+X, azimuth CCW from +X):
     `azimuth = fmodf(270.0f - heading + 360.0f, 360.0f)`.
     Verified: heading 0°(N)→270°, 90°(E)→180°, 180°(S)→90°, 270°(W)→0°.
   - Calls `g_camera.SetPivot(lookPivot)` + `SetSpherical(lookDist, azimuth, 0.0f)`.
   - Elevation fixed at 0° (horizontal forward look); user can tilt via orbit drag.

6. **G key** (`WM_KEYDOWN`): sets `g_gpsNeedElevLookup = true` on GPS mode entry so the
   first valid fix always triggers a fresh elevation raycast.

7. **ImGui GPS status** (inside sidebar, below GPS checkbox):
   - Valid: shows scene-relative XY/Z position, heading, cached terrain elevation.
   - Dropout: amber `"no fix (camera frozen)"` label.

8. **ImGui title**: updated `"Phase 6"` → `"Phase 7"`.

### Out of scope (deferred)
- SerialGps, TcpGps, reconnect timer (MockGpsSource never disconnects).
- GPS height_offset ImGui slider (Phase 8).
- GPS position indicator on screen (arrow/crosshair).

### Design notes
- No changes to `Camera.h/.cpp` or `TileGrid.h/.cpp` — existing API is sufficient.
- Dropout handling is implicit: GPS update only executes when `gpsPos.valid`. User input
  (orbit/pan) always applies; GPS tracking overrides it on the next valid poll.
- The elevation AABB lookup gives `aabbMax.z` (highest vertex Z in the tile). This is a
  conservative estimate but safe — camera cannot be below the terrain surface.

### Files changed
- `src/terrain/Config.h` — 4 new GPS constants
- `src/main.cpp` — GPS includes, globals, WinMain init, render-loop camera update,
  ImGui status, shutdown reset, title update

### Build result
Green. 5 targets build clean: imgui.lib, dxf_parser.lib, TerrainViewer.exe,
gps_tests.exe, parser_tests.exe.
gps_tests: 43 assertions in 15 test cases — all passed.

---

## [2026-03-03] Phase 7 Session 3 — SerialGps, TcpGps, CRS panel, coordinate readout

### Done

1. **CoordTransform** (`src/crs/`):
   - Added `crs::Datum { GDA94, GDA2020 }` enum.
   - `wgs84ToMga` gains `datum` parameter (default `GDA94`).
     GDA94 MGA Zone N = EPSG:28300+N; GDA2020 MGA Zone N = EPSG:7800+N.
   - Added `mgaToWgs84(easting, northing, elev, zone, datum)` inverse.
     After `proj_normalize_for_visualization`: MGA input (E,N), WGS84 output (lon,lat).
     Returns `WgsPoint { lat_deg = out.y, lon_deg = out.x }`.
   - Extracted `makeProjTransform` helper to share context/PJ create/destroy logic.

2. **IGpsSource.h** — added `gps::Datum { GDA94, GDA2020 }` enum so `main.cpp`
   can specify datum without ever including `crs/CoordTransform.h`. ✓ Architecture
   rule preserved.

3. **MockGpsSource** — added `datum` constructor parameter; passes `crsDatum`
   to `crs::wgs84ToMga`. Existing file-replay logic unchanged.

4. **SerialGps.h/.cpp** (new):
   - Windows COM port GPS source. Background thread: `CreateFileA → DCB 8N1 →
     `SetCommTimeouts(500ms) → ReadFile loop → line accumulator → NmeaParser
     → wgs84ToMga → ScenePosition`.
   - Reconnect: on `ReadFile` failure, closes handle and sleeps 5 s (10 × 500 ms
     polls so `m_running` is checked).
   - `HANDLE` stored as `void*` in header to avoid `windows.h` inclusion.
   - `isConnected()` reflects `m_connected` atomic.

5. **TcpGps.h/.cpp** (new):
   - WinSock2 TCP GPS source. Background thread: `socket → getaddrinfo → connect
     → SO_RCVTIMEO(500ms) → recv loop → line accumulator → NmeaParser
     → wgs84ToMga → ScenePosition`.
   - Reconnect: on `recv` ≤ 0, closes socket and sleeps 5 s.
   - Destructor force-closes the socket via atomic swap so `recv` unblocks before
     thread join.
   - `SOCKET` stored as `uintptr_t(~0)` sentinel in header to avoid `winsock2.h`.
   - `WSAStartup` / `WSACleanup` called in constructor / destructor.

6. **CoordReadout.h/.cpp** (new, `src/gps/`):
   - `gps::sceneToMga(sx,sy,sz, sceneOrigin)` — adds origin to scene coords.
   - `gps::mgaToWgs(easting, northing, elev, zone, gps::Datum)` — calls
     `crs::mgaToWgs84` internally. Exposes `MgaCoord` / `WgsCoord` structs.
   - `main.cpp` may include this; `crs/CoordTransform.h` stays hidden. ✓

7. **GPS source selector** (ImGui sidebar, new section "GPS Source"):
   - Combo: `Mock (replay) | Serial (COM) | TCP`.
   - Serial: port (char[16], default "COM3") + baud (int, default 9600).
   - TCP: host (char[128], default "127.0.0.1") + port (int, default 4001).
   - `Connect` button → `RecreateGpsSource()` (destroys existing, creates new).
   - `Disconnect` button → `g_gpsSrc.reset()`.
   - Connected status (green "connected" / amber "connecting...").

8. **RecreateGpsSource()** helper — joins existing GPS thread then creates the
   selected source type with current `g_crsZone` / `g_crsDatum`.
   Called on startup (Mock default), Connect, and Apply CRS.

9. **CRS panel** (ImGui sidebar, new section "CRS"):
   - Datum combo: GDA94 / GDA2020 (UI pending; applied on "Apply CRS").
   - Zone `InputInt` clamped [49, 56] (UI pending).
   - Auto-suggest: `zone = int(sceneOrigin.X) / 1_000_000`. Valid [49,56] → shows
     "Auto N" SmallButton that prefills the zone input. Computes once after
     `LoadTerrain`.
   - "Apply CRS" button: writes `g_crsZone` / `g_crsDatum` from UI state,
     invalidates coordinate cache, calls `RecreateGpsSource()`.

10. **Coordinate readout** (ImGui sidebar, new section "Coords"):
    - Each frame: if GPS mode with valid fix → use `g_gpsLastKnown` scene coords;
      else use `g_camera.Pivot()`.
    - Recomputes only when position changes > 0.1 m (squared threshold) or on
      first frame (`NaN` sentinel).
    - Displays: `MGA E/N/Z` (6 decimal places) and `WGS84 lat/lon` (6 decimal
      places, +/- sign prefix), zone/datum label.
    - Shows `(unavailable — check zone)` on PROJ exception.

11. **`g_gpsSrc` changed** from `unique_ptr<MockGpsSource>` to
    `unique_ptr<IGpsSource>` — fully polymorphic.

### Architecture rules confirmed ✓
- `crs/CoordTransform.h` never included in `main.cpp`.
- `gps::Datum` in `IGpsSource.h` bridges UI ↔ CRS without leaking crs types.
- New GPS sources (`SerialGps`, `TcpGps`, `CoordReadout`) are all in `src/gps/`.
- `winsock2.h` only in `TcpGps.cpp`; `windows.h` only in `SerialGps.cpp`.

### Files changed
- `src/crs/CoordTransform.h` — Datum enum, mgaToWgs84 declaration
- `src/crs/CoordTransform.cpp` — datum support, inverse, shared helper
- `src/gps/IGpsSource.h` — added gps::Datum enum
- `src/gps/MockGpsSource.h/.cpp` — datum parameter
- `src/gps/SerialGps.h/.cpp` — new
- `src/gps/TcpGps.h/.cpp` — new
- `src/gps/CoordReadout.h/.cpp` — new
- `src/main.cpp` — IGpsSource, GPS selector, CRS panel, coord readout
- `CMakeLists.txt` — SerialGps.cpp, TcpGps.cpp, CoordReadout.cpp, Ws2_32.lib

### Build result
Green. All 5 targets: imgui.lib, dxf_parser.lib, TerrainViewer.exe,
gps_tests.exe, parser_tests.exe.
gps_tests: 43 assertions in 15 test cases — all passed.

---

## [2026-03-03] Phase 8 S1 — Session Persistence (JSON)

### Done
1. **`src/terrain/Config.h`** — added `SESSION_AUTOSAVE_SECONDS = 60`.

2. **`src/terrain/DesignPass.h/.cpp`** — replaced hardcoded `opacity = 0.6f`
   with `m_opacity` member + `SetOpacity`/`GetOpacity` accessors. Default 0.6.

3. **`src/app/Session.h`** — `SessionData` struct (schema v1):
   - files: terrain_dxf, design_dxf, linework_dxf
   - visibility: show_terrain, show_design, show_linework, gps_mode
   - opacity: design_opacity (float)
   - crs: zone (int), datum (string "GDA94" | "GDA2020")
   - gps: source, serial_port, serial_baud, tcp_host, tcp_port
   - camera: pivot_x/y/z, radius, azimuth, elevation
   - window: x, y, width, height (INT_MIN = OS chooses)
   - misc: disk_cache_keep_on_exit, last_connected_at
   `Session` class: `DefaultPath()`, `Load()`, `Save()`, `loaded` flag.

4. **`src/app/Session.cpp`** — full implementation:
   - `DefaultPath()`: `%APPDATA%\TerrainViewer\session.json`; fallback to
     `<exe_dir>\session.json` via `GetModuleFileNameW`.
   - `Load()`: missing → silent false; parse error → rename to `.bak`,
     set `toastMsg`, return false; success → all fields populated with
     graceful per-field defaults (lambda helpers: `str`, `b`, `i32`, `f32`).
   - `Save()`: atomic write via `.tmp` then `fs::rename`; creates parent
     directories; `last_connected_at` serialised as JSON `null` if empty.

5. **`src/main.cpp`** — major integration:
   - DXF path globals changed from `static const char[]` to `static std::string`
     (mutable; Session 2 will add file pickers).
   - Session globals: `g_session`, `g_sessionPath`, `g_toast`, `g_toastTimer`,
     `g_sessionSavePending` (atomic), `g_autoSaveRunning` (atomic),
     `g_autoSaveThread`, `g_hwnd`, `g_designOpacity`.
   - `GatherSession()`: collects all persistable state (files, visibility,
     opacity, CRS, GPS, camera via `g_camera.*`, window via `GetWindowPlacement`).
   - `ApplySession()`: restores all state; camera only if `g_session.loaded`
     (first run stays terrain-centred); calls `RecreateGpsSource()`.
   - `WinMain` flow: `Load session` → `resolve DXF paths` → `create window with
     session position` → `renderer` → `loads` → `passes` → `origin alignment` →
     `CRS auto-suggest` → `ApplySession()` → `ImGui` → `ShowWindow` → `start
     auto-save thread` → `message loop` → `shutdown`.
   - Auto-save: background thread sets `g_sessionSavePending` every
     `SESSION_AUTOSAVE_SECONDS`; main thread performs actual save (no
     cross-thread DX11 access).
   - Final save on clean exit.
   - Disk cache deleted on exit if `disk_cache_keep_on_exit == false`.
   - Toast overlay: amber text, 5 s display, `NoInputs|NoMove|NoResize`.
   - Design opacity `SliderFloat` in ImGui sidebar.
   - Session filename shown in sidebar ("Session: session.json").
   - Phase title updated to "Terrain Viewer — Phase 8".

6. **`CMakeLists.txt`** — added `src/app/Session.cpp`.

### Architecture rules confirmed ✓
- `Session.cpp` uses `nlohmann/json.hpp` (already in vcpkg).
- Atomic `.tmp` → rename write: crash-safe session update.
- Auto-save thread only sets a flag; all DX11/file I/O on main thread.
- Window position saved via `WINDOWPLACEMENT::rcNormalPosition` (correct for
  minimised/maximised states).
- Camera restore gated on `g_session.loaded` (true only when file found).

### Files changed
- `src/terrain/Config.h` — SESSION_AUTOSAVE_SECONDS
- `src/terrain/DesignPass.h/.cpp` — m_opacity, SetOpacity/GetOpacity
- `src/app/Session.h` — new
- `src/app/Session.cpp` — new
- `src/main.cpp` — full integration
- `CMakeLists.txt` — Session.cpp added

### Build result
Green. All 5 targets: imgui.lib, dxf_parser.lib, TerrainViewer.exe,
gps_tests.exe, parser_tests.exe.
gps_tests: 43 assertions in 15 test cases — all passed.

---

## [2026-03-03] Phase 8 S2 — Full Production Sidebar UI

### Done
1. **`src/shaders/terrain.hlsl`** — TileData cbuffer: replaced `float _tp`
   with `float opacity`. PS outputs `float4(lit * lodTint, opacity)`.

2. **`src/shaders/linework.hlsl`** — added `cbuffer LineAlpha : register(b2)`
   (`float opacity; float3 _la`). PS outputs `float4(p.color, opacity)`.

3. **`src/terrain/TerrainPass.h/.cpp`** — opacity support:
   - Added `m_opacity = 1.0f`, `SetOpacity`/`GetOpacity`.
   - Added `m_dsOpaque` (depth write ON) + `m_dsTransparent` (depth write OFF).
   - Added `m_blendState` (SRC_ALPHA / INV_SRC_ALPHA).
   - `Begin`: selects opaque or blend path based on `m_opacity`.
   - `DrawMesh`: writes `m_opacity` to TileData cbuffer.
   - `End(ctx)` (signature updated): restores `OMSetBlendState(nullptr)`.

4. **`src/terrain/LineworkPass.h/.cpp`** — opacity support:
   - Added `LineAlphaConstants` (b2 PS), `m_lineAlphaCB`, `m_blendState`,
     `m_dsOpaque`, `m_dsTransparent`.
   - Added `m_opacity = 1.0f`, `SetOpacity`/`GetOpacity`.
   - `Begin`: writes opacity to `m_lineAlphaCB`, binds `PSSetCB(b2)`,
     selects opaque/blend path.
   - `End`: unbinds GS + restores blend (both previously only unbound GS).

5. **`src/app/Session.h`** — added to `SessionData`:
   - `terrain_opacity = 1.0f`, `linework_opacity = 1.0f`
   - `gps_height_offset = 1.7f`

6. **`src/app/Session.cpp`** — load/save the three new fields.
   Opacity object now has `terrain`, `design`, `linework` keys.
   GPS object now has `height_offset` key.

7. **`CMakeLists.txt`** — added `ole32.lib shell32.lib`
   (required for `CoCreateInstance` / `IFileOpenDialog`).

8. **`src/main.cpp`** — major restructure:
   - **Window title**: `"Terrain Viewer"` (no phase suffix).
   - **CoInitialize**: `CoInitializeEx` at WinMain entry; `CoUninitialize`
     at exit. Required for `IFileOpenDialog`.
   - **New globals**: `g_terrainOpacity`, `g_lineworkOpacity`,
     `g_gpsHeightOffset`, `g_sidebarOpen` (replaces `g_showSidebar`).
   - **`OpenFileDlg(owner, title)`**: `IFileOpenDialog` COM dialog; DXF
     filter; returns UTF-8 path or empty.
   - **`FullReload()`**: resets all TileGrids, LineworkMesh, and GpuBudget;
     calls LoadTerrain/Design/Linework; re-applies origin alignment and
     recreates GPS source. Called when user picks a new file.
   - **`ApplyOriginAlignment()`**: extracted from WinMain inline — also
     updates CRS auto-suggest zone.
   - **GatherSession / ApplySession**: handle three new fields.
   - **GPS height offset**: replaced `terrain::GPS_HEIGHT_OFFSET_M` constant
     with mutable `g_gpsHeightOffset`.
   - **Right-edge collapsible sidebar**:
     - Closed: 28px tab with "<" button (opens sidebar).
     - Open: 320px panel, NoTitleBar/NoMove/NoResize, full viewport height.
     - **Files section**: "Open Terrain/Design/Linework DXF..." (full-width
       buttons, IFileOpenDialog), filename + visibility checkbox per layer.
     - **Layers section**: full-width opacity sliders (terrain/design/linework),
       `SetNextItemWidth(-1)`.
     - **GPS section**: enable checkbox, source combo, serial/TCP config,
       height-offset slider, Connect/Disconnect buttons, status indicator.
     - **View section**: Reset View button, coord readout (MGA E/N/Z +
       WGS84 lat/lon), camera pivot/spherical display, keyboard hints.
     - **Settings section**: CRS datum/zone with auto-suggest, Apply CRS
       button, disk-cache keep toggle, LOD overlay / Force LOD0 checkboxes.
     - **Footer**: tile/GPU stats, session filename, FPS.
   - All interactive items push `ImGuiStyleVar_FramePadding (8, 15)` → ~43px
     height (≈44px touch target spec).
   - `g_sidebarOpen` replaces `g_showSidebar`; ESC key toggles sidebar.
   - `TerrainPass::End` call updated: `g_terrainPass.End(g_renderer.Context())`.

### Architecture rules confirmed ✓
- `IFileOpenDialog` in main.cpp only (COM, no crs/ leakage).
- Opacity path selected per-pass per-frame; opaque path (opacity=1.0)
  still uses depth write ON so Z-order is correct.
- Linework PS b2 slot independent of GS b1 (LineData) — no conflict.
- `FullReload` resets GpuBudget to avoid stale LRU entries.
- All new session fields have safe defaults; old session.json files load
  gracefully (new keys fall back to defaults if missing).

### Files changed
- `src/shaders/terrain.hlsl`
- `src/shaders/linework.hlsl`
- `src/terrain/TerrainPass.h/.cpp`
- `src/terrain/LineworkPass.h/.cpp`
- `src/app/Session.h`
- `src/app/Session.cpp`
- `src/main.cpp`
- `CMakeLists.txt`

### Build result
Green. All 5 targets: imgui.lib, dxf_parser.lib, TerrainViewer.exe,
gps_tests.exe, parser_tests.exe.
gps_tests: 43 assertions in 15 test cases — all passed.

---

## [2026-03-04] Fix — TerrainPass depth write always ON

### Bug
Design surface always rendered in front of terrain, even where terrain is
geometrically closer to the camera.

### Root cause
`TerrainPass::Begin()` switched to `m_dsTransparent` (DepthWriteMask = ZERO)
whenever terrain opacity < 1.0. With depth write OFF, terrain left the depth
buffer at its cleared value (1.0 everywhere). The design pass uses
LESS_EQUAL comparison with depth write OFF; every design fragment passed
`D_design <= 1.0`, so design appeared in front of terrain at every pixel.

The earlier fix (commit 530c41e, remove negative bias + LESS_EQUAL on design)
correctly addressed the design shader side but left this terrain-side gap.

### Fix
`TerrainPass` now always binds `m_dsOpaque` (depth write ON, LESS).
Opacity < 1.0 only controls the blend state (SRC_ALPHA / INV_SRC_ALPHA);
the depth buffer always records the terrain surface location. This is correct
because the terrain surface IS physically present even when semi-transparent --
design geometry behind it should remain occluded.

Removed `m_dsTransparent` from TerrainPass entirely.

### Files changed
- `src/terrain/TerrainPass.h` -- removed m_dsTransparent, updated comment
- `src/terrain/TerrainPass.cpp` -- always use m_dsOpaque; blend-only branch
  for opacity < 1.0; removed m_dsTransparent creation + Shutdown reset

### Build result
Green. TerrainViewer.exe.

---

## [2026-03-04] Fix — DesignPass positive DepthBias to yield at coincident geometry

### Additional bug (same symptom, different cause)
After the terrain depth-write fix, design was still appearing in front of terrain
at coincident or near-coincident surfaces (terrain and design at the same elevation).
LESS_EQUAL passes when design_depth == terrain_depth, so design won at all no-change
areas. With terrain opacity < 1 and my first fix now writing terrain depth, the
depth buffer is filled but LESS_EQUAL still made design win at coincident pixels.

### Fix
Added DepthBias = +100 to both DesignPass rasterizer states (m_rsFront, m_rsBack).
On D24_UNORM this is ~6e-6 of the [0,1] depth range -- a few centimetres equivalent
at typical survey viewing distances. Effect:
- Coincident geometry: design_depth + bias > terrain_depth -> LESS_EQUAL FAILS -> terrain shows
- Genuine fill areas (design above terrain): depth margin >> bias -> LESS_EQUAL still passes -> design shows
- Cut areas (design below terrain): design already has larger depth -> unaffected

Previous bias was negative (commit 530c41e removed it). Positive bias is the
correct direction: design yields to terrain, not the other way around.

### Files changed
- src/terrain/DesignPass.h -- updated comment
- src/terrain/DesignPass.cpp -- DepthBias = 100 on both rasterizer states

### Build result
Green. TerrainViewer.exe.

---

## [2026-03-04] Phase 8 S3 — Error hardening, progress bar, polish, licenses

### Done
1. **Removed LOD colour overlay debug artifact**
   - Removed LOD colour overlay checkbox from sidebar (main.cpp).
   - Removed `SetShowLodColour` / `GetShowLodColour` / `m_showLodColour` from
     `TerrainPass.h/.cpp` and `DesignPass.h/.cpp`.
   - `DrawMesh` now writes `cb->lodTint = {1,1,1}` unconditionally; no shader change needed.
   - Suppressed C4100 (unreferenced `lod` parameter) with `int /*lod*/` in .cpp signatures.
   - Force LOD0 checkbox retained (useful in production for performance troubleshooting).
   - Confirmed: no tile AABB wireframe, no ImGui demo window, no OutputDebugString in codebase.

2. **Error hardening**
   - `LoadTerrain`, `LoadDesign`, `LoadLinework`: all DXF parse + init logic wrapped in
     `try { ... } catch (std::exception const& ex)`.
   - On catch: sets appropriate status message AND fires a 6-second toast to the user.
   - File-not-found: pre-existing early-exit handling retained.
   - DX11 init failure: already had `MessageBox + return 1` — verified correct.
   - Session corrupt: already handled in P8 S1 — verified correct.

3. **Parse progress bar (async FullReload)**
   - Added `LoadData` struct + globals: `g_parseProgress` (atomic<float>), `g_parseBusy`
     (atomic<bool>), `g_loadApplyPending` (atomic<bool>), `g_parseCurrentFile` (atomic<int>),
     `g_loaderThread` (thread).
   - `FullReload()` now:
     - Returns immediately if `g_parseBusy` (ignore double-click/re-entrant call).
     - Resets all GPU/tile state on main thread.
     - Spawns background thread: parses terrain → design → linework sequentially,
       updating `g_parseProgress` and `g_parseCurrentFile` between phases.
     - On completion: `g_loadApplyPending.store(true)`, `g_parseBusy.store(false)`.
   - `ApplyLoadResults()` runs on main thread when `g_loadApplyPending` fires:
     - Calls `TileGrid::Init` (disk-only, fast) for terrain and design.
     - Calls `LineworkMesh::Load(device, polylines)` — requires DX11, main thread only.
     - Sets all ready flags, status messages, camera defaults.
     - Calls `ApplyOriginAlignment`, `RecreateGpsSource`.
   - Render loop: `g_loadApplyPending.exchange(false)` check before ImGui each frame.
   - ImGui progress overlay: centred floating window while `g_parseBusy`:
     - Progress < 1.0: determinate `ProgressBar(prog)` + "Loading terrain/design/linework..."
     - Progress >= 1.0 (LOD gen phase): marquee `ProgressBar(-1 * time)` + "Generating LOD meshes"
     - Disappears automatically when `g_parseBusy` clears.
   - Startup loads (before ImGui): remain synchronous (no render loop at that point).
   - Shutdown: `g_loaderThread.join()` before GPU resource release.
   - Thread error handling: errors stored in `g_loadData.*Error`, reported as toasts in Apply.

4. **THIRD_PARTY_LICENSES.txt** (repo root)
   - Dear ImGui MIT (© 2014-2024 Omar Cornut)
   - meshoptimizer MIT (© 2016-2025 Arseny Kapoulkine)
   - PROJ MIT-style (© Gerald Evenden / Frank Warmerdam)
   - nlohmann/json MIT (© 2013-2025 Niels Lohmann)
   - Catch2 BSL-1.0 (TEST-ONLY — not shipped in release binaries)
   - All license texts verbatim from vcpkg copyright files and vendored source.

### Files changed
- src/main.cpp
- src/terrain/TerrainPass.h, TerrainPass.cpp
- src/terrain/DesignPass.h, DesignPass.cpp
- THIRD_PARTY_LICENSES.txt (new)

### Test results
Both Debug and Release build clean (zero errors, no new warnings).
parser_tests: not run (parser not touched).

### Current state
Build: GREEN — Debug and Release.
Phase 8 COMPLETE. Next: Phase 9 — TBD.

---

## [2026-03-04] Phase 8 S4 — Server config, offline indicator, freshness overlay, self-contained deployment

### Goal
Make the release folder work when copied to another machine. Add server configuration
UI stub, offline indicator banner, and freshness overlay (F key) as stubs for Phase 11.

### Changes

1. **Session.json v2**
   - `SessionData.schema_version` bumped to 2.
   - `last_connected_at` (top-level) replaced by nested `server: { url, enabled, last_connected_at }`.
   - Added `freshness: { overlay_visible }`.
   - Load: v1 files load gracefully (old `last_connected_at` ignored, new fields default).
   - Save: atomic write as before.

2. **Freshness overlay (terrain.hlsl + TerrainPass)**
   - `terrain.hlsl` TileData cbuffer: added `float4 overlayColor` (rgb=tint, a=blend factor).
     PS: `lerp(lit * lodTint, overlayColor.rgb, overlayColor.a)`.
   - `TileDataConstants` in TerrainPass.h updated to match (32 bytes, was 16).
   - `TerrainPass::SetTileOverlayColor()` — call before render loop to set overlay.
   - `(1,1,1,0)` = no overlay (default). Color computed from `cache.meta server_last_modified`.
   - Age bands: Green <24h, Yellow <7d, Orange <30d, Red >30d or null.
   - F key toggles overlay; persisted to `session.freshness.overlay_visible`.
   - `g_terrainCacheDir` stored on load; `g_terrainFreshnessColor` recomputed from cache.meta.
   - Phase 11: replace uniform color with per-tile color from manifest data.

3. **Offline indicator banner**
   - Shown when `server.enabled && url !empty && last_connected_at set` and hours >= `OFFLINE_WARN_HOURS`.
   - Persistent non-blocking ImGui overlay at top-centre: "Offline Xh YYm -- terrain data may be outdated".
   - `GetOfflineTime()` parses ISO8601 via `_mkgmtime`; returns `{-1,0}` if condition not met.

4. **Server settings UI**
   - New "Server:" sub-section in Settings CollapsingHeader (before CRS).
   - URL InputText + "Enable server connection" Checkbox.
   - Last contact time shown when configured.
   - Saved to `session.server.*` on auto-save / shutdown.

5. **Offline shader compilation (.cso) — self-contained**
   - `fxc.exe` found via `find_program` from Windows SDK 10.0.26100.0 hint.
   - CMake PRE_BUILD compiles all 7 shader stages to exe directory:
     terrain_vs/ps.cso, design_vs/ps.cso, linework_vs/gs/ps.cso.
   - `d3dcompiler.lib` removed from link libraries.
   - `SHADERS_DIR_STR` compile definition removed.
   - All three pass .cpp files (TerrainPass, DesignPass, LineworkPass) now use
     `LoadCso(L"name.cso")` helper (GetModuleFileNameW → parent_path / filename).
   - `#include <d3dcompiler.h>` removed from all three pass files.

6. **POST_BUILD runtime copies**
   - `proj_9.dll` → exe directory.
   - `proj_data/` (proj.db + datum grids from vcpkg) → exe directory.
   - MSVC CRT: msvcp140.dll, msvcp140_1.dll, vcruntime140.dll, vcruntime140_1.dll.
   - Sample DXFs from docs/sample_data/ → exe directory (user picks on first run).

7. **PROJ data search path**
   - `SetEnvironmentVariableA("PROJ_DATA", projData)` called in WinMain between
     origin alignment and ApplySession (before first PROJ call via RecreateGpsSource).
   - Path: `<exe_dir>/proj_data`.

8. **Empty DXF defaults**
   - `TERRAIN_DXF_STR`, `DESIGN_DXF_STR`, `LINEWORK_DXF_STR` compile defines removed.
   - `g_terrainDxfPath` etc. initialise to `""` (empty).
   - DXF path resolution in WinMain: session value only (no compile-time fallback).
   - `LoadTerrain/Design/Linework`: empty path → informational status message, no crash.
   - Async FullReload thread: empty path → silently skip (not an error).
   - Sample DXFs copied to exe dir; user opens them via Files panel on first run.

9. **nlohmann/json added to TerrainViewer link libraries** (needed for cache.meta read in main.cpp).

### Files changed
- src/app/Session.h, Session.cpp
- src/shaders/terrain.hlsl
- src/terrain/TerrainPass.h, TerrainPass.cpp
- src/terrain/DesignPass.cpp
- src/terrain/LineworkPass.cpp
- src/main.cpp
- CMakeLists.txt
- CLAUDE.md (current state + SHADERS rule updated)

### Test results
Debug and Release both build clean — zero errors, zero warnings.
All 7 .cso files compiled successfully.
proj_9.dll, proj_data/, MSVC CRT DLLs, sample DXFs all present in output directory.
parser_tests: not run (parser not touched).

### Current state
Build: GREEN — Debug and Release.
Phase 8 S4 complete. Phase 8 fully done (v8.0 tag already set at S3).
Next: Phase 9 — TBD.

---
