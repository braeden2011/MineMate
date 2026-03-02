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
    src/renderer/Renderer.cpp
    src/terrain/GpuBudget.cpp
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
    d3d11.lib
    d3dcompiler.lib
    dxgi.lib
    dxguid.lib
)

target_compile_definitions(TerrainViewer PRIVATE
    UNICODE _UNICODE
    NOMINMAX                 # prevent windows.h from defining min/max macros
    SHADERS_DIR_STR="${CMAKE_SOURCE_DIR}/src/shaders"
    TERRAIN_DXF_STR="${CMAKE_SOURCE_DIR}/docs/sample_data/0217_SL_TRI.dxf"
)

target_compile_options(TerrainViewer PRIVATE /W4 /WX-)
