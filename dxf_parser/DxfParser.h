#pragma once
// dxf_parser/DxfParser.h
// DXF parsing interface.
// NO DX11 HEADERS — this library must compile standalone.

#include "DxfTypes.h"

#include <atomic>
#include <filesystem>

namespace dxf {

// Parse a DXF file into a streaming tile cache on disk.
//
// dxfPath:  path to the source .dxf file
// cacheDir: directory that will receive tile_X_Y_lod0.bin files
// progress: written atomically in [0.0, 1.0] as parsing advances
//
// Returns a ParseResult containing origin, face/polyline counts, and warnings.
// Only LOD0 tile files are written; LOD1/LOD2 generation is a later phase.
ParseResult parseToCache(const std::filesystem::path& dxfPath,
                         const std::filesystem::path& cacheDir,
                         std::atomic<float>&           progress);

} // namespace dxf
