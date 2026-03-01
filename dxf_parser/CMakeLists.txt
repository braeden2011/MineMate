cmake_minimum_required(VERSION 3.20)
project(dxf_parser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Static library ----
add_library(dxf_parser STATIC
    DxfParser.cpp
)

target_include_directories(dxf_parser PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}          # DxfTypes.h, DxfParser.h
)

# DxfParser.cpp includes terrain/Config.h — no DX11 headers in Config.h,
# so this doesn't violate the "no DX11 in dxf_parser" rule.
target_include_directories(dxf_parser PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_options(dxf_parser PRIVATE /W4 /WX-)

# ---- Unit tests ----
find_package(Catch2 CONFIG REQUIRED)

add_executable(parser_tests
    tests/parser_tests.cpp
)

target_link_libraries(parser_tests PRIVATE
    dxf_parser
    Catch2::Catch2WithMain
)

# Expose sample data directory so tests can find DXF files.
target_compile_definitions(parser_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/docs/sample_data"
)

target_compile_options(parser_tests PRIVATE /W4 /WX-)
