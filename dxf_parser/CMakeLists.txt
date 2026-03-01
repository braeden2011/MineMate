cmake_minimum_required(VERSION 3.20)
project(dxf_parser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Static library (placeholder — no sources yet) ----
add_library(dxf_parser STATIC
    DxfParser.cpp
)
target_include_directories(dxf_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ---- Unit tests ----
find_package(Catch2 CONFIG REQUIRED)

add_executable(parser_tests
    tests/parser_tests.cpp
)
target_link_libraries(parser_tests PRIVATE
    dxf_parser
    Catch2::Catch2WithMain
)
