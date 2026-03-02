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
