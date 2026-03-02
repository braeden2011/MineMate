#pragma once
// src/terrain/Config.h
// Global constants — the ONLY place these values are defined.
// No magic numbers anywhere else in the codebase.

namespace terrain {

constexpr float TILE_SIZE_M       = 50.0f;
constexpr int   GPU_BUDGET_MB     = 512;
constexpr float LOD_RATIOS[]      = { 1.0f, 0.15f, 0.02f };
constexpr float LOD_DISTANCES_M[] = { 150.0f, 400.0f };
constexpr float LINEWORK_WIDTH_PX = 2.0f;
constexpr int   PARSE_CHUNK_FACES        = 50000;
constexpr int   MAX_TILE_LOADS_PER_FRAME = 100;

// GPS smoothing / filtering
constexpr float GPS_HEADING_ALPHA  = 0.15f;  // EMA weight for heading smoothing
constexpr float GPS_MIN_SPEED_MS   = 0.5f;   // below this speed, heading not updated
constexpr int   OFFLINE_WARN_HOURS = 4;
constexpr int   MANIFEST_POLL_SECONDS    = 60;
constexpr int   PREFETCH_RADIUS_TILES    = 2;
constexpr int   MAX_CONCURRENT_DOWNLOADS = 2;

} // namespace terrain
