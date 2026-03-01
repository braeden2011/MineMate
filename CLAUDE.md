# CLAUDE.md — Terrain Viewer Bootstrap
# READ THIS FILE at the start of every session.

## What this project is
Windows C++17 / DirectX 11 terrain surface viewer.
Target scale: 5 million 3DFACE triangles per surface.
Spec: docs/scope_v0.4.docx | Dev process: docs/dev_guide_v1.0.docx

## Current state
Phase: 0 — Complete
Last completed: P0 Scaffold — DX11 window + ImGui rendering
Next task: Phase 1 — DXF Parser (see Dev Guide Section 5, Phase 1 brief)
Known issues: None

## Build
Use PowerShell (cmake.exe lives inside the VS 2022 installation):

    $cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    & $cmake -B build\release -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
    & $cmake --build build\release --config Release
    .\build\release\Release\TerrainViewer.exe

Or run the helper script:  .\build_configure.bat  then  .\build_all.bat

## Architecture rules — NEVER violate
- Index buffers: uint32. Never uint16.
- GPU vertex positions: always origin-offset. Never raw MGA coordinates.
- dxf_parser/ has ZERO DX11 header dependency.
- GPS behind IGpsSource interface only.
- Tile size and GPU budget: constants in src/terrain/Config.h only.
- All DX11 resources: ComPtr<T> RAII. No raw COM ownership.

## Key constants (define in src/terrain/Config.h)
TILE_SIZE_M=100.0f GPU_BUDGET_MB=256 LOD_RATIOS={1.0,0.15,0.02}
LOD_DISTANCES_M={200.0,600.0} LINEWORK_WIDTH_PX=2.0

## vcpkg deps: meshoptimizer proj nlohmann-json catch2

## Sample files
docs/sample_data/terrain.dxf        10,900 3DFACE triangles
docs/sample_data/0210_SL_TRI.dxf    47,762 3DFACE triangles
docs/sample_data/0217_SL_CAD.dxf    446 POLYLINE + 1,550 LWPOLYLINE

## NEVER
- Change uint32 to uint16 index buffers
- Raw MGA coords in GPU buffers
- DX11 headers in dxf_parser/
- Magic numbers (use Config.h)
- Leave a broken build
- Touch scope_v0.4.docx
