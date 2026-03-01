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
