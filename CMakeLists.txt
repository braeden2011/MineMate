cmake_minimum_required(VERSION 3.20)
project(TerrainViewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- vcpkg deps ----
find_package(meshoptimizer CONFIG REQUIRED)
find_package(PROJ CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Catch2 CONFIG REQUIRED)

# ---- Dear ImGui (vendored) ----
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_compile_definitions(imgui PUBLIC UNICODE _UNICODE)

# ---- dxf_parser static lib ----
add_subdirectory(dxf_parser)

# ---- TerrainViewer executable ----
add_executable(TerrainViewer WIN32
    src/main.cpp
    src/app/Camera.cpp
    src/app/Session.cpp
    src/crs/CoordTransform.cpp
    src/gps/CoordReadout.cpp
    src/gps/MockGpsSource.cpp
    src/gps/NmeaParser.cpp
    src/gps/SerialGps.cpp
    src/gps/TcpGps.cpp
    src/renderer/Renderer.cpp
    src/terrain/DesignPass.cpp
    src/terrain/GpuBudget.cpp
    src/terrain/LineworkMesh.cpp
    src/terrain/LineworkPass.cpp
    src/terrain/Mesh.cpp
    src/terrain/TerrainPass.cpp
    src/terrain/TileGrid.cpp
)

target_include_directories(TerrainViewer PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(TerrainViewer PRIVATE
    imgui
    dxf_parser
    meshoptimizer::meshoptimizer
    PROJ::proj
    nlohmann_json::nlohmann_json
    d3d11.lib
    dxgi.lib
    dxguid.lib
    Ws2_32.lib
    ole32.lib
    shell32.lib
)

target_compile_definitions(TerrainViewer PRIVATE
    UNICODE _UNICODE
    NOMINMAX                 # prevent windows.h from defining min/max macros
    # Shaders compiled to .cso at build time (fxc PRE_BUILD below) — no SHADERS_DIR_STR.
    # DXF paths are empty by default; user picks files via the Files sidebar panel.
)

target_compile_options(TerrainViewer PRIVATE /W4 /WX-)

# ---- Offline shader compilation (fxc.exe → .cso into output dir) -----------
find_program(FXC_EXE fxc.exe
    HINTS
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.19041.0/x64"
    DOC "HLSL shader compiler (fxc.exe) from Windows SDK"
)

if(NOT FXC_EXE)
    message(WARNING "fxc.exe not found — HLSL shaders will not be compiled. "
                    "Specify FXC_EXE manually if it is in a non-standard location.")
else()
    set(SHADERS_SRC "${CMAKE_SOURCE_DIR}/src/shaders")
    add_custom_command(TARGET TerrainViewer PRE_BUILD
        COMMAND "${FXC_EXE}" /nologo /E VS /T vs_5_0
            /Fo "$<TARGET_FILE_DIR:TerrainViewer>/terrain_vs.cso"
            "${SHADERS_SRC}/terrain.hlsl"
        COMMAND "${FXC_EXE}" /nologo /E PS /T ps_5_0
            /Fo "$<TARGET_FILE_DIR:TerrainViewer>/terrain_ps.cso"
            "${SHADERS_SRC}/terrain.hlsl"
        COMMAND "${FXC_EXE}" /nologo /E VS /T vs_5_0
            /Fo "$<TARGET_FILE_DIR:TerrainViewer>/design_vs.cso"
            "${SHADERS_SRC}/design.hlsl"
        COMMAND "${FXC_EXE}" /nologo /E PS /T ps_5_0
            /Fo "$<TARGET_FILE_DIR:TerrainViewer>/design_ps.cso"
            "${SHADERS_SRC}/design.hlsl"
        COMMAND "${FXC_EXE}" /nologo /E VS /T vs_5_0
            /Fo "$<TARGET_FILE_DIR:TerrainViewer>/linework_vs.cso"
            "${SHADERS_SRC}/linework.hlsl"
        COMMAND "${FXC_EXE}" /nologo /E GS /T gs_5_0
            /Fo "$<TARGET_FILE_DIR:TerrainViewer>/linework_gs.cso"
            "${SHADERS_SRC}/linework.hlsl"
        COMMAND "${FXC_EXE}" /nologo /E PS /T ps_5_0
            /Fo "$<TARGET_FILE_DIR:TerrainViewer>/linework_ps.cso"
            "${SHADERS_SRC}/linework.hlsl"
        COMMENT "Compiling HLSL shaders to .cso"
        VERBATIM
    )
endif()

# ---- POST_BUILD: copy runtime dependencies into the output directory --------

# PROJ shared library
add_custom_command(TARGET TerrainViewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "C:/vcpkg/packages/proj_x64-windows/bin/proj_9.dll"
        "$<TARGET_FILE_DIR:TerrainViewer>/proj_9.dll"
    COMMENT "Copying proj_9.dll"
)

# PROJ data directory (proj.db + datum grids)
add_custom_command(TARGET TerrainViewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "C:/vcpkg/packages/proj_x64-windows/share/proj"
        "$<TARGET_FILE_DIR:TerrainViewer>/proj_data"
    COMMENT "Copying PROJ data"
)

# MSVC runtime DLLs (redistributable CRT)
set(MSVC_CRT_DIR
    "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT")
foreach(dll
        msvcp140.dll
        msvcp140_1.dll
        vcruntime140.dll
        vcruntime140_1.dll)
    add_custom_command(TARGET TerrainViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MSVC_CRT_DIR}/${dll}"
            "$<TARGET_FILE_DIR:TerrainViewer>/${dll}"
    )
endforeach()

# Sample DXF files (user can open from exe directory on first run)
foreach(dxf
        terrain.dxf
        0210_SL_TRI.dxf
        0217_SL_TRI.dxf
        0217_SL_CAD.dxf)
    if(EXISTS "${CMAKE_SOURCE_DIR}/docs/sample_data/${dxf}")
        add_custom_command(TARGET TerrainViewer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/docs/sample_data/${dxf}"
                "$<TARGET_FILE_DIR:TerrainViewer>/${dxf}"
        )
    endif()
endforeach()

# ---- GPS unit tests (NmeaParser only — no PROJ/DX11 dependency) ----
add_executable(gps_tests
    src/gps/NmeaParser.cpp
    src/gps/tests/gps_tests.cpp
)

target_include_directories(gps_tests PRIVATE src)

target_link_libraries(gps_tests PRIVATE
    Catch2::Catch2WithMain
)

target_compile_definitions(gps_tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(gps_tests PRIVATE /W4 /WX-)
