#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <tchar.h>
#include <shobjidl.h>   // IFileOpenDialog

#include "renderer/Renderer.h"
#include "app/Camera.h"
#include "app/Session.h"
#include "terrain/Config.h"
#include "terrain/GpuBudget.h"
#include "terrain/DesignPass.h"
#include "terrain/LineworkMesh.h"
#include "terrain/LineworkPass.h"
#include "terrain/TerrainPass.h"
#include "terrain/TileGrid.h"
#include "DxfParser.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "gps/IGpsSource.h"
#include "gps/MockGpsSource.h"
#include "gps/SerialGps.h"
#include "gps/TcpGps.h"
#include "gps/CoordReadout.h"

#include "DxfTypes.h"   // dxf::ParsedPolyline for DesignSet

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>

namespace fs = std::filesystem;

// ── DesignSet — one group of design surface + linework from the designs folder ─
struct DesignSet {
    std::string name;           // base name shown in sidebar
    std::string surface_path;   // _TRI.dxf, .dxf, or empty
    std::string linework_path;  // _CAD.dxf, .dxf, or empty
    bool single_file = false;   // true = one DXF contains both 3DFACE and POLYLINE
    bool visible     = true;

    // Runtime state — populated on load
    std::unique_ptr<TileGrid>      grid;      // null until surface loaded
    std::unique_ptr<LineworkMesh>  lwMesh;    // null until linework loaded
    // Polylines cached in RAM so re-check avoids re-parsing the CAD file
    std::vector<dxf::ParsedPolyline> polylines;

    std::array<float,3> surfOrigin = {};   // from parseToCache result
    std::array<float,3> lwOrigin   = {};
    std::string msg;
    int budgetBase = 0;          // unique budget key base = 100000 * (index+1)

    bool surfParsed = false;     // true once surface cache dir has tiles
    fs::path surfCacheDir;
    fs::path lwCacheDir;
};

// ── Folder paths (v3 session) ─────────────────────────────────────────────────
static std::string g_terrainFolder;
static std::string g_designsFolder;

// Derived from terrain folder scan; used only for GPS nmea path.
static std::string g_terrainDxfPath;

// ── Active design sets ────────────────────────────────────────────────────────
static std::vector<DesignSet> g_designSets;

// ── Render pipeline ───────────────────────────────────────────────────────────

static Renderer    g_renderer;
static Camera      g_camera;
static GpuBudget   g_budget(static_cast<size_t>(terrain::GPU_BUDGET_MB) * 1024 * 1024);
static TileGrid    g_tileGrid;
static TerrainPass   g_terrainPass;
static DesignPass    g_designPass;
static LineworkPass  g_lineworkPass;
static bool          g_running    = true;
static bool          g_tilesReady = false;
static std::string   g_statusMsg;

// Scene origin — terrain is authoritative.
static std::array<float, 3> g_sceneOrigin = {0.0f, 0.0f, 0.0f};

// ── Visibility / mode flags ───────────────────────────────────────────────────

static bool               g_showTerrain  = true;
static bool               g_sidebarOpen  = true;
static bool               g_gpsMode       = false;
static DirectX::XMFLOAT3 g_defaultPivot  = {0.0f, 0.0f, 0.0f};
static float              g_defaultRadius = 360.0f;

// ── Layer opacity (global — applies to all design sets) ───────────────────────
static float g_terrainOpacity  = 1.0f;
static float g_designOpacity   = 0.6f;
static float g_lineworkOpacity = 1.0f;

// ── GPS source config ─────────────────────────────────────────────────────────

enum class GpsSourceType { Mock = 0, Serial = 1, Tcp = 2 };
static GpsSourceType g_gpsSourceType  = GpsSourceType::Mock;
static char          g_serialPort[16] = "COM3";
static int           g_serialBaud     = 9600;
static char          g_tcpHost[128]   = "127.0.0.1";
static int           g_tcpPort        = 4001;
static float         g_gpsHeightOffset = terrain::GPS_HEIGHT_OFFSET_M;

// ── CRS config ────────────────────────────────────────────────────────────────
static int           g_crsZone  = terrain::GPS_MGA_ZONE;
static gps::Datum    g_crsDatum = gps::Datum::GDA94;
static int           g_uiZone   = terrain::GPS_MGA_ZONE;
static int           g_uiDatum  = 0;   // 0=GDA94, 1=GDA2020
static int           g_zoneSuggest = 0;

// ── GPS state ─────────────────────────────────────────────────────────────────

static std::unique_ptr<gps::IGpsSource> g_gpsSrc;
static gps::ScenePosition g_gpsLastKnown{};
static float g_gpsCachedElev      = 0.0f;
static float g_gpsPrevElevX       = 0.0f;
static float g_gpsPrevElevY       = 0.0f;
static bool  g_gpsNeedElevLookup  = true;

// ── Coordinate readout cache ──────────────────────────────────────────────────
static gps::MgaCoord g_coordMga{};
static gps::WgsCoord g_coordWgs{};
static float         g_coordPrevX = std::numeric_limits<float>::quiet_NaN();
static float         g_coordPrevY = std::numeric_limits<float>::quiet_NaN();
static bool          g_coordValid = false;

// ── Session ───────────────────────────────────────────────────────────────────
static app::Session          g_session;
static fs::path              g_sessionPath;
static std::string           g_toast;
static float                 g_toastTimer = 0.f;
static std::atomic<bool>     g_sessionSavePending{false};
static std::atomic<bool>     g_autoSaveRunning{false};
static std::thread           g_autoSaveThread;

// ── Async load ────────────────────────────────────────────────────────────────
// FullReload: loads terrain + all visible design sets in a background thread.
// DesignOnly: loads one design set (first-time parse) in a background thread.
// g_loadApplyPending is the release/acquire barrier between thread and main.

enum class LoadKind { Full, DesignOnly };

struct DesignLoadResult {
    std::string      surfaceError;
    std::string      lineworkError;
    dxf::ParseResult surfaceResult;
    dxf::ParseResult lineworkResult;  // for paired: from _CAD.dxf separately
    fs::path         surfaceCacheDir;
    fs::path         lineworkCacheDir;
    bool             single_file = false;
    std::string      name;  // matches DesignSet::name for ApplyLoadResults
};

struct LoadData {
    std::string      terrainError;
    dxf::ParseResult terrainResult;
    fs::path         terrainCacheDir;
    std::vector<DesignLoadResult> designResults;  // one per active design set
};

static LoadData            g_loadData;
static LoadKind            g_loadKind       = LoadKind::Full;
static int                 g_loadDesignIdx  = -1;  // DesignOnly: which g_designSets entry
static std::atomic<float>  g_parseProgress{0.0f};
static std::atomic<bool>   g_parseBusy{false};
static std::atomic<bool>   g_loadApplyPending{false};
static std::atomic<int>    g_parseCurrentFile{0};  // index into current parse step label
static std::thread         g_loaderThread;

// ── Server config (Phase 10+) ─────────────────────────────────────────────────
static char        g_serverUrl[256]         = "";
static bool        g_serverEnabled          = false;
static std::string g_serverLastConnectedAt;  // ISO8601 or empty; updated by Phase 11

// ── Freshness overlay ─────────────────────────────────────────────────────────
static bool                  g_freshnessOverlay     = false;
static fs::path              g_terrainCacheDir;      // set after terrain loads
static DirectX::XMFLOAT4    g_terrainFreshnessColor = { 1.f, 0.1f, 0.1f, 0.5f };

// ── LHS button bar ────────────────────────────────────────────────────────────
static float g_zoomStep = 0.5f;   // notches per button tap; configurable in Settings

// ── Click-and-hold coord pick ─────────────────────────────────────────────────

// Per-surface pick result (terrain or one design set).
struct PickHit {
    std::string label;          // "Terrain" or DesignSet.name
    gps::MgaCoord mga{};
    bool hasCutFill = false;
    float cutFill   = 0.f;      // design_z - terrain_z; positive=fill, negative=cut
};

static float g_lmbHeldSecs  = 0.f;
static float g_lmbDragAccum = 0.f;
static float g_lmbDownX     = 0.f;
static float g_lmbDownY     = 0.f;
static bool  g_pickActive      = false;
static float g_pickTimer       = 0.f;
static bool  g_pickHasHit      = false;
static DirectX::XMFLOAT3 g_pickHitScene = {};  // scene-space primary hit (for crosshair)
static std::vector<PickHit> g_pickHits;         // one entry per surface hit, Z desc order
// Set true when a hold-triggered pick opens the popup so the LMB release that
// ends the hold is ignored by the click-outside dismiss logic.
static bool  g_pickJustOpened  = false;
// Saved camera matrices at the moment the hold started.
static DirectX::XMMATRIX g_pickView = DirectX::XMMatrixIdentity();
static DirectX::XMMATRIX g_pickProj = DirectX::XMMatrixIdentity();
// Touch long-press state (single-finger hold)
static float g_touchHoldSecs   = 0.f;
static float g_touchHoldDragPx = 0.f;
static float g_touchHoldX      = 0.f;
static float g_touchHoldY      = 0.f;

// ── Window handle ─────────────────────────────────────────────────────────────
static HWND g_hwnd = nullptr;

// ── Pick debug overlay (toggle: backtick key; compile in Debug only) ──────────
#ifdef _DEBUG
static bool g_pickDbgVisible = false;
struct PickDbgInfo {
    DirectX::XMFLOAT3 rayOrigin{}, rayDir{}, hitScene{};
    float reprX{}, reprY{}, cursorX{}, cursorY{};
};
static PickDbgInfo g_pickDbg{};
#endif

// ── Mouse state ───────────────────────────────────────────────────────────────

static POINT g_lastMousePos = {0, 0};
static bool  g_lmbDown      = false;
static bool  g_rmbDown      = false;
static bool  g_mmbDown      = false;

// ── Touch state (WM_POINTER, up to 2 contacts) ────────────────────────────────

struct TouchContact { UINT32 id; float x, y; bool live = false; };
static TouchContact g_touch[2];
static int          g_touchN      = 0;
static float        g_touchPrevCX = 0.f;
static float        g_touchPrevCY = 0.f;
static float        g_touchPrevD  = 1.f;

// Sidebar bounds — used for touch hit-testing to block viewport input.
static ImVec2 g_sidebarPos  = {0.f, 0.f};
static ImVec2 g_sidebarSize = {0.f, 0.f};

// ── UI colour scheme (F3) ─────────────────────────────────────────────────────
// Accent: #2E75B6   Status green: #2ECC71   orange: #E67E22   red: #C0392B
static constexpr ImVec4 kAccentColor = { 0.180f, 0.459f, 0.714f, 1.0f };
static constexpr ImVec4 kGreenColor  = { 0.180f, 0.800f, 0.443f, 1.0f };
static constexpr ImVec4 kOrangeColor = { 0.902f, 0.494f, 0.133f, 1.0f };
static constexpr ImVec4 kRedColor    = { 0.753f, 0.224f, 0.169f, 1.0f };

// ── Thousands-separator formatter ─────────────────────────────────────────────
// Formats a value with 1 decimal place and space-separated groups: 7563891.2 → "7 563 891.2"
static std::string FormatThousands(double val, int dp = 1)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", dp, val);
    std::string s(buf);
    const size_t dot = s.find('.');
    int intEnd = static_cast<int>(dot == std::string::npos ? s.size() : dot);
    for (int pos = intEnd - 3; pos > 0; pos -= 3)
        s.insert(static_cast<size_t>(pos), " ");
    return s;
}

// ── Freshness color from cache.meta ───────────────────────────────────────────
// Reads server_last_modified from cacheDir/cache.meta and returns an RGBA tint.
// Green <24h, Yellow <7d, Orange <30d, Red >30d or no server data.
static DirectX::XMFLOAT4 ComputeFreshnessColor(const fs::path& cacheDir)
{
    // Red = local-only / unknown.
    static constexpr DirectX::XMFLOAT4 kRed    = { 1.f, 0.1f, 0.1f, 0.5f };
    static constexpr DirectX::XMFLOAT4 kOrange = { 0.9f, 0.5f, 0.1f, 0.5f };
    static constexpr DirectX::XMFLOAT4 kYellow = { 0.9f, 0.9f, 0.1f, 0.5f };
    static constexpr DirectX::XMFLOAT4 kGreen  = { 0.2f, 0.8f, 0.2f, 0.5f };

    if (cacheDir.empty()) return kRed;
    const fs::path metaPath = cacheDir / "cache.meta";
    if (!fs::exists(metaPath)) return kRed;

    try {
        std::ifstream f(metaPath);
        const auto j = nlohmann::json::parse(f);
        auto it = j.find("server_last_modified");
        if (it == j.end() || it->is_null()) return kRed;
        const std::string ts = it->get<std::string>();
        if (ts.empty()) return kRed;

        std::tm tm{};
        if (sscanf_s(ts.c_str(), "%d-%d-%dT%d:%d:%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 3) {
            tm.tm_year -= 1900;
            tm.tm_mon  -= 1;
            tm.tm_isdst = -1;
            const time_t then = _mkgmtime(&tm);
            const double diffDays = difftime(time(nullptr), then) / 86400.0;
            if (diffDays <  1.0) return kGreen;
            if (diffDays <  7.0) return kYellow;
            if (diffDays < 30.0) return kOrange;
        }
    } catch (...) {}
    return kRed;
}

// ── Offline indicator time ─────────────────────────────────────────────────────
// Returns {hours, minutes} elapsed since server_last_connected_at, or {-1, 0}
// if the condition for showing the banner is not met.
static std::pair<int, int> GetOfflineTime()
{
    if (!g_serverEnabled || g_serverUrl[0] == '\0' || g_serverLastConnectedAt.empty())
        return { -1, 0 };

    std::tm tm{};
    if (sscanf_s(g_serverLastConnectedAt.c_str(), "%d-%d-%dT%d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 3)
        return { -1, 0 };

    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    tm.tm_isdst = -1;
    const time_t then = _mkgmtime(&tm);
    const double diffSecs = difftime(time(nullptr), then);
    if (diffSecs < 0) return { 0, 0 };
    const int totalMins = static_cast<int>(diffSecs / 60.0);
    return { totalMins / 60, totalMins % 60 };
}

// ── Touch baseline reset ───────────────────────────────────────────────────────
static void TouchUpdateBaselines()
{
    if (g_touchN == 1) {
        for (const auto& c : g_touch)
            if (c.live) { g_touchPrevCX = c.x; g_touchPrevCY = c.y; break; }
    } else if (g_touchN == 2) {
        g_touchPrevCX = (g_touch[0].x + g_touch[1].x) * 0.5f;
        g_touchPrevCY = (g_touch[0].y + g_touch[1].y) * 0.5f;
        const float dx = g_touch[1].x - g_touch[0].x;
        const float dy = g_touch[1].y - g_touch[0].y;
        g_touchPrevD  = std::max(1.f, sqrtf(dx * dx + dy * dy));
    }
}

// ── Unproject screen pixel → world-space ray ──────────────────────────────────
static void UnprojectRay(float px, float py, float vpW, float vpH,
                          const DirectX::XMMATRIX& view,
                          const DirectX::XMMATRIX& proj,
                          DirectX::XMFLOAT3& rayOriginOut,
                          DirectX::XMFLOAT3& rayDirOut)
{
    using namespace DirectX;
    const float ndcX =  2.0f * px / vpW - 1.0f;
    const float ndcY = -2.0f * py / vpH + 1.0f;

    const XMMATRIX invVP = XMMatrixInverse(nullptr, view * proj);

    XMVECTOR nearNdc   = XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
    XMVECTOR farNdc    = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);
    XMVECTOR nearWorld = XMVector4Transform(nearNdc, invVP);
    XMVECTOR farWorld  = XMVector4Transform(farNdc,  invVP);

    nearWorld = nearWorld / XMVectorSplatW(nearWorld);
    farWorld  = farWorld  / XMVectorSplatW(farWorld);

    XMStoreFloat3(&rayOriginOut, nearWorld);
    XMStoreFloat3(&rayDirOut,    XMVector3Normalize(farWorld - nearWorld));
}

// ── ImGui style (F3) ─────────────────────────────────────────────────────────
static void ApplyImGuiStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    // Rounding
    s.WindowRounding    = 6.0f;
    s.FrameRounding     = 6.0f;
    s.PopupRounding     = 6.0f;
    s.ScrollbarRounding = 6.0f;

    // Padding / spacing
    s.WindowPadding = { 12.f, 12.f };
    s.FramePadding  = {  8.f,  5.f };
    s.ItemSpacing   = {  8.f,  6.f };

    // Accent colours (#2E75B6 and variants)
    constexpr ImVec4 acc    = { 0.180f, 0.459f, 0.714f, 1.0f };
    constexpr ImVec4 accHov = { 0.250f, 0.530f, 0.790f, 1.0f };
    constexpr ImVec4 accAct = { 0.140f, 0.390f, 0.640f, 1.0f };

    auto& c = s.Colors;
    c[ImGuiCol_CheckMark]        = acc;
    c[ImGuiCol_SliderGrab]       = acc;
    c[ImGuiCol_SliderGrabActive] = accAct;
    c[ImGuiCol_Button]           = { acc.x, acc.y, acc.z, 0.55f };
    c[ImGuiCol_ButtonHovered]    = accHov;
    c[ImGuiCol_ButtonActive]     = accAct;
    c[ImGuiCol_Header]           = { acc.x, acc.y, acc.z, 0.40f };
    c[ImGuiCol_HeaderHovered]    = { acc.x, acc.y, acc.z, 0.75f };
    c[ImGuiCol_HeaderActive]     = accAct;
    c[ImGuiCol_FrameBgActive]    = { acc.x, acc.y, acc.z, 0.35f };

    // Font: Segoe UI 16px — silent fallback to ImGui default if file missing
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
    // If the above returns null ImGui automatically uses its built-in default font.
}

// ── GPS source factory ────────────────────────────────────────────────────────
static void RecreateGpsSource()
{
    g_gpsSrc.reset();

    const fs::path nmeaPath =
        fs::path(g_terrainDxfPath).parent_path() / "gps.nmea";

    switch (g_gpsSourceType) {
    case GpsSourceType::Mock:
        g_gpsSrc = std::make_unique<gps::MockGpsSource>(
            nmeaPath, g_sceneOrigin, g_crsZone, g_crsDatum);
        break;
    case GpsSourceType::Serial:
        g_gpsSrc = std::make_unique<gps::SerialGps>(
            g_serialPort, g_serialBaud, g_sceneOrigin, g_crsZone, g_crsDatum);
        break;
    case GpsSourceType::Tcp:
        g_gpsSrc = std::make_unique<gps::TcpGps>(
            g_tcpHost, g_tcpPort, g_sceneOrigin, g_crsZone, g_crsDatum);
        break;
    }

    g_gpsNeedElevLookup = true;
}

// ── Session gather ────────────────────────────────────────────────────────────
static void GatherSession()
{
    auto& d = g_session.data;

    d.terrain_folder  = g_terrainFolder;
    d.terrain_visible = g_showTerrain;
    d.designs_folder  = g_designsFolder;

    // Rebuild active_designs list from current g_designSets
    d.active_designs.clear();
    for (const auto& ds : g_designSets) {
        app::ActiveDesign ad;
        ad.name    = ds.name;
        ad.visible = ds.visible;
        d.active_designs.push_back(std::move(ad));
    }

    d.gps_mode = g_gpsMode;

    d.terrain_opacity  = g_terrainOpacity;
    d.design_opacity   = g_designOpacity;
    d.linework_opacity = g_lineworkOpacity;

    d.crs_zone  = g_crsZone;
    d.crs_datum = (g_crsDatum == gps::Datum::GDA2020) ? "GDA2020" : "GDA94";

    switch (g_gpsSourceType) {
    case GpsSourceType::Mock:   d.gps_source = "mock";   break;
    case GpsSourceType::Serial: d.gps_source = "serial"; break;
    case GpsSourceType::Tcp:    d.gps_source = "tcp";    break;
    }
    d.serial_port       = g_serialPort;
    d.serial_baud       = g_serialBaud;
    d.tcp_host          = g_tcpHost;
    d.tcp_port          = g_tcpPort;
    d.gps_height_offset = g_gpsHeightOffset;

    const auto piv     = g_camera.Pivot();
    d.camera_pivot_x   = piv.x;
    d.camera_pivot_y   = piv.y;
    d.camera_pivot_z   = piv.z;
    d.camera_radius    = g_camera.Radius();
    d.camera_azimuth   = g_camera.Azimuth();
    d.camera_elevation = g_camera.Elevation();

    if (g_hwnd) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        if (GetWindowPlacement(g_hwnd, &wp)) {
            d.window_x      = wp.rcNormalPosition.left;
            d.window_y      = wp.rcNormalPosition.top;
            d.window_width  = wp.rcNormalPosition.right  - wp.rcNormalPosition.left;
            d.window_height = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        }
        d.window_maximized = (IsZoomed(g_hwnd) != FALSE);
    }

    d.server_url              = g_serverUrl;
    d.server_enabled          = g_serverEnabled;
    d.server_last_connected_at = g_serverLastConnectedAt;
    d.freshness_overlay_visible = g_freshnessOverlay;
    d.zoom_step               = g_zoomStep;
}

// ── Session apply ─────────────────────────────────────────────────────────────
static void ApplySession()
{
    const auto& d = g_session.data;

    g_showTerrain = d.terrain_visible;
    g_gpsMode     = d.gps_mode;

    g_terrainOpacity  = d.terrain_opacity;
    g_designOpacity   = d.design_opacity;
    g_lineworkOpacity = d.linework_opacity;
    g_terrainPass.SetOpacity(g_terrainOpacity);
    g_designPass.SetOpacity(g_designOpacity);
    g_lineworkPass.SetOpacity(g_lineworkOpacity);

    g_crsZone  = d.crs_zone;
    g_crsDatum = (d.crs_datum == "GDA2020") ? gps::Datum::GDA2020 : gps::Datum::GDA94;
    g_uiZone   = g_crsZone;
    g_uiDatum  = (g_crsDatum == gps::Datum::GDA2020) ? 1 : 0;

    if      (d.gps_source == "serial") g_gpsSourceType = GpsSourceType::Serial;
    else if (d.gps_source == "tcp")    g_gpsSourceType = GpsSourceType::Tcp;
    else                               g_gpsSourceType = GpsSourceType::Mock;

    if (!d.serial_port.empty())
        strncpy_s(g_serialPort, d.serial_port.c_str(), sizeof(g_serialPort) - 1);
    g_serialBaud = d.serial_baud;
    if (!d.tcp_host.empty())
        strncpy_s(g_tcpHost, d.tcp_host.c_str(), sizeof(g_tcpHost) - 1);
    g_tcpPort        = d.tcp_port;
    g_gpsHeightOffset = d.gps_height_offset;

    // Camera — only restore if a session file was found.
    if (g_session.loaded) {
        g_camera.SetPivot(d.camera_pivot_x, d.camera_pivot_y, d.camera_pivot_z);
        g_camera.SetSpherical(d.camera_radius, d.camera_azimuth, d.camera_elevation);
    }

    if (!d.server_url.empty())
        strncpy_s(g_serverUrl, d.server_url.c_str(), sizeof(g_serverUrl) - 1);
    g_serverEnabled          = d.server_enabled;
    g_serverLastConnectedAt  = d.server_last_connected_at;
    g_freshnessOverlay       = d.freshness_overlay_visible;
    g_zoomStep               = d.zoom_step;

    RecreateGpsSource();
}

// ── File dialog ───────────────────────────────────────────────────────────────
// Returns chosen file path (UTF-8) or empty string if cancelled / error.
static std::string OpenFileDlg(HWND owner, const wchar_t* title)
{
    IFileOpenDialog* pDlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
    if (FAILED(hr)) return {};

    if (title) pDlg->SetTitle(title);

    static const COMDLG_FILTERSPEC kFilter[] = {
        { L"DXF files", L"*.dxf;*.DXF" },
        { L"All files", L"*.*"          },
    };
    pDlg->SetFileTypes(ARRAYSIZE(kFilter), kFilter);
    pDlg->SetFileTypeIndex(1);
    pDlg->SetDefaultExtension(L"dxf");

    hr = pDlg->Show(owner);
    if (FAILED(hr)) { pDlg->Release(); return {}; }

    IShellItem* pItem = nullptr;
    hr = pDlg->GetResult(&pItem);
    pDlg->Release();
    if (FAILED(hr)) return {};

    PWSTR pszPath = nullptr;
    std::string result;
    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
    if (SUCCEEDED(hr) && pszPath) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1,
                                          nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            result.resize(static_cast<size_t>(n) - 1);
            WideCharToMultiByte(CP_UTF8, 0, pszPath, -1,
                                result.data(), n, nullptr, nullptr);
        }
        CoTaskMemFree(pszPath);
    }
    pItem->Release();
    return result;
}

// Returns chosen folder path (UTF-8) or empty string if cancelled / error.
static std::string OpenFolderDlg(HWND owner, const wchar_t* title)
{
    IFileOpenDialog* pDlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
    if (FAILED(hr)) return {};

    FILEOPENDIALOGOPTIONS opts = 0;
    pDlg->GetOptions(&opts);
    pDlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (title) pDlg->SetTitle(title);

    hr = pDlg->Show(owner);
    if (FAILED(hr)) { pDlg->Release(); return {}; }

    IShellItem* pItem = nullptr;
    hr = pDlg->GetResult(&pItem);
    pDlg->Release();
    if (FAILED(hr)) return {};

    PWSTR pszPath = nullptr;
    std::string result;
    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
    if (SUCCEEDED(hr) && pszPath) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1,
                                          nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            result.resize(static_cast<size_t>(n) - 1);
            WideCharToMultiByte(CP_UTF8, 0, pszPath, -1,
                                result.data(), n, nullptr, nullptr);
        }
        CoTaskMemFree(pszPath);
    }
    pItem->Release();
    return result;
}

// ── Folder scanner helpers ────────────────────────────────────────────────────

// Returns path of the first .dxf file found in folderPath (case-insensitive),
// or empty string if none found. Logs a warning if multiple are found.
static std::string ScanTerrainFolder(const std::string& folderPath)
{
    if (folderPath.empty()) return {};
    std::error_code ec;
    if (!fs::is_directory(folderPath, ec)) return {};

    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(folderPath, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto ext = entry.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (ext == ".dxf") found.push_back(entry.path());
    }
    if (found.empty()) return {};
    // Sort for determinism; first file wins.
    std::sort(found.begin(), found.end());
    return found[0].string();
}

// Scans folderPath for .dxf files and groups them into DesignSets by base name.
// Detection rules (in order):
//   1. *_TRI.dxf  → paired surface; look for matching *_CAD.dxf
//   2. *_CAD.dxf (no matching _TRI) → linework-only set
//   3. *.dxf (no suffix) → single-file (both surface + linework)
//   4. Paired + single for same base name → warn, prefer paired
static std::vector<DesignSet> ScanDesignFolder(const std::string& folderPath)
{
    if (folderPath.empty()) return {};
    std::error_code ec;
    if (!fs::is_directory(folderPath, ec)) return {};

    // Collect all .dxf paths
    std::vector<fs::path> dxfs;
    for (const auto& entry : fs::directory_iterator(folderPath, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto ext = entry.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (ext == ".dxf") dxfs.push_back(entry.path());
    }
    std::sort(dxfs.begin(), dxfs.end());

    // Group by base name using ordered map for consistent ordering
    struct RawEntry {
        std::string surface_path;
        std::string linework_path;
        bool has_tri = false;
        bool has_cad = false;
        bool has_single = false;
        std::string single_path;
    };
    std::map<std::string, RawEntry> byName;

    for (const auto& p : dxfs) {
        const std::string stem = p.stem().string();
        const std::string path = p.string();

        auto endsWith = [](const std::string& s, const std::string& suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        };

        if (endsWith(stem, "_TRI")) {
            const std::string base = stem.substr(0, stem.size() - 4);
            byName[base].has_tri = true;
            byName[base].surface_path = path;
        } else if (endsWith(stem, "_CAD")) {
            const std::string base = stem.substr(0, stem.size() - 4);
            byName[base].has_cad = true;
            byName[base].linework_path = path;
        } else {
            byName[stem].has_single = true;
            byName[stem].single_path = path;
        }
    }

    std::vector<DesignSet> result;
    for (auto& [name, raw] : byName) {
        DesignSet ds;
        ds.name = name;

        const bool paired = raw.has_tri || raw.has_cad;
        if (paired) {
            // Paired takes precedence; warn if also single exists
            ds.surface_path    = raw.surface_path;
            ds.linework_path   = raw.linework_path;
            ds.single_file     = false;
        } else if (raw.has_single) {
            // Case B: single DXF for both surface and linework
            ds.surface_path    = raw.single_path;
            ds.linework_path   = raw.single_path;
            ds.single_file     = true;
        } else {
            continue; // should not happen
        }
        result.push_back(std::move(ds));
    }
    return result;
}

// ── Surface coord pick helpers ────────────────────────────────────────────────

// Unprojects screen pixel, triangle-level ray-casts all visible surfaces,
// collects hits from terrain and all visible design sets, computes Cut/Fill
// for each design vs terrain, and populates g_pickHits + g_pickHitScene.
// Sets g_lmbHeldSecs=-999 / g_touchHoldSecs=-999 to prevent re-trigger.
static void TriggerSurfacePick(float px, float py, float vpW, float vpH,
                                const DirectX::XMMATRIX& view,
                                const DirectX::XMMATRIX& proj)
{
    using namespace DirectX;
    XMFLOAT3 rayO, rayD;
    UnprojectRay(px, py, vpW, vpH, view, proj, rayO, rayD);

    // ── Terrain hit ────────────────────────────────────────────────────────
    // requireGpu=false: reads triangle data from disk regardless of GPU state.
    // This prevents misses when S4's proactive eviction temporarily removes the
    // tile from GPU (LOADING/EMPTY at the moment the 0.5s hold fires), and also
    // covers centroid-binning gaps where a triangle's disk tile has a broader
    // AABB than the GPU-resident version.
    XMFLOAT3 terrHit{};
    const bool hasTerr = g_tilesReady && g_showTerrain &&
                         g_tileGrid.RayCastDetailed(rayO, rayD, terrHit,
                                                    /*requireGpu=*/false);

    // ── Design set hits ────────────────────────────────────────────────────
    struct DsHit { int idx; XMFLOAT3 pos; };
    std::vector<DsHit> dsHits;
    for (int i = 0; i < static_cast<int>(g_designSets.size()); ++i) {
        const auto& ds = g_designSets[i];
        if (!ds.visible || !ds.grid) continue;
        XMFLOAT3 h{};
        if (ds.grid->RayCastDetailed(rayO, rayD, h, /*requireGpu=*/false))
            dsHits.push_back({ i, h });
    }

    if (!hasTerr && dsHits.empty()) {
        g_pickHasHit = false;
        g_pickHits.clear();
        g_pickActive     = true;
        g_pickTimer      = 8.f;
        g_lmbHeldSecs    = -999.f;
        g_touchHoldSecs  = -999.f;
        return;
    }

    // ── Determine primary hit (for crosshair placement) ────────────────────
    // Pick the surface the ray reaches first (closest to eye).
    const XMVECTOR ro = XMLoadFloat3(&rayO);
    XMFLOAT3 primaryHit = hasTerr ? terrHit : dsHits[0].pos;
    float bestDistSq = hasTerr
        ? XMVectorGetX(XMVector3LengthSq(XMLoadFloat3(&terrHit) - ro))
        : 1e30f;
    for (const auto& dh : dsHits) {
        const float d = XMVectorGetX(XMVector3LengthSq(XMLoadFloat3(&dh.pos) - ro));
        if (d < bestDistSq) { bestDistSq = d; primaryHit = dh.pos; }
    }
    g_pickHasHit   = true;
    g_pickHitScene = primaryHit;

    // ── Vertical ray for accurate Z on all surfaces at primary XY ──────────
    // Fired straight down from primaryHit XY so every surface row reports the
    // same Easting/Northing.  requireGpu=false so proactively-evicted tiles
    // (from S4) are still found via their on-disk LOD0 data.
    const XMFLOAT3 vertO = { primaryHit.x, primaryHit.y, 100000.f };
    const XMFLOAT3 vertD = { 0.f, 0.f, -1.f };

    XMFLOAT3 terrVert{};
    const bool terrVertOk = hasTerr &&
                            g_tileGrid.RayCastDetailed(vertO, vertD, terrVert,
                                                       /*requireGpu=*/false);

    // ── Build ordered hit list (Z descending) ─────────────────────────────
    // All rows share the same E/N (primaryHit XY converted to MGA).
    // Only Z varies — sourced from the vertical ray on each surface.
    g_pickHits.clear();

    // Terrain row — use vertical-ray Z so E/N matches all other rows exactly
    if (hasTerr) {
        PickHit ph;
        ph.label = "Terrain";
        const float terrZ = terrVertOk ? terrVert.z : terrHit.z;
        ph.mga = gps::sceneToMga(primaryHit.x, primaryHit.y, terrZ, g_sceneOrigin);
        g_pickHits.push_back(std::move(ph));
    }

    // Design rows — E/N always from primaryHit XY; Z from vertical ray
    for (const auto& dh : dsHits) {
        const auto& ds = g_designSets[dh.idx];
        PickHit ph;
        ph.label = ds.name;
        XMFLOAT3 desVert{};
        const bool dvOk = ds.grid->RayCastDetailed(vertO, vertD, desVert,
                                                    /*requireGpu=*/false);
        const float desZ = dvOk ? desVert.z : dh.pos.z;
        // E/N from primaryHit (same for all rows); only Z differs per surface
        ph.mga = gps::sceneToMga(primaryHit.x, primaryHit.y, desZ, g_sceneOrigin);
        if (terrVertOk && dvOk) {
            ph.hasCutFill = true;
            ph.cutFill    = desVert.z - terrVert.z;
        }
        g_pickHits.push_back(std::move(ph));
    }

    // Sort all hits by Z descending (highest surface first).
    // Terrain always shows first if it has the highest Z; otherwise it goes
    // where its elevation falls. The label disambiguates.
    std::sort(g_pickHits.begin(), g_pickHits.end(),
        [](const PickHit& a, const PickHit& b) {
            return a.mga.elev > b.mga.elev;
        });

    g_pickActive     = true;
    g_pickJustOpened = true;
    g_pickTimer      = 8.f;
    g_lmbHeldSecs    = -999.f;
    g_touchHoldSecs  = -999.f;

#ifdef _DEBUG
    if (g_pickDbgVisible) {
        g_pickDbg.rayOrigin = rayO;
        g_pickDbg.rayDir    = rayD;
        g_pickDbg.hitScene  = g_pickHitScene;
        g_pickDbg.cursorX   = px;
        g_pickDbg.cursorY   = py;
        const XMVECTOR ndc = XMVector3TransformCoord(
            XMLoadFloat3(&g_pickHitScene), view * proj);
        g_pickDbg.reprX = (XMVectorGetX(ndc) *  0.5f + 0.5f) * vpW;
        g_pickDbg.reprY = (XMVectorGetY(ndc) * -0.5f + 0.5f) * vpH;
    }
#endif
}

// Appends all current pick hits to saved_coords.csv in the terrain folder.
static void SavePickToCsv()
{
    if (!g_pickHasHit || g_pickHits.empty()) return;

    const fs::path csvPath = (!g_terrainFolder.empty())
        ? fs::path(g_terrainFolder) / "saved_coords.csv"
        : ([]{
                wchar_t e[MAX_PATH]{};
                GetModuleFileNameW(nullptr, e, MAX_PATH);
                return fs::path(e).parent_path() / "saved_coords.csv";
           })();

    const bool needsHeader = !fs::exists(csvPath);
    std::ofstream f(csvPath, std::ios::app);
    if (!f) return;
    if (needsHeader)
        f << "timestamp,easting_m,northing_m,elev_m,mga_zone,datum,surface,cut_fill_m\n";

    time_t t = time(nullptr);
    struct tm tm{};
    gmtime_s(&tm, &t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
    const char* datum = (g_crsDatum == gps::Datum::GDA2020) ? "GDA2020" : "GDA94";

    for (const auto& ph : g_pickHits) {
        f << std::fixed << ts << ","
          << std::setprecision(3) << ph.mga.easting  << ","
                                  << ph.mga.northing << ","
                                  << ph.mga.elev     << ","
          << g_crsZone << "," << datum << ","
          << ph.label  << ",";
        if (ph.hasCutFill)
            f << std::setprecision(3) << ph.cutFill;
        f << "\n";
    }

    g_toast      = "Saved to " + csvPath.filename().string();
    g_toastTimer = 4.f;
}

// Forward declaration required by imgui_impl_win32.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            g_renderer.OnResize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    // ── WM_POINTER: touch (PT_TOUCH) ─────────────────────────────────────
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP:
    {
        const UINT32 pid = GET_POINTERID_WPARAM(wParam);
        POINTER_INFO pi  = {};
        if (!GetPointerInfo(pid, &pi))
            return DefWindowProc(hwnd, msg, wParam, lParam);

        POINT pt = pi.ptPixelLocation;
        ScreenToClient(hwnd, &pt);
        const float px = static_cast<float>(pt.x);
        const float py = static_cast<float>(pt.y);

        if (pi.pointerType == PT_TOUCH) {
            const bool overUi = g_sidebarSize.x > 0.f
                && px >= g_sidebarPos.x && px < g_sidebarPos.x + g_sidebarSize.x
                && py >= g_sidebarPos.y && py < g_sidebarPos.y + g_sidebarSize.y;
            if (overUi)
                return DefWindowProc(hwnd, msg, wParam, lParam);

            if (msg == WM_POINTERDOWN) {
                for (auto& c : g_touch) {
                    if (!c.live) {
                        c = { pid, px, py, true };
                        ++g_touchN;
                        TouchUpdateBaselines();
                        if (g_touchN == 1) {
                            // First touch — start hold detection.
                            g_touchHoldSecs   = 0.f;
                            g_touchHoldDragPx = 0.f;
                            g_touchHoldX      = px;
                            g_touchHoldY      = py;
                        } else {
                            // Second finger cancels any pending hold.
                            g_touchHoldSecs = 0.f;
                        }
                        break;
                    }
                }
            } else if (msg == WM_POINTERUP) {
                for (auto& c : g_touch) {
                    if (c.live && c.id == pid) {
                        c.live = false;
                        --g_touchN;
                        TouchUpdateBaselines();
                        if (g_touchN == 0)
                            g_touchHoldSecs = 0.f;
                        break;
                    }
                }
            } else {
                int slot = -1;
                for (int i = 0; i < 2; ++i)
                    if (g_touch[i].live && g_touch[i].id == pid) { slot = i; break; }
                if (slot < 0) return 0;

                g_touch[slot].x = px;
                g_touch[slot].y = py;

                if (g_touchN == 1) {
                    const float hdx = px - g_touchPrevCX;
                    const float hdy = py - g_touchPrevCY;
                    g_camera.OrbitDelta(hdx, hdy);
                    g_touchHoldDragPx += sqrtf(hdx * hdx + hdy * hdy);
                    g_touchPrevCX = px;
                    g_touchPrevCY = py;
                } else if (g_touchN == 2) {
                    const float cx  = (g_touch[0].x + g_touch[1].x) * 0.5f;
                    const float cy  = (g_touch[0].y + g_touch[1].y) * 0.5f;
                    g_camera.PanDelta(cx - g_touchPrevCX, cy - g_touchPrevCY);
                    const float ddx = g_touch[1].x - g_touch[0].x;
                    const float ddy = g_touch[1].y - g_touch[0].y;
                    const float d   = std::max(1.f, sqrtf(ddx * ddx + ddy * ddy));
                    if (g_touchPrevD > 1.f) {
                        const float notches = -logf(d / g_touchPrevD) / logf(0.85f);
                        g_camera.ZoomDelta(notches);
                    }
                    g_touchPrevCX = cx;
                    g_touchPrevCY = cy;
                    g_touchPrevD  = d;
                }
            }
            return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    // ── LMB double-click: pivot on terrain AABB ───────────────────────────
    case WM_LBUTTONDBLCLK:
        if (!ImGui::GetIO().WantCaptureMouse && g_tilesReady) {
            g_lmbDown      = true;
            g_lastMousePos = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetCapture(hwnd);
            const float vpW  = static_cast<float>(g_renderer.Width());
            const float vpH  = static_cast<float>(g_renderer.Height());
            const auto  view = g_camera.ViewMatrix();
            const auto  proj = g_camera.ProjMatrix(vpW / vpH);
            DirectX::XMFLOAT3 rayOrigin, rayDir;
            UnprojectRay(static_cast<float>(GET_X_LPARAM(lParam)),
                         static_cast<float>(GET_Y_LPARAM(lParam)),
                         vpW, vpH, view, proj, rayOrigin, rayDir);
            DirectX::XMFLOAT3 hit;
            if (g_tileGrid.RayCast(rayOrigin, rayDir, hit))
                g_camera.SetPivot(hit.x, hit.y, hit.z);
        }
        return 0;

    // ── Keyboard shortcuts ────────────────────────────────────────────────
    case WM_KEYDOWN:
        if (!ImGui::GetIO().WantCaptureKeyboard) {
            switch (static_cast<int>(wParam)) {
#ifdef _DEBUG
            case 0xC0:  // VK_OEM_3 — backtick / grave accent
                g_pickDbgVisible = !g_pickDbgVisible;
                break;
#endif
            case 'R':
                g_camera.SetPivot(g_defaultPivot.x, g_defaultPivot.y, g_defaultPivot.z);
                g_camera.SetSpherical(g_defaultRadius, 270.0f, 33.7f);
                break;
            case 'G':
                g_gpsMode = !g_gpsMode;
                if (g_gpsMode) g_gpsNeedElevLookup = true;
                break;
            case 'T': g_showTerrain = !g_showTerrain;  break;
            case 'F': g_freshnessOverlay = !g_freshnessOverlay; break;
            case VK_ESCAPE: g_sidebarOpen = !g_sidebarOpen; break;
            }
        }
        return 0;

    // ── Mouse ─────────────────────────────────────────────────────────────
    case WM_LBUTTONDOWN:
        if (!ImGui::GetIO().WantCaptureMouse) {
            g_lmbDown      = true;
            g_lmbHeldSecs  = 0.f;
            g_lmbDragAccum = 0.f;
            g_lmbDownX     = static_cast<float>(GET_X_LPARAM(lParam));
            g_lmbDownY     = static_cast<float>(GET_Y_LPARAM(lParam));
            g_lastMousePos = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetCapture(hwnd);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_lmbDown) {
            g_lmbDown      = false;
            g_lmbHeldSecs  = 0.f;
            g_lmbDragAccum = 0.f;
            if (!g_rmbDown && !g_mmbDown) ReleaseCapture();
        }
        return 0;

    case WM_RBUTTONDOWN:
        if (!ImGui::GetIO().WantCaptureMouse) {
            g_rmbDown = true;
            g_lastMousePos = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetCapture(hwnd);
        }
        return 0;

    case WM_RBUTTONUP:
        if (g_rmbDown) {
            g_rmbDown = false;
            if (!g_lmbDown && !g_mmbDown) ReleaseCapture();
        }
        return 0;

    case WM_MBUTTONDOWN:
        if (!ImGui::GetIO().WantCaptureMouse) {
            g_mmbDown = true;
            g_lastMousePos = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetCapture(hwnd);
        }
        return 0;

    case WM_MBUTTONUP:
        if (g_mmbDown) {
            g_mmbDown = false;
            if (!g_lmbDown && !g_rmbDown) ReleaseCapture();
        }
        return 0;

    case WM_MOUSEMOVE:
        if (g_lmbDown || g_rmbDown || g_mmbDown) {
            const POINT cur = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const float dx  = static_cast<float>(cur.x - g_lastMousePos.x);
            const float dy  = static_cast<float>(cur.y - g_lastMousePos.y);
            if (g_lmbDown) {
                g_camera.OrbitDelta(dx, dy);
                g_lmbDragAccum += sqrtf(dx * dx + dy * dy);
            } else if (g_rmbDown || g_mmbDown) {
                g_camera.PanDelta(dx, dy);
            }
            g_lastMousePos = cur;
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (!ImGui::GetIO().WantCaptureMouse) {
            const float notches =
                static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            g_camera.ZoomDelta(notches);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ── Origin alignment ──────────────────────────────────────────────────────────
// Applies per-design origin offsets relative to terrain scene origin.
// Must be called after terrain origin is set and after each design set is loaded.
// DesignPass/LineworkPass world matrices are set at draw time (per design set).
static void ApplyOriginAlignment()
{
    for (auto& ds : g_designSets) {
        if (ds.grid) {
            const float dx = ds.surfOrigin[0] - g_sceneOrigin[0];
            const float dy = ds.surfOrigin[1] - g_sceneOrigin[1];
            const float dz = ds.surfOrigin[2] - g_sceneOrigin[2];
            ds.grid->ApplyOriginOffset(dx, dy, dz);
        }
    }
    // CRS auto-suggest from terrain easting (AMG full-easting / 1e6 gives zone).
    const int suggested = static_cast<int>(g_sceneOrigin[0]) / 1'000'000;
    if (suggested >= 49 && suggested <= 56)
        g_zoneSuggest = suggested;
}

// ── Terrain setup (synchronous — startup only) ────────────────────────────────

static void LoadTerrain()
{
    g_terrainDxfPath = ScanTerrainFolder(g_terrainFolder);
    if (g_terrainDxfPath.empty()) {
        if (!g_terrainFolder.empty())
            g_statusMsg = "No DXF found in terrain folder.";
        else
            g_statusMsg = "No terrain folder — use Files panel to select one.";
        return;
    }

    const fs::path dxfPath = g_terrainDxfPath;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path cacheDir = fs::path(tmp) / L"TerrainViewer" / L"tiles" / dxfPath.stem();

    try {
        std::atomic<float> progress{0.0f};
        const auto result = dxf::parseToCache(dxfPath, cacheDir, progress);

        if (result.faceCount == 0 && result.tileCount == 0) {
            g_statusMsg = "Parse produced no tiles.";
            return;
        }
        if (!g_tileGrid.Init(cacheDir)) {
            g_statusMsg = "TileGrid::Init failed — no tiles in cache.";
            return;
        }

        g_tileGrid.SetBudget(&g_budget);
        g_sceneOrigin           = result.origin;
        g_terrainCacheDir       = cacheDir;
        g_terrainFreshnessColor = ComputeFreshnessColor(cacheDir);
        g_tilesReady            = true;
        g_statusMsg = std::to_string(g_tileGrid.TileCount()) + " tiles  "
                    + std::to_string(result.faceCount)       + " faces";

        const auto  centre = g_tileGrid.SceneCentre();
        const float radius = g_tileGrid.SceneRadius();
        g_camera.SetPivot(centre.x, centre.y, centre.z);
        g_camera.SetSpherical(radius, 270.0f, 33.7f);
        g_defaultPivot  = centre;
        g_defaultRadius = radius;
    }
    catch (const std::exception& ex) {
        g_statusMsg  = std::string("Terrain load error: ") + ex.what();
        g_toast      = g_statusMsg;
        g_toastTimer = 6.f;
    }
}

// ── Apply one DesignLoadResult to a DesignSet (main thread — needs DX11) ──────
static void ApplyDesignResult(DesignSet& ds, DesignLoadResult& dr)
{
    // Surface
    if (!dr.surfaceError.empty()) {
        ds.msg = dr.surfaceError;
        return;
    }
    auto& sr = dr.surfaceResult;
    if (sr.faceCount > 0 || sr.tileCount > 0) {
        auto grid = std::make_unique<TileGrid>();
        if (grid->Init(dr.surfaceCacheDir)) {
            grid->SetBudget(&g_budget);
            grid->SetBudgetIndexBase(ds.budgetBase);
            grid->SetForceLod0(true);  // design surfaces always full-detail — no LOD switching
            ds.surfOrigin   = sr.origin;
            ds.surfParsed   = true;
            ds.surfCacheDir = dr.surfaceCacheDir;

            // Apply origin offset now (terrain origin must already be set)
            const float dx = ds.surfOrigin[0] - g_sceneOrigin[0];
            const float dy = ds.surfOrigin[1] - g_sceneOrigin[1];
            const float dz = ds.surfOrigin[2] - g_sceneOrigin[2];
            grid->ApplyOriginOffset(dx, dy, dz);

            ds.grid = std::move(grid);
            ds.msg  = std::to_string(ds.grid->TileCount()) + " tiles  "
                    + std::to_string(sr.faceCount) + " faces";
        } else {
            ds.msg = "TileGrid::Init failed for " + ds.name;
        }
    }

    // Linework — polylines come from either the linework parse or the surface parse
    // (single-file case: sr already has polylines)
    auto& polylines = ds.single_file ? sr.polylines : dr.lineworkResult.polylines;
    if (!dr.lineworkError.empty()) {
        ds.msg += "  |  linework: " + dr.lineworkError;
    } else if (!polylines.empty()) {
        auto lwMesh = std::make_unique<LineworkMesh>();
        if (lwMesh->Load(g_renderer.Device(), polylines)) {
            ds.polylines = polylines;    // cache for re-load without re-parse
            ds.lwOrigin  = ds.single_file ? sr.origin : dr.lineworkResult.origin;
            ds.lwCacheDir = dr.lineworkCacheDir;
            ds.lwMesh    = std::move(lwMesh);
            ds.msg += "  " + std::to_string(ds.lwMesh->SegmentCount()) + " segs";
        }
    }
}

// ── Apply load results on the main thread (DX11 + state, after parse thread) ──
static void ApplyLoadResults()
{
    if (g_loadKind == LoadKind::Full) {
        // ── Terrain ───────────────────────────────────────────────────────
        if (!g_loadData.terrainError.empty()) {
            g_statusMsg  = g_loadData.terrainError;
            g_toast      = g_statusMsg;
            g_toastTimer = 6.f;
        } else {
            auto& r = g_loadData.terrainResult;
            if (r.faceCount == 0 && r.tileCount == 0) {
                g_statusMsg = "Parse produced no tiles.";
            } else if (!g_tileGrid.Init(g_loadData.terrainCacheDir)) {
                g_statusMsg = "TileGrid::Init failed — no tiles in cache.";
            } else {
                g_tileGrid.SetBudget(&g_budget);
                g_sceneOrigin           = r.origin;
                g_terrainCacheDir       = g_loadData.terrainCacheDir;
                g_terrainFreshnessColor = ComputeFreshnessColor(g_loadData.terrainCacheDir);
                g_tilesReady            = true;
                g_statusMsg = std::to_string(g_tileGrid.TileCount()) + " tiles  "
                            + std::to_string(r.faceCount)             + " faces";
                const auto  centre = g_tileGrid.SceneCentre();
                const float radius = g_tileGrid.SceneRadius();
                g_camera.SetPivot(centre.x, centre.y, centre.z);
                g_camera.SetSpherical(radius, 270.0f, 33.7f);
                g_defaultPivot  = centre;
                g_defaultRadius = radius;
            }
        }

        // ── Design sets (indexed parallel to g_designSets) ────────────────
        for (int i = 0; i < static_cast<int>(g_loadData.designResults.size()); ++i) {
            // Match by name — find the DesignSet with this name
            const std::string& dsName = g_loadData.designResults[i].name;
            auto it = std::find_if(g_designSets.begin(), g_designSets.end(),
                [&dsName](const DesignSet& ds){ return ds.name == dsName; });
            if (it == g_designSets.end()) continue;
            ApplyDesignResult(*it, g_loadData.designResults[i]);
        }
    } else {
        // ── DesignOnly: apply just one design set ─────────────────────────
        if (g_loadDesignIdx >= 0 &&
            g_loadDesignIdx < static_cast<int>(g_designSets.size()) &&
            !g_loadData.designResults.empty())
        {
            ApplyDesignResult(g_designSets[g_loadDesignIdx],
                              g_loadData.designResults[0]);
        }
    }

    // Free cached polylines from load results (they are now in DesignSet::polylines)
    for (auto& dr : g_loadData.designResults) {
        dr.surfaceResult.polylines.clear();
        dr.lineworkResult.polylines.clear();
    }

    g_coordPrevX        = std::numeric_limits<float>::quiet_NaN();
    g_gpsNeedElevLookup = true;
    RecreateGpsSource();
}

// ── Full reload — resets GPU state, kicks off background parse thread ──────────
// Triggered when terrain/design folder changes or on startup.
static void FullReload()
{
    if (g_parseBusy.load()) return;   // already loading — ignore
    if (g_loaderThread.joinable())
        g_loaderThread.join();

    // Evict all design set grids (properly untracks from budget before reset)
    for (auto& ds : g_designSets) {
        if (ds.grid)   { ds.grid->EvictAll();   ds.grid.reset();   }
        if (ds.lwMesh) {                         ds.lwMesh.reset(); }
    }

    // Reset terrain grid and budget
    g_tileGrid   = TileGrid{};
    g_budget     = GpuBudget(static_cast<size_t>(terrain::GPU_BUDGET_MB) * 1024 * 1024);

    g_tilesReady  = false;
    g_sceneOrigin = {0.f, 0.f, 0.f};
    g_statusMsg.clear();

    g_loadData       = LoadData{};
    g_loadKind       = LoadKind::Full;
    g_loadDesignIdx  = -1;

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path base = fs::path(tmp) / L"TerrainViewer" / L"tiles";

    // Terrain cache dir
    const std::string tp = ScanTerrainFolder(g_terrainFolder);
    g_terrainDxfPath = tp;
    g_loadData.terrainCacheDir = tp.empty() ? fs::path{} : base / fs::path(tp).stem();

    // Snapshot visible design sets for the thread
    struct DesignSnap {
        std::string name;
        std::string surface_path;
        std::string linework_path;
        bool single_file;
        fs::path surfCacheDir;
        fs::path lwCacheDir;
    };
    std::vector<DesignSnap> snaps;
    for (int i = 0; i < static_cast<int>(g_designSets.size()); ++i) {
        auto& ds = g_designSets[i];
        ds.budgetBase = 100000 * (i + 1);
        ds.msg.clear();
        if (!ds.visible) continue;
        DesignSnap s;
        s.name          = ds.name;
        s.surface_path  = ds.surface_path;
        s.linework_path = ds.linework_path;
        s.single_file   = ds.single_file;
        s.surfCacheDir  = ds.surface_path.empty()
            ? fs::path{} : base / fs::path(ds.surface_path).stem();
        s.lwCacheDir    = (ds.linework_path.empty() || ds.single_file)
            ? fs::path{} : base / fs::path(ds.linework_path).stem();
        ds.surfCacheDir = s.surfCacheDir;
        ds.lwCacheDir   = s.lwCacheDir;
        g_loadData.designResults.push_back(DesignLoadResult{
            {}, {}, {}, {}, s.surfCacheDir, s.lwCacheDir, s.single_file, s.name });
        snaps.push_back(std::move(s));
    }

    g_parseProgress    = 0.f;
    g_parseCurrentFile = 0;
    g_parseBusy        = true;

    g_loaderThread = std::thread([tp, snaps]()
    {
        // Phase 0: terrain
        g_parseCurrentFile = 0;
        g_parseProgress    = 0.f;
        if (!tp.empty()) {
            if (!fs::exists(tp)) {
                g_loadData.terrainError = "terrain DXF not found: " + tp;
            } else {
                try {
                    g_loadData.terrainResult = dxf::parseToCache(
                        tp, g_loadData.terrainCacheDir, g_parseProgress);
                } catch (const std::exception& ex) {
                    g_loadData.terrainError = std::string("Terrain error: ") + ex.what();
                }
            }
        }

        // Phases 1+: design sets
        for (int i = 0; i < static_cast<int>(snaps.size()); ++i) {
            g_parseCurrentFile = 1 + i;
            g_parseProgress    = 0.f;
            const auto& s   = snaps[i];
            auto& dr        = g_loadData.designResults[i];

            // Surface
            if (!s.surface_path.empty()) {
                if (!fs::exists(s.surface_path)) {
                    dr.surfaceError = "not found: " + s.surface_path;
                } else {
                    try {
                        dr.surfaceResult = dxf::parseToCache(
                            s.surface_path, s.surfCacheDir, g_parseProgress);
                    } catch (const std::exception& ex) {
                        dr.surfaceError = std::string("Surface error: ") + ex.what();
                    }
                }
            }

            // Linework: for paired files parse _CAD separately;
            // for single_file the polylines are already in surfaceResult.
            if (!s.single_file && !s.linework_path.empty()) {
                if (!fs::exists(s.linework_path)) {
                    dr.lineworkError = "not found: " + s.linework_path;
                } else {
                    try {
                        std::atomic<float> lwProg{0.f};
                        dxf::clearTileCache(s.lwCacheDir);
                        dr.lineworkResult = dxf::parseToCache(
                            s.linework_path, s.lwCacheDir, lwProg);
                    } catch (const std::exception& ex) {
                        dr.lineworkError = std::string("Linework error: ") + ex.what();
                    }
                }
            }
        }

        g_loadApplyPending.store(true);
        g_parseBusy.store(false);
    });
}

// ── Single design set (re)load — for checkbox-enable of a never-parsed design ─
// If the surface cache already has tiles, skips parsing and inits TileGrid fast.
// If cache is empty, parses the DXF async and shows progress overlay.
static void LoadDesignSetAsync(int idx)
{
    if (g_parseBusy.load()) return;
    if (g_loaderThread.joinable()) g_loaderThread.join();

    auto& ds = g_designSets[idx];
    ds.msg.clear();

    // Fast path: cache already exists — init TileGrid synchronously.
    if (ds.surfParsed && !ds.surfCacheDir.empty()) {
        auto grid = std::make_unique<TileGrid>();
        if (grid->Init(ds.surfCacheDir)) {
            grid->SetBudget(&g_budget);
            grid->SetBudgetIndexBase(ds.budgetBase);
            grid->SetForceLod0(true);  // design surfaces always full-detail — no LOD switching
            const float dx = ds.surfOrigin[0] - g_sceneOrigin[0];
            const float dy = ds.surfOrigin[1] - g_sceneOrigin[1];
            const float dz = ds.surfOrigin[2] - g_sceneOrigin[2];
            grid->ApplyOriginOffset(dx, dy, dz);
            ds.grid = std::move(grid);
            ds.msg  = std::to_string(ds.grid->TileCount()) + " tiles (cached)";
        }
        // Restore linework from cached polylines
        if (!ds.polylines.empty()) {
            auto lwm = std::make_unique<LineworkMesh>();
            if (lwm->Load(g_renderer.Device(), ds.polylines))
                ds.lwMesh = std::move(lwm);
        }
        return;
    }

    // Slow path: parse DXF async
    g_loadData       = LoadData{};
    g_loadKind       = LoadKind::DesignOnly;
    g_loadDesignIdx  = idx;
    g_loadData.designResults.push_back(DesignLoadResult{
        {}, {}, {}, {}, ds.surfCacheDir, ds.lwCacheDir, ds.single_file, ds.name });

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path base  = fs::path(tmp) / L"TerrainViewer" / L"tiles";
    if (ds.surfCacheDir.empty() && !ds.surface_path.empty())
        g_loadData.designResults[0].surfaceCacheDir = ds.surfCacheDir =
            base / fs::path(ds.surface_path).stem();
    if (!ds.single_file && ds.lwCacheDir.empty() && !ds.linework_path.empty())
        g_loadData.designResults[0].lineworkCacheDir = ds.lwCacheDir =
            base / fs::path(ds.linework_path).stem();

    const std::string sp = ds.surface_path;
    const std::string lp = ds.linework_path;
    const bool sf        = ds.single_file;
    const fs::path scd   = ds.surfCacheDir;
    const fs::path lcd   = ds.lwCacheDir;

    g_parseProgress    = 0.f;
    g_parseCurrentFile = 1;
    g_parseBusy        = true;

    g_loaderThread = std::thread([sp, lp, sf, scd, lcd]()
    {
        auto& dr = g_loadData.designResults[0];

        if (!sp.empty()) {
            if (!fs::exists(sp)) {
                dr.surfaceError = "not found: " + sp;
            } else {
                try {
                    dr.surfaceResult = dxf::parseToCache(sp, scd, g_parseProgress);
                } catch (const std::exception& ex) {
                    dr.surfaceError = std::string("Surface error: ") + ex.what();
                }
            }
        }
        if (!sf && !lp.empty()) {
            if (!fs::exists(lp)) {
                dr.lineworkError = "not found: " + lp;
            } else {
                try {
                    std::atomic<float> lwProg{0.f};
                    dxf::clearTileCache(lcd);
                    dr.lineworkResult = dxf::parseToCache(lp, lcd, lwProg);
                } catch (const std::exception& ex) {
                    dr.lineworkError = std::string("Linework error: ") + ex.what();
                }
            }
        }

        g_loadApplyPending.store(true);
        g_parseBusy.store(false);
    });
}

// ── Entry point ───────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    (void)nCmdShow;   // F4 overrides show state — OS hint no longer used.
    // COM required for IFileOpenDialog.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // ── 1. Load session (before window creation so we get saved position) ──
    g_sessionPath = app::Session::DefaultPath();
    {
        std::string toastMsg;
        g_session.Load(g_sessionPath, toastMsg);
        if (!toastMsg.empty()) {
            g_toast      = toastMsg;
            g_toastTimer = 5.f;
        }
    }

    // ── 2. Resolve folder paths from session ──────────────────────────────────
    {
        auto pickDir = [](const std::string& s) -> std::string {
            return (!s.empty() && fs::is_directory(s)) ? s : std::string{};
        };
        g_terrainFolder = pickDir(g_session.data.terrain_folder);
        g_designsFolder = pickDir(g_session.data.designs_folder);
        g_showTerrain   = g_session.data.terrain_visible;
    }

    // ── 2b. Scan design folder and restore active design sets ──────────────
    if (!g_designsFolder.empty()) {
        g_designSets = ScanDesignFolder(g_designsFolder);
        // Restore active/visible state from session
        for (auto& ds : g_designSets) {
            // default: not visible (user must explicitly check or it was saved)
            ds.visible = false;
            ds.budgetBase = 100000 * (static_cast<int>(&ds - g_designSets.data()) + 1);
        }
        for (const auto& ad : g_session.data.active_designs) {
            auto it = std::find_if(g_designSets.begin(), g_designSets.end(),
                [&ad](const DesignSet& ds){ return ds.name == ad.name; });
            if (it != g_designSets.end()) it->visible = ad.visible;
        }
    }

    // ── 3. Window ──────────────────────────────────────────────────────────
    WNDCLASSEX wc{};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = _T("TerrainViewerWnd");
    RegisterClassEx(&wc);

    {
        const auto& wd = g_session.data;
        const int wX = (g_session.loaded && wd.window_x != INT_MIN) ? wd.window_x : CW_USEDEFAULT;
        const int wY = (g_session.loaded && wd.window_y != INT_MIN) ? wd.window_y : CW_USEDEFAULT;
        const int wW = g_session.loaded ? wd.window_width  : 1280;
        const int wH = g_session.loaded ? wd.window_height : 720;

        g_hwnd = CreateWindowEx(
            0,
            _T("TerrainViewerWnd"),
            _T("Terrain Viewer"),
            WS_OVERLAPPEDWINDOW,
            wX, wY, wW, wH,
            nullptr, nullptr, hInstance, nullptr);
    }
    if (!g_hwnd) return 1;

    // ── 4. Renderer ────────────────────────────────────────────────────────
    RECT rc{};
    GetClientRect(g_hwnd, &rc);

    if (!g_renderer.Init(g_hwnd, rc.right - rc.left, rc.bottom - rc.top)) {
        MessageBox(g_hwnd, _T("D3D11 initialisation failed."), _T("Error"),
                   MB_OK | MB_ICONERROR);
        return 1;
    }

    // (Terrain and design sets load async via FullReload after ImGui init — step 9b.)

    // ── 6. Render passes ───────────────────────────────────────────────────
    if (!g_terrainPass.Init(g_renderer.Device()))
        MessageBox(g_hwnd, _T("TerrainPass::Init failed."), _T("Error"), MB_OK | MB_ICONERROR);
    if (!g_designPass.Init(g_renderer.Device()))
        MessageBox(g_hwnd, _T("DesignPass::Init failed."), _T("Error"), MB_OK | MB_ICONERROR);
    if (!g_lineworkPass.Init(g_renderer.Device()))
        MessageBox(g_hwnd, _T("LineworkPass::Init failed."), _T("Error"), MB_OK | MB_ICONERROR);

    // ── 7. Origin alignment (no-op at startup; applied in ApplyLoadResults) ─

    // ── 7b. PROJ data search path (must be set before first PROJ call) ─────
    // ApplySession() → RecreateGpsSource() initialises PROJ via CoordTransform.
    // Point PROJ at the proj_data/ directory co-located with the exe so the app
    // runs self-contained on any machine without a system PROJ installation.
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        const fs::path projData = fs::path(exePath).parent_path() / L"proj_data";
        const std::string projDataStr = projData.string();
        SetEnvironmentVariableA("PROJ_DATA", projDataStr.c_str());
    }

    // ── 8. Apply session (visibility, opacity, CRS, GPS config, camera) ────
    ApplySession();   // also calls RecreateGpsSource()

    // ── 9. Dear ImGui ──────────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ApplyImGuiStyle();   // F3: dark theme + accent colours + Segoe UI font
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_renderer.Device(), g_renderer.Context());

    // F4: first launch → maximized. Subsequent launches → restore saved state.
    {
        const bool wantMax = !g_session.loaded || g_session.data.window_maximized;
        ShowWindow(g_hwnd, wantMax ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    }
    UpdateWindow(g_hwnd);

    // ── 9b. Kick off async load for terrain + all active designs ───────────
    // FullReload always runs on startup so the progress overlay shows during parse.
    if (!g_terrainFolder.empty() || !g_designSets.empty())
        FullReload();

    // ── 10. Auto-save background thread ────────────────────────────────────
    g_autoSaveRunning = true;
    g_autoSaveThread  = std::thread([] {
        while (g_autoSaveRunning) {
            const int slices = terrain::SESSION_AUTOSAVE_SECONDS * 2;
            for (int i = 0; i < slices && g_autoSaveRunning.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (g_autoSaveRunning)
                g_sessionSavePending = true;
        }
    });

    // ── Sidebar layout constants ────────────────────────────────────────────
    static constexpr float kSidebarW = 320.f;  // sidebar width when open
    static constexpr float kTabW     =  28.f;  // collapsed tab strip width
    static constexpr float kFPx      =   8.f;  // FramePadding.x for touch targets
    static constexpr float kFPy      =  15.f;  // FramePadding.y → ~43px item height

    // ── 11. Message loop ───────────────────────────────────────────────────
    MSG msg{};
    while (g_running)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                g_running = false;
        }

        if (!g_running) break;

        // ── Per-frame camera matrices ──────────────────────────────────────
        const float aspect = static_cast<float>(g_renderer.Width()) /
                             static_cast<float>(g_renderer.Height());
        const auto view = g_camera.ViewMatrix();
        const auto proj = g_camera.ProjMatrix(aspect);

        // ── Tile visibility + streaming ────────────────────────────────────
        const auto camPos = g_camera.Position();
        {
            const bool anyDesign = std::any_of(g_designSets.begin(), g_designSets.end(),
                [](const DesignSet& ds){ return ds.visible && ds.grid != nullptr; });
            if (g_tilesReady || anyDesign) {
                const auto planes = ExtractFrustumPlanes(view, proj);
                if (g_tilesReady) {
                    g_tileGrid.UpdateVisibility(planes, camPos);
                    g_tileGrid.FlushLoads(g_renderer.Device(),
                                          terrain::MAX_TILE_LOADS_PER_FRAME);
                }
                for (auto& ds : g_designSets) {
                    if (!ds.visible || !ds.grid) continue;
                    ds.grid->UpdateVisibility(planes, camPos);
                    ds.grid->FlushLoads(g_renderer.Device(),
                                        terrain::MAX_TILE_LOADS_PER_FRAME);
                }
            }
        }

        // ── GPS camera mode ───────────────────────────────────────────────
        if (g_gpsMode && g_gpsSrc) {
            const gps::ScenePosition gpsPos = g_gpsSrc->poll();
            if (gpsPos.valid) {
                g_gpsLastKnown = gpsPos;

                const float dex      = gpsPos.x - g_gpsPrevElevX;
                const float dey      = gpsPos.y - g_gpsPrevElevY;
                const float moveSq   = dex * dex + dey * dey;
                const float threshSq = terrain::GPS_MOVE_THRESHOLD_M
                                     * terrain::GPS_MOVE_THRESHOLD_M;
                if (g_gpsNeedElevLookup || moveSq > threshSq) {
                    if (g_tilesReady) {
                        const DirectX::XMFLOAT3 rayO = { gpsPos.x, gpsPos.y, 9999.0f };
                        const DirectX::XMFLOAT3 rayD = { 0.0f, 0.0f, -1.0f };
                        DirectX::XMFLOAT3 hit;
                        if (g_tileGrid.RayCast(rayO, rayD, hit))
                            g_gpsCachedElev = hit.z;
                    }
                    g_gpsPrevElevX      = gpsPos.x;
                    g_gpsPrevElevY      = gpsPos.y;
                    g_gpsNeedElevLookup = false;
                }

                const float eyeZ       = g_gpsCachedElev + g_gpsHeightOffset;
                const float headingRad = gpsPos.heading * DirectX::XM_PI / 180.0f;
                const float lookDist   = terrain::GPS_VIEW_DISTANCE_M;
                const float lookX      = sinf(headingRad) * lookDist;
                const float lookY      = cosf(headingRad) * lookDist;
                const float azimuth    = fmodf(270.0f - gpsPos.heading + 360.0f, 360.0f);
                g_camera.SetPivot(gpsPos.x + lookX, gpsPos.y + lookY, eyeZ);
                g_camera.SetSpherical(lookDist, azimuth, 0.0f);
            }
        }

        // ── Coordinate readout ─────────────────────────────────────────────
        {
            float sceneX, sceneY, sceneZ;
            if (g_gpsMode && g_gpsLastKnown.valid) {
                sceneX = g_gpsLastKnown.x;
                sceneY = g_gpsLastKnown.y;
                sceneZ = g_gpsLastKnown.z;
            } else {
                const auto piv = g_camera.Pivot();
                sceneX = piv.x;
                sceneY = piv.y;
                sceneZ = piv.z;
            }
            const float ddx   = sceneX - g_coordPrevX;
            const float ddy   = sceneY - g_coordPrevY;
            const bool  moved = std::isnan(g_coordPrevX) || (ddx * ddx + ddy * ddy > 0.01f);
            if (moved) {
                try {
                    g_coordMga   = gps::sceneToMga(sceneX, sceneY, sceneZ, g_sceneOrigin);
                    g_coordWgs   = gps::mgaToWgs(g_coordMga.easting, g_coordMga.northing,
                                                  g_coordMga.elev, g_crsZone, g_crsDatum);
                    g_coordValid = true;
                } catch (...) {
                    g_coordValid = false;
                }
                g_coordPrevX = sceneX;
                g_coordPrevY = sceneY;
            }
        }

        // ── Auto-save ─────────────────────────────────────────────────────
        if (g_sessionSavePending.exchange(false)) {
            GatherSession();
            g_session.Save(g_sessionPath);
        }

        // ── Apply async load results (main thread — needs DX11 device) ─────
        if (g_loadApplyPending.exchange(false)) {
            if (g_loaderThread.joinable())
                g_loaderThread.join();
            ApplyLoadResults();
        }

        // ── ImGui ─────────────────────────────────────────────────────────
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ── Viewport size (used by hold detection and pick helpers) ────────
        const float vpW = static_cast<float>(g_renderer.Width());
        const float vpH = static_cast<float>(g_renderer.Height());

        // ── Click-and-hold coord pick timer ───────────────────────────────
        // Matrices are captured on the FIRST frame of the hold so the unprojected
        // ray uses the camera orientation that matches the click screen position,
        // not the potentially-orbited camera 0.5s later.
        if (g_lmbDown && !g_pickActive && !ImGui::GetIO().WantCaptureMouse) {
            if (g_lmbHeldSecs == 0.f) { g_pickView = view; g_pickProj = proj; }
            g_lmbHeldSecs += io.DeltaTime;
            if (g_lmbHeldSecs > 0.5f && g_lmbDragAccum < 8.f)
                TriggerSurfacePick(g_lmbDownX, g_lmbDownY, vpW, vpH, g_pickView, g_pickProj);
        }
        if (g_touchN == 1 && !g_pickActive) {
            if (g_touchHoldSecs == 0.f) { g_pickView = view; g_pickProj = proj; }
            g_touchHoldSecs += io.DeltaTime;
            if (g_touchHoldSecs > 0.5f && g_touchHoldDragPx < 8.f)
                TriggerSurfacePick(g_touchHoldX, g_touchHoldY, vpW, vpH, g_pickView, g_pickProj);
        }

        // ── Parse progress overlay ────────────────────────────────────────
        if (g_parseBusy.load()) {
            const int   fileIdx = g_parseCurrentFile.load();
            const float prog    = g_parseProgress.load();
            // fileIdx 0 = terrain, 1+ = design set index (fileIdx-1)
            char labelBuf[64] = {};
            if (fileIdx == 0) {
                snprintf(labelBuf, sizeof(labelBuf), "terrain");
            } else if (fileIdx - 1 < static_cast<int>(g_designSets.size())) {
                snprintf(labelBuf, sizeof(labelBuf), "design: %s",
                         g_designSets[fileIdx - 1].name.c_str());
            } else {
                snprintf(labelBuf, sizeof(labelBuf), "design");
            }
            const char* label = labelBuf;

            const float overlayW = 380.f;
            ImGui::SetNextWindowPos(
                { (io.DisplaySize.x - overlayW) * 0.5f,
                  io.DisplaySize.y * 0.5f - 40.f },
                ImGuiCond_Always);
            ImGui::SetNextWindowSize({ overlayW, 0.f }, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.90f);
            ImGui::Begin("##loadprogress", nullptr,
                ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoInputs  |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove    |
                ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoSavedSettings);
            if (prog < 1.0f) {
                char buf[48];
                snprintf(buf, sizeof(buf), "%.0f%%", prog * 100.f);
                ImGui::ProgressBar(prog, { -1.f, 0.f }, buf);
                ImGui::Text("Loading %s...", label);
            } else {
                // LOD generation phase — indeterminate marquee
                ImGui::ProgressBar(-1.f * (float)ImGui::GetTime(), { -1.f, 0.f }, "");
                ImGui::Text("Generating LOD meshes — %s", label);
            }
            ImGui::End();
        }

        // ── Offline banner (F3: slim full-width top bar) ──────────────────
        {
            const auto [offHours, offMins] = GetOfflineTime();
            if (offHours >= terrain::OFFLINE_WARN_HOURS) {
                static constexpr float kBannerH = 24.f;
                ImGui::SetNextWindowPos({ 0.f, 0.f }, ImGuiCond_Always);
                ImGui::SetNextWindowSize({ io.DisplaySize.x, kBannerH }, ImGuiCond_Always);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{ 0.35f, 0.15f, 0.0f, 1.f });
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f, 3.f));
                ImGui::Begin("##offline", nullptr,
                    ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoInputs  |
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove    |
                    ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoSavedSettings);
                ImGui::TextColored({ 1.f, 0.8f, 0.2f, 1.f },
                    "Offline %dh %02dm -- terrain data may be outdated",
                    offHours, offMins);
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                ImGui::End();
            }
        }

        // ── LHS button bar (always shown — 4 touch-friendly buttons) ─────────
        {
            static constexpr float kBtnW    = 52.f;
            static constexpr float kBtnGapY =  4.f;  // between buttons
            // Compute actual button height from current font + kFPy so the window
            // is sized correctly regardless of whether Segoe UI or the fallback
            // default font is active.
            const float actualBtnH = ImGui::GetFontSize() + 2.f * kFPy;
            const float barH = 4.f * actualBtnH + 3.f * kBtnGapY + 8.f;  // +8 = 2×padding
            ImGui::SetNextWindowPos({ 8.f, 8.f }, ImGuiCond_Always);
            ImGui::SetNextWindowSize({ kBtnW, barH }, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.75f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 4.f));
            ImGui::Begin("##lhsbar", nullptr,
                ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoMove     |
                ImGuiWindowFlags_NoResize     | ImGuiWindowFlags_NoScrollbar|
                ImGuiWindowFlags_NoSavedSettings);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, kFPy));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(0.f, kBtnGapY));
            if (ImGui::Button("+##zin",  ImVec2(kBtnW - 8.f, 0.f))) g_camera.ZoomDelta( g_zoomStep);
            if (ImGui::Button("-##zout", ImVec2(kBtnW - 8.f, 0.f))) g_camera.ZoomDelta(-g_zoomStep);
            if (ImGui::Button("O##rst",  ImVec2(kBtnW - 8.f, 0.f))) {
                g_camera.SetPivot(g_defaultPivot.x, g_defaultPivot.y, g_defaultPivot.z);
                g_camera.SetSpherical(g_defaultRadius, 270.0f, 33.7f);
            }
            if (ImGui::Button("=##sb",   ImVec2(kBtnW - 8.f, 0.f))) g_sidebarOpen = true;
            ImGui::PopStyleVar(2);   // FramePadding + ItemSpacing
            ImGui::End();
            ImGui::PopStyleVar();    // WindowPadding
        }

        // ── Surface coord pick popup (F5) ────────────────────────────────
        // Stays open until user clicks outside or triggers a new pick.
        if (g_pickActive) {
            {
                const float pw = 280.f;
                // Anchor bottom-left corner 8 px from each edge.
                // Pivot (0, 1) = window's own bottom-left at the given position.
                ImGui::SetNextWindowPos(
                    { 8.f, io.DisplaySize.y - 8.f }, ImGuiCond_Always,
                    ImVec2(0.f, 1.f));
                ImGui::SetNextWindowSize({ pw, 0.f }, ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.95f);
                ImGui::Begin("##pickpopup", nullptr,
                    ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoMove    |
                    ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoSavedSettings);

                // Dismiss on quick click (release with < 8 px drag) outside popup.
                // Drag-to-orbit keeps the popup open; the hold-release that opened
                // the popup is absorbed via g_pickJustOpened.
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    if (g_pickJustOpened) {
                        g_pickJustOpened = false;   // swallow the hold-ending release
                    } else if (!ImGui::IsWindowHovered()) {
                        const ImVec2 d = ImGui::GetMouseDragDelta(
                            ImGuiMouseButton_Left, 0.0f);
                        if (d.x * d.x + d.y * d.y < 8.f * 8.f)
                            g_pickActive = false;
                    }
                }

                if (g_pickHasHit && !g_pickHits.empty()) {
                    // Helper: right-aligned value cell
                    auto addRow = [&](const char* lbl, const std::string& val,
                                      const ImVec4* col = nullptr)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(lbl);
                        ImGui::TableSetColumnIndex(1);
                        const float tw = ImGui::CalcTextSize(val.c_str()).x;
                        ImGui::SetCursorPosX(
                            ImGui::GetCursorPosX() +
                            ImGui::GetContentRegionAvail().x - tw);
                        if (col) ImGui::TextColored(*col, "%s", val.c_str());
                        else     ImGui::TextUnformatted(val.c_str());
                    };

                    // Two-column table: rows may repeat for each surface hit
                    if (ImGui::BeginTable("##pcoords", 2, ImGuiTableFlags_None)) {
                        ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthFixed, 62.f);
                        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);

                        for (int hi = 0; hi < static_cast<int>(g_pickHits.size()); ++hi) {
                            const auto& ph = g_pickHits[hi];
                            if (hi > 0) {
                                // Blank spacer row between surfaces
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::Spacing();
                            }
                            addRow("Surface", ph.label);
                            addRow("E", FormatThousands(ph.mga.easting));
                            addRow("N", FormatThousands(ph.mga.northing));
                            addRow("Z", FormatThousands(ph.mga.elev) + " m");
                            if (ph.hasCutFill) {
                                char cfBuf[32];
                                snprintf(cfBuf, sizeof(cfBuf), "%+.1f m", ph.cutFill);
                                const ImVec4 cfCol = (ph.cutFill >= 0.f) ? kGreenColor : kOrangeColor;
                                addRow("Cut/Fill", cfBuf, &cfCol);
                            }
                        }
                        // Data freshness stub
                        addRow("Data", "Local");

                        ImGui::EndTable();
                    }
                } else {
                    ImGui::TextColored(kOrangeColor,
                        g_tilesReady ? "No surface at pick point."
                                     : "No terrain loaded.");
                }

                ImGui::Spacing();
                ImGui::Separator();
                if (g_pickHasHit) {
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, kFPy));
                    if (ImGui::Button("Save to CSV##psave", ImVec2(-1.f, 0.f)))
                        SavePickToCsv();
                    ImGui::PopStyleVar();
                }

                ImGui::End();
            }
        }

        // ── Pick marker: crosshair projected onto the 3D surface ──────────
        // Drawn on the foreground drawlist (always on top) while pick popup is open.
        if (g_pickActive && g_pickHasHit) {
            using namespace DirectX;
            const XMVECTOR pt  = XMLoadFloat3(&g_pickHitScene);
            const XMVECTOR ndc = XMVector3TransformCoord(pt, view * proj);
            const float ndcZ   = XMVectorGetZ(ndc);
            if (ndcZ >= 0.f && ndcZ <= 1.f) {
                const float sx = (XMVectorGetX(ndc) *  0.5f + 0.5f) * vpW;
                const float sy = (XMVectorGetY(ndc) * -0.5f + 0.5f) * vpH;
                const float r  = 10.f;   // ring radius
                const float a  = 6.f;    // arm length (outside ring)
                auto* dl = ImGui::GetForegroundDrawList();
                // Shadow (offset 1 px)
                dl->AddCircle({ sx + 1.f, sy + 1.f }, r, IM_COL32(0, 0, 0, 130), 0, 2.f);
                // Cyan ring
                dl->AddCircle({ sx, sy }, r, IM_COL32(80, 240, 210, 230), 0, 2.f);
                // Crosshair arms — white with 1 px shadow
                for (int pass = 0; pass < 2; ++pass) {
                    const float o  = pass ? 0.f : 1.f;
                    const ImU32 c  = pass ? IM_COL32(255, 255, 255, 210)
                                          : IM_COL32(0,   0,   0,   120);
                    dl->AddLine({ sx - r - a + o, sy + o }, { sx - r + o, sy + o }, c, 1.5f);
                    dl->AddLine({ sx + r + o,     sy + o }, { sx + r + a + o, sy + o }, c, 1.5f);
                    dl->AddLine({ sx + o, sy - r - a + o }, { sx + o, sy - r + o }, c, 1.5f);
                    dl->AddLine({ sx + o, sy + r + o     }, { sx + o, sy + r + a + o }, c, 1.5f);
                }
                // Centre dot
                dl->AddCircleFilled({ sx, sy }, 2.5f, IM_COL32(80, 240, 210, 230));
            }
        }

        // ── Toast notification ─────────────────────────────────────────────
        if (g_toastTimer > 0.f) {
            g_toastTimer -= io.DeltaTime;
            const float toastW = 480.f;
            ImGui::SetNextWindowPos(
                { (io.DisplaySize.x - toastW) * 0.5f, 16.f }, ImGuiCond_Always);
            ImGui::SetNextWindowSize({ toastW, 0.f }, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.88f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{ 0.20f, 0.10f, 0.02f, 1.f });
            ImGui::Begin("##toast", nullptr,
                ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove   |
                ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextColored({ 1.f, 0.8f, 0.2f, 1.f }, "Session: %s", g_toast.c_str());
            ImGui::PopStyleColor();
            ImGui::End();
        }

#ifdef _DEBUG
        // ── Pick debug overlay (backtick to toggle) ────────────────────────
        if (g_pickDbgVisible && g_pickActive && g_pickHasHit) {
            ImGui::SetNextWindowPos({ 8.f, 220.f }, ImGuiCond_Always);
            ImGui::SetNextWindowSize({ 390.f, 0.f }, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.85f);
            ImGui::Begin("##pickdbg", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextColored({ 1.f, 1.f, 0.f, 1.f }, "Pick Debug  (` to toggle)");
            ImGui::Separator();
            ImGui::Text("ray_orig:  (%.2f, %.2f, %.2f)",
                g_pickDbg.rayOrigin.x, g_pickDbg.rayOrigin.y, g_pickDbg.rayOrigin.z);
            ImGui::Text("ray_dir:   (%.4f, %.4f, %.4f)",
                g_pickDbg.rayDir.x, g_pickDbg.rayDir.y, g_pickDbg.rayDir.z);
            ImGui::Text("hit_scene: (%.2f, %.2f, %.2f)",
                g_pickDbg.hitScene.x, g_pickDbg.hitScene.y, g_pickDbg.hitScene.z);
            ImGui::Text("reproj_sc: (%.1f, %.1f)", g_pickDbg.reprX, g_pickDbg.reprY);
            ImGui::Text("cursor_sc: (%.1f, %.1f)", g_pickDbg.cursorX, g_pickDbg.cursorY);
            const float edx = g_pickDbg.reprX - g_pickDbg.cursorX;
            const float edy = g_pickDbg.reprY - g_pickDbg.cursorY;
            const float err = sqrtf(edx * edx + edy * edy);
            if (err > 5.f)
                ImGui::TextColored(kRedColor, "!! REPROJECT ERROR: %.1f px", err);
            else
                ImGui::TextColored(kGreenColor, "reproject OK (%.1f px)", err);
            ImGui::End();
        }
#endif

        // ── Sidebar ────────────────────────────────────────────────────────
        //
        // When OPEN:  320px panel anchored to right edge, full height.
        // When CLOSED: 28px tab strip on right edge with ">" open button.
        // All interactive items pushed to kFPy=15 frame padding (~43px) for
        // 44px touch targets.

        if (!g_sidebarOpen) {
            // ── Collapsed tab ─────────────────────────────────────────────
            const float tabH = 44.f;
            ImGui::SetNextWindowPos(
                { io.DisplaySize.x - kTabW,
                  (io.DisplaySize.y - tabH) * 0.5f },
                ImGuiCond_Always);
            ImGui::SetNextWindowSize({ kTabW, tabH }, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.7f);
            ImGui::Begin("##tabtoggle", nullptr,
                ImGuiWindowFlags_NoTitleBar    | ImGuiWindowFlags_NoMove     |
                ImGuiWindowFlags_NoResize      | ImGuiWindowFlags_NoScrollbar|
                ImGuiWindowFlags_NoSavedSettings);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, kFPy));
            if (ImGui::Button("<", ImVec2(kTabW - 8.f, 0.f))) g_sidebarOpen = true;
            ImGui::PopStyleVar();
            g_sidebarPos  = ImGui::GetWindowPos();
            g_sidebarSize = ImGui::GetWindowSize();
            ImGui::End();

        } else {
            // ── Open sidebar ──────────────────────────────────────────────
            ImGui::SetNextWindowPos(
                { io.DisplaySize.x - kSidebarW, 0.f }, ImGuiCond_Always);
            ImGui::SetNextWindowSize(
                { kSidebarW, io.DisplaySize.y }, ImGuiCond_Always);
            ImGui::Begin("##sidebar", nullptr,
                ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoMove     |
                ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoSavedSettings);

            g_sidebarPos  = ImGui::GetWindowPos();
            g_sidebarSize = ImGui::GetWindowSize();

            // ── Header row ────────────────────────────────────────────────
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kFPx, kFPy));
            if (ImGui::Button(">", ImVec2(0.f, 0.f))) g_sidebarOpen = false;
            ImGui::PopStyleVar();
            ImGui::SameLine();
            ImGui::TextUnformatted("Controls  (Esc)");
            ImGui::Separator();

            // ── Files section ─────────────────────────────────────────────
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(kAccentColor, "FILES");
            {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kFPx, kFPy));

                // ── Terrain folder ────────────────────────────────────────
                ImGui::TextUnformatted("Terrain folder:");
                ImGui::SetNextItemWidth(-70.f);
                {
                    const std::string disp = g_terrainFolder.empty()
                        ? "(not set)" : fs::path(g_terrainFolder).filename().string();
                    ImGui::TextDisabled("%s", disp.c_str());
                }
                ImGui::SameLine();
                if (ImGui::Button("Browse##tfldr", ImVec2(60.f, 0.f))) {
                    const std::string p = OpenFolderDlg(g_hwnd, L"Select Terrain Folder");
                    if (!p.empty() && p != g_terrainFolder) {
                        g_terrainFolder = p;
                        FullReload();
                    }
                }
                ImGui::Checkbox("Terrain##tvis", &g_showTerrain);

                ImGui::Spacing();

                // ── Designs folder ────────────────────────────────────────
                ImGui::TextUnformatted("Designs folder:");
                {
                    const std::string disp = g_designsFolder.empty()
                        ? "(not set)" : fs::path(g_designsFolder).filename().string();
                    ImGui::TextDisabled("%s", disp.c_str());
                }
                ImGui::SameLine();
                if (ImGui::Button("Browse##dfldr", ImVec2(60.f, 0.f))) {
                    const std::string p = OpenFolderDlg(g_hwnd, L"Select Designs Folder");
                    if (!p.empty() && p != g_designsFolder) {
                        g_designsFolder = p;
                        // Evict all existing design sets
                        for (auto& ds : g_designSets) {
                            if (ds.grid)   { ds.grid->EvictAll(); ds.grid.reset(); }
                            if (ds.lwMesh) { ds.lwMesh.reset(); }
                        }
                        g_designSets = ScanDesignFolder(g_designsFolder);
                        for (int i = 0; i < static_cast<int>(g_designSets.size()); ++i)
                            g_designSets[i].budgetBase = 100000 * (i + 1);
                        // No auto-load — user explicitly checks designs they want
                    }
                }

                // ── Design set list ───────────────────────────────────────
                for (int i = 0; i < static_cast<int>(g_designSets.size()); ++i) {
                    auto& ds = g_designSets[i];
                    bool wasVisible = ds.visible;
                    char chkId[64];
                    snprintf(chkId, sizeof(chkId), "##dsvis%d", i);
                    ImGui::Checkbox(chkId, &ds.visible);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(ds.name.c_str());
                    if (!ds.msg.empty()) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%s)", ds.msg.c_str());
                    }

                    if (ds.visible != wasVisible) {
                        if (!ds.visible) {
                            // Uncheck: evict GPU buffers and free RAM cache.
                            // polylines and surfParsed are cleared so re-enable
                            // re-reads from DXF (cache hit keeps it fast).
                            if (ds.grid) { ds.grid->EvictAll(); ds.grid.reset(); }
                            if (ds.lwMesh) { ds.lwMesh.reset(); }
                            ds.polylines.clear();
                            ds.polylines.shrink_to_fit();
                            ds.surfParsed = false;
                        } else {
                            // Check: load from cache (fast) or parse (async)
                            LoadDesignSetAsync(i);
                        }
                    }
                }

                ImGui::PopStyleVar();
            }

            // ── Layers section ────────────────────────────────────────────
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(kAccentColor, "LAYERS");
            {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kFPx, kFPy));

                ImGui::TextUnformatted("Terrain opacity");
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::SliderFloat("##topac", &g_terrainOpacity, 0.f, 1.f, "%.2f"))
                    g_terrainPass.SetOpacity(g_terrainOpacity);

                ImGui::TextUnformatted("Design opacity (all)");
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::SliderFloat("##dopac", &g_designOpacity, 0.f, 1.f, "%.2f"))
                    g_designPass.SetOpacity(g_designOpacity);

                ImGui::TextUnformatted("Linework opacity (all)");
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::SliderFloat("##lopac", &g_lineworkOpacity, 0.f, 1.f, "%.2f"))
                    g_lineworkPass.SetOpacity(g_lineworkOpacity);

                ImGui::PopStyleVar();
            }

            // ── GPS section ───────────────────────────────────────────────
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(kAccentColor, "GPS");
            {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kFPx, kFPy));

                // Enable toggle.
                if (ImGui::Checkbox("GPS Mode##gpsena", &g_gpsMode))
                    if (g_gpsMode) g_gpsNeedElevLookup = true;

                // Connection type combo.
                {
                    int srcInt = static_cast<int>(g_gpsSourceType);
                    const char* srcItems[] = { "Mock (replay)", "Serial (COM)", "TCP" };
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::Combo("##gpssrc", &srcInt, srcItems, 3))
                        g_gpsSourceType = static_cast<GpsSourceType>(srcInt);
                }

                // Source-specific config.
                if (g_gpsSourceType == GpsSourceType::Serial) {
                    ImGui::SetNextItemWidth(90.f);
                    ImGui::InputText("Port##ser", g_serialPort, sizeof(g_serialPort));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::InputInt("Baud##ser", &g_serialBaud, 0);
                } else if (g_gpsSourceType == GpsSourceType::Tcp) {
                    ImGui::SetNextItemWidth(140.f);
                    ImGui::InputText("Host##tcp", g_tcpHost, sizeof(g_tcpHost));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::InputInt("Port##tcp", &g_tcpPort, 0);
                }

                // Height offset slider.
                ImGui::TextUnformatted("Eye height (m)");
                ImGui::SetNextItemWidth(-1.f);
                ImGui::SliderFloat("##hofs", &g_gpsHeightOffset, 0.f, 5.f, "%.2f");

                // Connect / Disconnect buttons.
                if (ImGui::Button("Connect##gpscon", ImVec2(-1.f, 0.f)))
                    RecreateGpsSource();
                if (g_gpsSrc) {
                    if (ImGui::Button("Disconnect##gpsdis", ImVec2(-1.f, 0.f)))
                        g_gpsSrc.reset();
                }

                // Status indicator.
                if (g_gpsSrc) {
                    const bool connected = g_gpsSrc->isConnected();
                    ImGui::TextColored(
                        connected
                            ? ImVec4{0.3f, 1.f, 0.3f, 1.f}
                            : ImVec4{1.f, 0.6f, 0.2f, 1.f},
                        connected ? "● connected" : "● connecting...");
                } else {
                    ImGui::TextDisabled("● disconnected");
                }

                // Live GPS reading when mode is active.
                if (g_gpsMode && g_gpsSrc) {
                    if (g_gpsLastKnown.valid) {
                        ImGui::Text("  scene (%.1f, %.1f, %.1f)",
                            g_gpsLastKnown.x, g_gpsLastKnown.y, g_gpsLastKnown.z);
                        ImGui::Text("  hdg=%.1f deg  terrain_z=%.1f m",
                            g_gpsLastKnown.heading, g_gpsCachedElev);
                    } else {
                        ImGui::TextColored({1.f, 0.6f, 0.f, 1.f},
                            "  no fix (camera frozen)");
                    }
                }

                ImGui::PopStyleVar();
            }

            // ── View section ──────────────────────────────────────────────
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(kAccentColor, "VIEW");
            {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kFPx, kFPy));

                if (ImGui::Button("Reset View##rstview", ImVec2(-1.f, 0.f))) {
                    g_camera.SetPivot(g_defaultPivot.x, g_defaultPivot.y, g_defaultPivot.z);
                    g_camera.SetSpherical(g_defaultRadius, 270.0f, 33.7f);
                }

                ImGui::PopStyleVar();

                // Coordinate readout (read-only — no touch target padding needed).
                ImGui::Separator();
                const char* datumLabel =
                    (g_crsDatum == gps::Datum::GDA2020) ? "GDA2020" : "GDA94";
                ImGui::Text("Coords (%s zone %d):", datumLabel, g_crsZone);
                if (g_coordValid) {
                    ImGui::Text("  E: %.3f m", g_coordMga.easting);
                    ImGui::Text("  N: %.3f m", g_coordMga.northing);
                    ImGui::Text("  Z: %.3f m", g_coordMga.elev);
                    ImGui::Text("  lat: %+.6f", g_coordWgs.lat_deg);
                    ImGui::Text("  lon: %+.6f", g_coordWgs.lon_deg);
                } else {
                    ImGui::TextDisabled("  (unavailable — check zone)");
                }

                {
                    const auto p = g_camera.Pivot();
                    ImGui::Text("  pivot (%.1f, %.1f, %.1f)", p.x, p.y, p.z);
                    ImGui::Text("  r=%.0f  az=%.0f  el=%.1f",
                        g_camera.Radius(), g_camera.Azimuth(), g_camera.Elevation());
                }
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kFPx, kFPy));
                if (ImGui::Checkbox("Freshness overlay (F)##fresh", &g_freshnessOverlay))
                    g_session.data.freshness_overlay_visible = g_freshnessOverlay;
                ImGui::PopStyleVar();
                ImGui::TextDisabled("LMB=orbit  RMB/MMB=pan  Wheel=zoom");
                ImGui::TextDisabled("LMBx2=pivot  R=reset  F=freshness  Esc=sidebar");
            }

            // ── Settings section ──────────────────────────────────────────
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(kAccentColor, "SETTINGS");
            {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kFPx, kFPy));

                // Server (Phase 10+) ───────────────────────────────────────
                ImGui::TextUnformatted("Server:");
                ImGui::SetNextItemWidth(-1.f);
                ImGui::InputText("##srvurl", g_serverUrl, sizeof(g_serverUrl));
                ImGui::Checkbox("Enable server connection##srvena", &g_serverEnabled);
                if (g_serverEnabled && g_serverUrl[0] != '\0') {
                    const auto [oh, om] = GetOfflineTime();
                    if (oh >= 0)
                        ImGui::TextDisabled("  Last contact: %dh %02dm ago", oh, om);
                    else
                        ImGui::TextDisabled("  Never connected");
                }

                ImGui::Separator();

                // CRS ──────────────────────────────────────────────────────
                ImGui::TextUnformatted("CRS:");
                {
                    const char* datumItems[] = { "GDA94", "GDA2020" };
                    ImGui::SetNextItemWidth(95.f);
                    ImGui::Combo("Datum##crs", &g_uiDatum, datumItems, 2);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(55.f);
                    ImGui::InputInt("Zone##crs", &g_uiZone, 0);
                    g_uiZone = std::clamp(g_uiZone, 49, 56);
                    if (g_zoneSuggest > 0) {
                        ImGui::SameLine();
                        char slab[24];
                        snprintf(slab, sizeof(slab), "Auto %d", g_zoneSuggest);
                        if (ImGui::SmallButton(slab)) g_uiZone = g_zoneSuggest;
                    }
                }
                if (ImGui::Button("Apply CRS##crsapply", ImVec2(-1.f, 0.f))) {
                    g_crsZone  = g_uiZone;
                    g_crsDatum = (g_uiDatum == 1) ? gps::Datum::GDA2020 : gps::Datum::GDA94;
                    g_coordPrevX = std::numeric_limits<float>::quiet_NaN();
                    RecreateGpsSource();
                }

                ImGui::Separator();

                // Cache ────────────────────────────────────────────────────
                ImGui::Checkbox("Keep disk cache on exit##dkchk",
                                &g_session.data.disk_cache_keep_on_exit);

                ImGui::Separator();

                // Button zoom step ─────────────────────────────────────────
                ImGui::TextUnformatted("Button zoom step:");
                ImGui::SetNextItemWidth(-1.f);
                ImGui::SliderFloat("##zstep", &g_zoomStep, 0.1f, 2.0f, "%.1f notches");

                ImGui::Separator();

                // Debug overlays ───────────────────────────────────────────
                if (g_tilesReady) {
                    bool forceLod0 = g_tileGrid.GetForceLod0();
                    if (ImGui::Checkbox("Force LOD0 terrain##flod", &forceLod0))
                        g_tileGrid.SetForceLod0(forceLod0);
                    // Design grids are always forced LOD0 — no toggle needed.
                }

                ImGui::PopStyleVar();
            }

            // ── Footer ────────────────────────────────────────────────────
            ImGui::Separator();
            if (g_tilesReady) {
                ImGui::Text("Terrain: %s", g_statusMsg.c_str());
                ImGui::Text("  tiles=%d  vis=%d  gpu=%d",
                    g_tileGrid.TileCount(), g_tileGrid.VisibleCount(), g_tileGrid.GpuCount());
            } else {
                ImGui::TextDisabled("Terrain: %s", g_statusMsg.c_str());
            }
            for (const auto& ds : g_designSets) {
                if (ds.grid || !ds.msg.empty())
                    ImGui::Text("%s: %s", ds.name.c_str(), ds.msg.c_str());
            }
            ImGui::Text("GPU: %zu/%d MB  evicted=%d",
                g_budget.UsedBytes() / (1024 * 1024),
                terrain::GPU_BUDGET_MB,
                g_budget.EvictCount());
            ImGui::Text("%s  %.0f fps",
                g_sessionPath.filename().string().c_str(),
                io.Framerate);

            ImGui::End();
        } // end sidebar

        // ── Render ────────────────────────────────────────────────────────
        g_renderer.BeginFrame();

        if (g_tilesReady && g_showTerrain) {
            // Freshness overlay: colour all tiles by server data age (F key toggle).
            // Phase 11 will update g_terrainFreshnessColor per-tile from manifest data.
            const DirectX::XMFLOAT4 overlayOff = { 1.f, 1.f, 1.f, 0.f };
            g_terrainPass.SetTileOverlayColor(
                g_freshnessOverlay ? g_terrainFreshnessColor : overlayOff);
            g_terrainPass.Begin(g_renderer.Context(), view, proj);
            for (const auto& item : g_tileGrid.GetDrawList(camPos))
                g_terrainPass.DrawMesh(g_renderer.Context(), *item.mesh, item.lod);
            g_terrainPass.End(g_renderer.Context());
        }

        // ── Design sets: linework then surfaces (per set, per world matrix) ──
        for (auto& ds : g_designSets) {
            if (!ds.visible) continue;
            const float ox = ds.surfOrigin[0] - g_sceneOrigin[0];
            const float oy = ds.surfOrigin[1] - g_sceneOrigin[1];
            const float oz = ds.surfOrigin[2] - g_sceneOrigin[2];
            const auto wm  = DirectX::XMMatrixTranslation(ox, oy, oz);

            if (ds.lwMesh) {
                const float lwOx = ds.lwOrigin[0] - g_sceneOrigin[0];
                const float lwOy = ds.lwOrigin[1] - g_sceneOrigin[1];
                const float lwOz = ds.lwOrigin[2] - g_sceneOrigin[2];
                g_lineworkPass.SetWorldMatrix(
                    DirectX::XMMatrixTranslation(lwOx, lwOy, lwOz));
                g_lineworkPass.Begin(g_renderer.Context(), view, proj, vpW, vpH);
                g_lineworkPass.Draw(g_renderer.Context(), *ds.lwMesh);
                g_lineworkPass.End(g_renderer.Context());
            }

            if (ds.grid) {
                g_designPass.SetWorldMatrix(wm);
                g_designPass.Begin(g_renderer.Context(), view, proj);
                for (const auto& item : ds.grid->GetDrawList(camPos))
                    g_designPass.DrawMesh(g_renderer.Context(), *item.mesh, item.lod);
                g_designPass.End(g_renderer.Context());
            }
        }

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_renderer.EndFrame();

        // ── Frame rate limiter ─────────────────────────────────────────────
        // Cap at ~60 fps when idle to reduce CPU load and prevent thermal
        // throttle on the tablet.  Wakes immediately on any input event, so
        // orbit/pan response is not affected.  Skipped while parsing so the
        // progress bar updates as fast as possible.
        if (!g_parseBusy.load())
            MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT,
                                        MWMO_INPUTAVAILABLE);
    }

    // ── Shutdown ──────────────────────────────────────────────────────────

    g_gpsSrc.reset();

    g_autoSaveRunning = false;
    if (g_autoSaveThread.joinable())
        g_autoSaveThread.join();

    // Wait for any in-progress parse to finish before releasing GPU resources.
    if (g_loaderThread.joinable())
        g_loaderThread.join();

    GatherSession();
    g_session.Save(g_sessionPath);

    // Release all design set GPU resources before DX11 device shutdown.
    for (auto& ds : g_designSets) {
        ds.lwMesh.reset();
        ds.grid.reset();
    }

    if (!g_session.data.disk_cache_keep_on_exit) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        fs::path cacheRoot = fs::path(tmp) / L"TerrainViewer" / L"tiles";
        std::error_code ec;
        fs::remove_all(cacheRoot, ec);
    }

    g_lineworkPass.Shutdown();
    g_designPass.Shutdown();
    g_terrainPass.Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_renderer.Shutdown();
    DestroyWindow(g_hwnd);
    UnregisterClass(_T("TerrainViewerWnd"), hInstance);

    CoUninitialize();

    return 0;
}
