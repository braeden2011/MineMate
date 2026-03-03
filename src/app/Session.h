#pragma once
// src/app/Session.h
// Persistent session state.  Serialised to/from session.json via Session::Load / Session::Save.
//
// Schema version 1.  All fields have safe defaults so partial/old files load gracefully.
// File paths: empty string means "use compile-time default DXF".
// Window position: INT_MIN means "let the OS choose" (first run or off-screen recovery).

#include <climits>
#include <filesystem>
#include <string>

namespace app {

struct SessionData {
    int schema_version = 1;

    // ── Files ─────────────────────────────────────────────────────────────
    std::string terrain_dxf;   // empty → use TERRAIN_DXF_STR compile-time default
    std::string design_dxf;    // empty → use DESIGN_DXF_STR
    std::string linework_dxf;  // empty → use LINEWORK_DXF_STR

    // ── Visibility ────────────────────────────────────────────────────────
    bool show_terrain  = true;
    bool show_design   = true;
    bool show_linework = true;
    bool gps_mode      = false;

    // ── Opacity ───────────────────────────────────────────────────────────
    float terrain_opacity  = 1.0f;
    float design_opacity   = 0.6f;
    float linework_opacity = 1.0f;

    // ── CRS ───────────────────────────────────────────────────────────────
    int         crs_zone  = 55;
    std::string crs_datum = "GDA94";   // "GDA94" | "GDA2020"

    // ── GPS ───────────────────────────────────────────────────────────────
    std::string gps_source       = "mock";   // "mock" | "serial" | "tcp"
    std::string serial_port      = "COM3";
    int         serial_baud      = 9600;
    std::string tcp_host         = "127.0.0.1";
    int         tcp_port         = 4001;
    float       gps_height_offset = 1.7f;   // camera eye height above terrain (m)

    // ── Camera ────────────────────────────────────────────────────────────
    float camera_pivot_x   = 0.f;
    float camera_pivot_y   = 0.f;
    float camera_pivot_z   = 0.f;
    float camera_radius    = 360.f;
    float camera_azimuth   = 270.f;
    float camera_elevation =  33.7f;

    // ── Window ────────────────────────────────────────────────────────────
    int window_x      = INT_MIN;  // INT_MIN → CW_USEDEFAULT (OS-chosen position)
    int window_y      = INT_MIN;
    int window_width  = 1280;
    int window_height = 720;

    // ── Misc ──────────────────────────────────────────────────────────────
    bool        disk_cache_keep_on_exit = true;
    std::string last_connected_at;  // ISO8601 or empty; written by Phase 11
};

class Session {
public:
    // Returns the canonical path for session.json.
    //   Primary:  %APPDATA%\TerrainViewer\session.json
    //   Fallback: <exe_dir>\session.json
    static std::filesystem::path DefaultPath();

    // Load from path.
    //   - File missing: returns false, toastMsg is empty (silent).
    //   - Parse error: renames .json → .bak, returns false, toastMsg set.
    //   - Success: data populated from file, loaded = true, returns true.
    bool Load(const std::filesystem::path& path, std::string& toastMsg);

    // Save to path.  Creates parent directories as needed.
    // Returns true on success.
    bool Save(const std::filesystem::path& path) const;

    SessionData data;
    bool        loaded = false;  // true iff data was read from an existing file
};

} // namespace app
