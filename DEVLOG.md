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
