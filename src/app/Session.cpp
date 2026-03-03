// src/app/Session.cpp
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Session.h"
#include <nlohmann/json.hpp>

#include <climits>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;
using     json = nlohmann::json;

namespace app {

// ── Path helpers ─────────────────────────────────────────────────────────────

fs::path Session::DefaultPath()
{
    wchar_t buf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH) > 0)
        return fs::path(buf) / L"TerrainViewer" / L"session.json";

    // Fallback: directory containing the running executable.
    wchar_t exeBuf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    return fs::path(exeBuf).parent_path() / L"session.json";
}

// ── Load ─────────────────────────────────────────────────────────────────────

bool Session::Load(const fs::path& path, std::string& toastMsg)
{
    toastMsg.clear();
    loaded = false;

    if (!fs::exists(path))
        return false;  // Missing file — silent, use defaults.

    std::ifstream f(path);
    if (!f.is_open())
        return false;

    json j;
    try {
        j = json::parse(f);
    } catch (const std::exception&) {
        f.close();
        // Rename corrupt file so it doesn't block future sessions.
        std::error_code ec;
        fs::path bak = path;
        bak.replace_extension(L".bak");
        fs::rename(path, bak, ec);
        toastMsg = "session.json was corrupt — renamed to .bak, using defaults.";
        return false;
    }

    // Parse every field with a safe default fallback.
    // Unknown keys are ignored; missing keys get the struct default.
    auto obj = [](const json& j, const char* k) -> json {
        auto it = j.find(k);
        return (it != j.end() && it->is_object()) ? *it : json::object();
    };
    auto str = [](const json& j, const char* k, const std::string& def) -> std::string {
        auto it = j.find(k);
        if (it != j.end() && it->is_string()) return it->get<std::string>();
        return def;
    };
    auto b = [](const json& j, const char* k, bool def) -> bool {
        auto it = j.find(k);
        if (it != j.end() && it->is_boolean()) return it->get<bool>();
        return def;
    };
    auto i32 = [](const json& j, const char* k, int def) -> int {
        auto it = j.find(k);
        if (it != j.end() && it->is_number()) return it->get<int>();
        return def;
    };
    auto f32 = [](const json& j, const char* k, float def) -> float {
        auto it = j.find(k);
        if (it != j.end() && it->is_number()) return it->get<float>();
        return def;
    };

    const json fi = obj(j, "files");
    data.terrain_dxf  = str(fi, "terrain_dxf",  {});
    data.design_dxf   = str(fi, "design_dxf",   {});
    data.linework_dxf = str(fi, "linework_dxf",  {});

    const json vi = obj(j, "visibility");
    data.show_terrain  = b(vi, "terrain",  true);
    data.show_design   = b(vi, "design",   true);
    data.show_linework = b(vi, "linework", true);
    data.gps_mode      = b(vi, "gps_mode", false);

    const json op = obj(j, "opacity");
    data.terrain_opacity  = f32(op, "terrain",  1.0f);
    data.design_opacity   = f32(op, "design",   0.6f);
    data.linework_opacity = f32(op, "linework", 1.0f);

    const json cr = obj(j, "crs");
    data.crs_zone  = i32(cr, "zone",  55);
    data.crs_datum = str(cr, "datum", "GDA94");

    const json gp = obj(j, "gps");
    data.gps_source        = str(gp, "source",        "mock");
    data.serial_port       = str(gp, "serial_port",   "COM3");
    data.serial_baud       = i32(gp, "serial_baud",   9600);
    data.tcp_host          = str(gp, "tcp_host",       "127.0.0.1");
    data.tcp_port          = i32(gp, "tcp_port",       4001);
    data.gps_height_offset = f32(gp, "height_offset",  1.7f);

    const json ca = obj(j, "camera");
    data.camera_pivot_x   = f32(ca, "pivot_x",   0.f);
    data.camera_pivot_y   = f32(ca, "pivot_y",   0.f);
    data.camera_pivot_z   = f32(ca, "pivot_z",   0.f);
    data.camera_radius    = f32(ca, "radius",    360.f);
    data.camera_azimuth   = f32(ca, "azimuth",   270.f);
    data.camera_elevation = f32(ca, "elevation",  33.7f);

    const json wi = obj(j, "window");
    data.window_x      = i32(wi, "x",      INT_MIN);
    data.window_y      = i32(wi, "y",      INT_MIN);
    data.window_width  = i32(wi, "width",  1280);
    data.window_height = i32(wi, "height", 720);

    data.disk_cache_keep_on_exit = b(j, "disk_cache_keep_on_exit", true);

    {
        auto it = j.find("last_connected_at");
        if (it != j.end() && it->is_string())
            data.last_connected_at = it->get<std::string>();
    }

    loaded = true;
    return true;
}

// ── Save ─────────────────────────────────────────────────────────────────────

bool Session::Save(const fs::path& path) const
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    const SessionData& d = data;

    json j = {
        { "schema_version", d.schema_version },
        { "files", {
            { "terrain_dxf",  d.terrain_dxf  },
            { "design_dxf",   d.design_dxf   },
            { "linework_dxf", d.linework_dxf  },
        }},
        { "visibility", {
            { "terrain",  d.show_terrain  },
            { "design",   d.show_design   },
            { "linework", d.show_linework },
            { "gps_mode", d.gps_mode      },
        }},
        { "opacity", {
            { "terrain",  d.terrain_opacity  },
            { "design",   d.design_opacity   },
            { "linework", d.linework_opacity },
        }},
        { "crs", {
            { "zone",  d.crs_zone  },
            { "datum", d.crs_datum },
        }},
        { "gps", {
            { "source",        d.gps_source        },
            { "serial_port",   d.serial_port        },
            { "serial_baud",   d.serial_baud        },
            { "tcp_host",      d.tcp_host           },
            { "tcp_port",      d.tcp_port           },
            { "height_offset", d.gps_height_offset  },
        }},
        { "camera", {
            { "pivot_x",   d.camera_pivot_x   },
            { "pivot_y",   d.camera_pivot_y   },
            { "pivot_z",   d.camera_pivot_z   },
            { "radius",    d.camera_radius    },
            { "azimuth",   d.camera_azimuth   },
            { "elevation", d.camera_elevation },
        }},
        { "window", {
            { "x",      d.window_x      },
            { "y",      d.window_y      },
            { "width",  d.window_width  },
            { "height", d.window_height },
        }},
        { "disk_cache_keep_on_exit", d.disk_cache_keep_on_exit },
        { "last_connected_at",
          d.last_connected_at.empty() ? json(nullptr) : json(d.last_connected_at) },
    };

    // Write atomically: write to .tmp then rename.
    const fs::path tmp = path.parent_path() / (path.stem().wstring() + L".tmp");
    {
        std::ofstream out(tmp);
        if (!out.is_open()) return false;
        out << j.dump(2);
        if (!out.good()) return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

} // namespace app
