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
// Flow:
//   1. Check cache.meta: if source_path + file_mtime match, skip re-parse and
//      return cached result (generateLODs still runs to fill any missing LODs).
//   2. Otherwise: delete stale tiles, stream-parse DXF → lod0.bin tiles.
//   3. Call generateLODs(cacheDir) to produce lod1.bin + lod2.bin for each tile.
//   4. Write cache.meta sidecar with source metadata and origin.
//
// dxfPath:  path to the source .dxf file
// cacheDir: directory that receives / already holds tile_X_Y_lodN.bin files
// progress: written atomically in [0.0, 1.0] as DXF parsing advances
//
// Returns a ParseResult with origin, counts, and any warnings.
// On cache hit: polylines is empty (not stored on disk in Phase 1).
ParseResult parseToCache(const std::filesystem::path& dxfPath,
                         const std::filesystem::path& cacheDir,
                         std::atomic<float>&           progress);

// Generate LOD1 and LOD2 tile bins from existing LOD0 bins in cacheDir.
// Each tile is processed independently; tiles whose LOD bins are already
// newer than the LOD0 bin are skipped (incremental update).
// Uses meshoptimizer for vertex deduplication and mesh simplification.
// LOD ratios are read from terrain::LOD_RATIOS (defined in terrain/Config.h).
void generateLODs(const std::filesystem::path& cacheDir);

// Delete all .bin tile files and cache.meta from cacheDir.
// Call on clean application exit when disk_cache_keep_on_exit == false.
void clearTileCache(const std::filesystem::path& cacheDir);

} // namespace dxf
