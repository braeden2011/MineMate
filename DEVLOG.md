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
