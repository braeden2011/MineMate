#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <tchar.h>

#include "renderer/Renderer.h"
#include "app/Camera.h"
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

#include <atomic>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

// Widen a narrow CMake compile-definition string literal.
#define TO_WIDE_(s) L##s
#define TO_WIDE(s)  TO_WIDE_(s)

static const char kTerrainDxfPath[]  = TERRAIN_DXF_STR;
static const char kDesignDxfPath[]   = DESIGN_DXF_STR;
static const char kLineworkDxfPath[] = LINEWORK_DXF_STR;

// ── Globals ───────────────────────────────────────────────────────────────────

static Renderer    g_renderer;
static Camera      g_camera;
static GpuBudget   g_budget(static_cast<size_t>(terrain::GPU_BUDGET_MB) * 1024 * 1024);
static TileGrid    g_tileGrid;
static TileGrid      g_designGrid;
static TerrainPass   g_terrainPass;
static DesignPass    g_designPass;
static LineworkMesh  g_lineworkMesh;
static LineworkPass  g_lineworkPass;
static bool          g_running        = true;
static bool          g_tilesReady     = false;
static bool          g_designReady    = false;
static bool          g_lineworkReady  = false;
static std::string   g_statusMsg;
static std::string   g_designMsg;
static std::string   g_lineworkMsg;

// Each DXF stores its own $EXTMIN as the scene origin.  These three values let
// us compute per-surface correction offsets after all loads complete.
static std::array<float, 3> g_sceneOrigin    = {0.0f, 0.0f, 0.0f};  // terrain (authoritative)
static std::array<float, 3> g_designOrigin   = {0.0f, 0.0f, 0.0f};
static std::array<float, 3> g_lineworkOrigin = {0.0f, 0.0f, 0.0f};

// ── Visibility / mode flags ───────────────────────────────────────────────────

static bool               g_showTerrain   = true;
static bool               g_showDesign    = true;
static bool               g_showLinework  = true;
static bool               g_showSidebar   = true;
static bool               g_gpsMode       = false;
static DirectX::XMFLOAT3 g_defaultPivot  = {0.0f, 0.0f, 0.0f};
static float              g_defaultRadius = 360.0f;

// ── GPS state ─────────────────────────────────────────────────────────────────

static std::unique_ptr<gps::MockGpsSource> g_gpsSrc;
// Last valid ScenePosition — frozen during GPS dropout; updated on each valid poll.
static gps::ScenePosition g_gpsLastKnown{};
// Cached terrain elevation at the last GPS XY (AABB top, updated when moved > threshold).
static float g_gpsCachedElev      = 0.0f;
static float g_gpsPrevElevX       = 0.0f;
static float g_gpsPrevElevY       = 0.0f;
static bool  g_gpsNeedElevLookup  = true;  // force lookup on first valid fix / on mode entry

// ── Mouse state ───────────────────────────────────────────────────────────────

static POINT g_lastMousePos = {0, 0};
static bool  g_lmbDown      = false;
static bool  g_rmbDown      = false;
static bool  g_mmbDown      = false;

// ── Touch state (WM_POINTER, up to 2 contacts) ────────────────────────────────

struct TouchContact { UINT32 id; float x, y; bool live = false; };
static TouchContact g_touch[2];
static int          g_touchN      = 0;     // live contact count
static float        g_touchPrevCX = 0.f;   // previous centroid X (or single-finger X)
static float        g_touchPrevCY = 0.f;
static float        g_touchPrevD  = 1.f;   // previous inter-finger distance (2-contact)

// ImGui sidebar screen rect — updated each frame; used for touch hit-testing.
// One-frame lag is imperceptible and avoids the need for imgui_internal.h.
static ImVec2 g_sidebarPos  = {0.f, 0.f};
static ImVec2 g_sidebarSize = {0.f, 0.f};

// ── Touch baseline reset ───────────────────────────────────────────────────────
// Call whenever the active contact count changes to prevent position jumps.
// Resets the "previous" centroid / distance to the CURRENT contact positions.
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
// D3D NDC: x∈[-1,1], y∈[-1,1], z∈[0,1] (near=0, far=1).
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

    XMVECTOR nearNdc  = XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
    XMVECTOR farNdc   = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);
    XMVECTOR nearWorld = XMVector4Transform(nearNdc, invVP);
    XMVECTOR farWorld  = XMVector4Transform(farNdc,  invVP);

    nearWorld = nearWorld / XMVectorSplatW(nearWorld);
    farWorld  = farWorld  / XMVectorSplatW(farWorld);

    XMStoreFloat3(&rayOriginOut, nearWorld);
    XMStoreFloat3(&rayDirOut,    XMVector3Normalize(farWorld - nearWorld));
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

    // ── WM_POINTER: touch (PT_TOUCH) and mouse-via-pointer (PT_MOUSE) ────
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

        // ── PT_TOUCH ─────────────────────────────────────────────────────
        if (pi.pointerType == PT_TOUCH) {
            // If the finger is over the ImGui sidebar, don't absorb the event
            // so ImGui can process it through its own WM_POINTER handler.
            const bool overUi = g_showSidebar
                && px >= g_sidebarPos.x && px < g_sidebarPos.x + g_sidebarSize.x
                && py >= g_sidebarPos.y && py < g_sidebarPos.y + g_sidebarSize.y;
            if (overUi)
                return DefWindowProc(hwnd, msg, wParam, lParam);

            if (msg == WM_POINTERDOWN) {
                // Claim a free slot (silently ignore a 3rd+ contact).
                for (auto& c : g_touch) {
                    if (!c.live) {
                        c = { pid, px, py, true };
                        ++g_touchN;
                        TouchUpdateBaselines();  // reset prev on count change
                        break;
                    }
                }
            } else if (msg == WM_POINTERUP) {
                for (auto& c : g_touch) {
                    if (c.live && c.id == pid) {
                        c.live = false;
                        --g_touchN;
                        TouchUpdateBaselines();  // reset prev on count change
                        break;
                    }
                }
            } else { // WM_POINTERUPDATE
                // Find the slot for this contact.
                int slot = -1;
                for (int i = 0; i < 2; ++i)
                    if (g_touch[i].live && g_touch[i].id == pid) { slot = i; break; }
                if (slot < 0) return 0;   // unknown contact, ignore

                // Update position.
                g_touch[slot].x = px;
                g_touch[slot].y = py;

                if (g_touchN == 1) {
                    // Single finger → orbit.
                    g_camera.OrbitDelta(px - g_touchPrevCX, py - g_touchPrevCY);
                    g_touchPrevCX = px;
                    g_touchPrevCY = py;
                } else if (g_touchN == 2) {
                    // Two fingers → centroid pan + pinch zoom (simultaneous).
                    const float cx  = (g_touch[0].x + g_touch[1].x) * 0.5f;
                    const float cy  = (g_touch[0].y + g_touch[1].y) * 0.5f;
                    g_camera.PanDelta(cx - g_touchPrevCX, cy - g_touchPrevCY);

                    const float ddx = g_touch[1].x - g_touch[0].x;
                    const float ddy = g_touch[1].y - g_touch[0].y;
                    const float d   = std::max(1.f, sqrtf(ddx * ddx + ddy * ddy));
                    if (g_touchPrevD > 1.f) {
                        // Camera::ZoomDelta(n): radius *= 0.85^n; positive n = zoom in.
                        // Pinch ratio r = d/prevD: spread (r>1) = zoom in (n>0).
                        // 0.85^n = 1/r  →  n = -ln(r) / ln(0.85)
                        const float notches = -logf(d / g_touchPrevD) / logf(0.85f);
                        g_camera.ZoomDelta(notches);
                    }

                    g_touchPrevCX = cx;
                    g_touchPrevCY = cy;
                    g_touchPrevD  = d;
                }
            }
            return 0;  // consumed; prevents synthesised WM_LBUTTON* for touch
        }

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    // ── LMB double-click: unproject ray → pivot on terrain AABB ──────────
    case WM_LBUTTONDBLCLK:
        if (!ImGui::GetIO().WantCaptureMouse && g_tilesReady) {
            g_lmbDown      = true;   // ensure WM_LBUTTONUP is handled correctly
            g_lastMousePos = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetCapture(hwnd);

            const float vpW    = static_cast<float>(g_renderer.Width());
            const float vpH    = static_cast<float>(g_renderer.Height());
            const auto  view   = g_camera.ViewMatrix();
            const auto  proj   = g_camera.ProjMatrix(vpW / vpH);

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
            case 'R':
                g_camera.SetPivot(g_defaultPivot.x, g_defaultPivot.y, g_defaultPivot.z);
                g_camera.SetSpherical(g_defaultRadius, 270.0f, 33.7f);
                break;
            case 'G':
                g_gpsMode = !g_gpsMode;
                if (g_gpsMode) g_gpsNeedElevLookup = true;  // re-lookup on mode entry
                break;
            case 'T': g_showTerrain   = !g_showTerrain;  break;
            case 'D': g_showDesign    = !g_showDesign;   break;
            case 'L': g_showLinework  = !g_showLinework; break;
            case VK_ESCAPE: g_showSidebar = !g_showSidebar; break;
            }
        }
        return 0;

    // ── Mouse: orbit (LMB) ────────────────────────────────────────────────
    case WM_LBUTTONDOWN:
        if (!ImGui::GetIO().WantCaptureMouse) {
            g_lmbDown = true;
            g_lastMousePos = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetCapture(hwnd);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_lmbDown) {
            g_lmbDown = false;
            if (!g_rmbDown && !g_mmbDown) ReleaseCapture();
        }
        return 0;

    // ── Mouse: pan (RMB) ──────────────────────────────────────────────────
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

    // ── Mouse: pan (MMB) ──────────────────────────────────────────────────
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

    // ── Mouse: move (orbit or pan) ────────────────────────────────────────
    case WM_MOUSEMOVE:
        if (g_lmbDown || g_rmbDown || g_mmbDown) {
            const POINT cur = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const float dx  = static_cast<float>(cur.x - g_lastMousePos.x);
            const float dy  = static_cast<float>(cur.y - g_lastMousePos.y);

            if (g_lmbDown)
                g_camera.OrbitDelta(dx, dy);
            else if (g_rmbDown || g_mmbDown)
                g_camera.PanDelta(dx, dy);

            g_lastMousePos = cur;
        }
        return 0;

    // ── Mouse: zoom (wheel) ───────────────────────────────────────────────
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

// ── Terrain setup ─────────────────────────────────────────────────────────────

static void LoadTerrain()
{
    fs::path dxfPath = kTerrainDxfPath;

    if (!fs::exists(dxfPath)) {
        g_statusMsg = "terrain DXF not found: " + dxfPath.string();
        return;
    }

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    fs::path cacheDir = fs::path(tmp) / L"TerrainViewer" / L"tiles" / dxfPath.stem();

    std::atomic<float> progress{0.0f};
    auto result = dxf::parseToCache(dxfPath, cacheDir, progress);

    if (result.faceCount == 0 && result.tileCount == 0) {
        g_statusMsg = "Parse produced no tiles.";
        return;
    }

    if (!g_tileGrid.Init(cacheDir)) {
        g_statusMsg = "TileGrid::Init failed — no tiles in cache.";
        return;
    }

    g_tileGrid.SetBudget(&g_budget);
    g_sceneOrigin = result.origin;   // terrain $EXTMIN — authoritative scene origin
    g_tilesReady  = true;
    g_statusMsg  = std::to_string(g_tileGrid.TileCount()) + " tiles  "
                 + std::to_string(result.faceCount)       + " faces";

    // ── Default camera: pivot = terrain centre, radius = half-diagonal ────
    const auto  centre = g_tileGrid.SceneCentre();
    const float radius = g_tileGrid.SceneRadius();
    g_camera.SetPivot(centre.x, centre.y, centre.z);
    g_camera.SetSpherical(radius, 270.0f, 33.7f);
    g_defaultPivot  = centre;
    g_defaultRadius = radius;
}

// ── Design surface setup ───────────────────────────────────────────────────────

static void LoadDesign()
{
    fs::path dxfPath = kDesignDxfPath;

    if (!fs::exists(dxfPath)) {
        g_designMsg = "design DXF not found: " + dxfPath.string();
        return;
    }

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    fs::path cacheDir = fs::path(tmp) / L"TerrainViewer" / L"tiles" / dxfPath.stem();

    std::atomic<float> progress{0.0f};
    auto result = dxf::parseToCache(dxfPath, cacheDir, progress);

    if (result.faceCount == 0 && result.tileCount == 0) {
        g_designMsg = "Design parse produced no tiles.";
        return;
    }

    if (!g_designGrid.Init(cacheDir)) {
        g_designMsg = "DesignGrid::Init failed — no tiles in cache.";
        return;
    }

    // Share the same GpuBudget as the terrain grid.
    // Use index base 100000 to prevent key collisions with terrain tile indices.
    g_designGrid.SetBudget(&g_budget);
    g_designGrid.SetBudgetIndexBase(100000);

    g_designOrigin = result.origin;  // design $EXTMIN — corrected vs scene origin after init
    g_designReady  = true;
    g_designMsg   = std::to_string(g_designGrid.TileCount()) + " tiles  "
                  + std::to_string(result.faceCount)         + " faces";
}

// ── Linework setup ────────────────────────────────────────────────────────────

static void LoadLinework()
{
    fs::path dxfPath = kLineworkDxfPath;

    if (!fs::exists(dxfPath)) {
        g_lineworkMsg = "linework DXF not found: " + dxfPath.string();
        return;
    }

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    fs::path cacheDir = fs::path(tmp) / L"TerrainViewer" / L"tiles" / dxfPath.stem();

    // parseToCache returns empty polylines on cache hit (polylines not stored on disk
    // in Phase 1 cache).  For a linework-only DXF (zero tile .bin files), clearing
    // the trivial cache.meta forces a fresh parse so polylines are always available.
    dxf::clearTileCache(cacheDir);

    std::atomic<float> progress{0.0f};
    auto result = dxf::parseToCache(dxfPath, cacheDir, progress);

    if (result.polylines.empty()) {
        g_lineworkMsg = "no polylines found in linework DXF.";
        return;
    }

    if (!g_lineworkMesh.Load(g_renderer.Device(), result.polylines)) {
        g_lineworkMsg = "LineworkMesh::Load failed (no valid segments?).";
        return;
    }

    g_lineworkOrigin = result.origin;  // linework $EXTMIN — corrected vs scene origin after init
    g_lineworkReady  = true;
    g_lineworkMsg    = std::to_string(result.polylines.size())      + " polylines  "
                     + std::to_string(g_lineworkMesh.SegmentCount()) + " segs";
}

// ── Entry point ───────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    // ── Window ────────────────────────────────────────────────────────────
    WNDCLASSEX wc{};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = _T("TerrainViewerWnd");
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowEx(
        0,
        _T("TerrainViewerWnd"),
        _T("Terrain Viewer"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1280, 720,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) return 1;

    // ── Renderer ──────────────────────────────────────────────────────────
    RECT rc{};
    GetClientRect(hwnd, &rc);

    if (!g_renderer.Init(hwnd, rc.right - rc.left, rc.bottom - rc.top)) {
        MessageBox(hwnd, _T("D3D11 initialisation failed."), _T("Error"),
                   MB_OK | MB_ICONERROR);
        return 1;
    }

    // ── Terrain (parse + tile grid + default camera) ──────────────────────
    LoadTerrain();

    // ── Design surface (parse + tile grid, shares GPU budget) ─────────────
    LoadDesign();

    // ── Linework (parse polylines, build line list VB/IB) ─────────────────
    LoadLinework();

    // ── TerrainPass ───────────────────────────────────────────────────────
    if (!g_terrainPass.Init(g_renderer.Device())) {
        MessageBox(hwnd, _T("TerrainPass::Init failed (shader compile error?)."),
                   _T("Error"), MB_OK | MB_ICONERROR);
    }

    // ── DesignPass ────────────────────────────────────────────────────────
    if (!g_designPass.Init(g_renderer.Device())) {
        MessageBox(hwnd, _T("DesignPass::Init failed (shader compile error?)."),
                   _T("Error"), MB_OK | MB_ICONERROR);
    }

    // ── LineworkPass ──────────────────────────────────────────────────────
    if (!g_lineworkPass.Init(g_renderer.Device())) {
        MessageBox(hwnd, _T("LineworkPass::Init failed (shader compile error?)."),
                   _T("Error"), MB_OK | MB_ICONERROR);
    }

    // ── Origin alignment ──────────────────────────────────────────────────
    // Each DXF subtracts its own $EXTMIN from vertex positions.  If the three
    // DXFs share the same $EXTMIN the offsets will be zero and this is a no-op.
    // If they differ, we shift design/linework into terrain scene space.
    if (g_designReady) {
        const float dx = g_designOrigin[0] - g_sceneOrigin[0];
        const float dy = g_designOrigin[1] - g_sceneOrigin[1];
        const float dz = g_designOrigin[2] - g_sceneOrigin[2];
        g_designGrid.ApplyOriginOffset(dx, dy, dz);
        g_designPass.SetWorldMatrix(DirectX::XMMatrixTranslation(dx, dy, dz));
    }
    if (g_lineworkReady) {
        const float dx = g_lineworkOrigin[0] - g_sceneOrigin[0];
        const float dy = g_lineworkOrigin[1] - g_sceneOrigin[1];
        const float dz = g_lineworkOrigin[2] - g_sceneOrigin[2];
        g_lineworkPass.SetWorldMatrix(DirectX::XMMatrixTranslation(dx, dy, dz));
    }

    // ── GPS source ────────────────────────────────────────────────────────
    // MockGpsSource replays an NMEA file at 1 Hz; falls back to hardcoded
    // Sydney-area sentences if the file is absent.  Zone is a local-mode
    // placeholder — Phase 10 will read it from the server /config endpoint.
    {
        const fs::path nmeaPath =
            fs::path(kTerrainDxfPath).parent_path() / "gps.nmea";
        g_gpsSrc = std::make_unique<gps::MockGpsSource>(
            nmeaPath, g_sceneOrigin, terrain::GPS_MGA_ZONE);
    }

    // ── Dear ImGui ────────────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_renderer.Device(), g_renderer.Context());

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // ── Message loop ──────────────────────────────────────────────────────
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
        if (g_tilesReady || g_designReady) {
            const auto planes = ExtractFrustumPlanes(view, proj);
            if (g_tilesReady) {
                g_tileGrid.UpdateVisibility(planes, camPos);
                g_tileGrid.FlushLoads(g_renderer.Device(),
                                      terrain::MAX_TILE_LOADS_PER_FRAME);
            }
            if (g_designReady) {
                g_designGrid.UpdateVisibility(planes, camPos);
                g_designGrid.FlushLoads(g_renderer.Device(),
                                        terrain::MAX_TILE_LOADS_PER_FRAME);
            }
        }

        // ── GPS camera mode ───────────────────────────────────────────────
        // When g_gpsMode is on and a valid fix is available: position the
        // camera eye at the GPS XY, at terrain-AABB-top + height offset,
        // looking in the heading direction.
        // On dropout (!valid): camera stays frozen; user can orbit freely.
        // On fix restore: tracking resumes automatically.
        if (g_gpsMode && g_gpsSrc) {
            const gps::ScenePosition gpsPos = g_gpsSrc->poll();
            if (gpsPos.valid) {
                g_gpsLastKnown = gpsPos;

                // Terrain elevation: downward AABB raycast, cached until the
                // GPS position moves more than GPS_MOVE_THRESHOLD_M.
                const float dex   = gpsPos.x - g_gpsPrevElevX;
                const float dey   = gpsPos.y - g_gpsPrevElevY;
                const float moveSq = dex * dex + dey * dey;
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

                // Place camera eye at GPS position + standing height above terrain.
                // Look-pivot is GPS_VIEW_DISTANCE_M ahead in heading direction.
                // Scene coords: North = +Y, East = +X.
                // Heading = clockwise degrees from North (standard compass bearing).
                const float eyeZ       = g_gpsCachedElev + terrain::GPS_HEIGHT_OFFSET_M;
                const float headingRad = gpsPos.heading * DirectX::XM_PI / 180.0f;
                const float lookDist   = terrain::GPS_VIEW_DISTANCE_M;
                const float lookX      = sinf(headingRad) * lookDist;
                const float lookY      = cosf(headingRad) * lookDist;
                // Camera azimuth (CCW from +X): heading 0°(N)→270°, 90°(E)→180°.
                const float azimuth = fmodf(270.0f - gpsPos.heading + 360.0f, 360.0f);
                g_camera.SetPivot(gpsPos.x + lookX, gpsPos.y + lookY, eyeZ);
                g_camera.SetSpherical(lookDist, azimuth, 0.0f);
            }
            // !valid: GPS dropout — camera frozen at last position, user orbits freely.
        }

        // ── ImGui ─────────────────────────────────────────────────────────
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (g_showSidebar) {
            const auto p = g_camera.Pivot();
            ImGui::Begin("Terrain Viewer — Phase 7", &g_showSidebar);
            // Capture window rect for touch hit-testing in WM_POINTER handler.
            g_sidebarPos  = ImGui::GetWindowPos();
            g_sidebarSize = ImGui::GetWindowSize();
            ImGui::Text("LMB=orbit  RMB/MMB=pan  Wheel=zoom  LMBx2=pivot");
            ImGui::Text("R=reset  T=terrain  D=design  L=linework  Esc=hide");
            ImGui::Separator();
            ImGui::Text("pivot (%.1f, %.1f, %.1f)", p.x, p.y, p.z);
            ImGui::Text("r=%.0f m  az=%.0f deg  el=%.1f deg",
                g_camera.Radius(), g_camera.Azimuth(), g_camera.Elevation());
            ImGui::Separator();
            ImGui::Text("Visibility:");
            ImGui::Checkbox("Terrain##vis",  &g_showTerrain);
            ImGui::SameLine();
            ImGui::Checkbox("Design##vis",   &g_showDesign);
            ImGui::SameLine();
            ImGui::Checkbox("Linework##vis", &g_showLinework);
            ImGui::SameLine();
            ImGui::Checkbox("GPS##vis",      &g_gpsMode);
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
            ImGui::Separator();
            ImGui::Text("Terrain: %s", g_statusMsg.c_str());
            if (g_tilesReady) {
                ImGui::Text("  tiles=%d  visible=%d  gpu=%d",
                    g_tileGrid.TileCount(),
                    g_tileGrid.VisibleCount(),
                    g_tileGrid.GpuCount());
            }
            ImGui::Text("Design:  %s", g_designMsg.c_str());
            if (g_designReady) {
                ImGui::Text("  tiles=%d  visible=%d  gpu=%d",
                    g_designGrid.TileCount(),
                    g_designGrid.VisibleCount(),
                    g_designGrid.GpuCount());
            }
            ImGui::Text("Lines:   %s", g_lineworkMsg.c_str());
            ImGui::Text("GPU: %zu / %d MB  evicted=%d",
                g_budget.UsedBytes() / (1024 * 1024),
                terrain::GPU_BUDGET_MB,
                g_budget.EvictCount());
            ImGui::Separator();
            if (g_tilesReady) {
                bool showLod = g_terrainPass.GetShowLodColour();
                if (ImGui::Checkbox("LOD overlay", &showLod)) {
                    g_terrainPass.SetShowLodColour(showLod);
                    g_designPass.SetShowLodColour(showLod);
                }

                bool forceLod0 = g_tileGrid.GetForceLod0();
                if (ImGui::Checkbox("Force LOD0 (full detail)", &forceLod0)) {
                    g_tileGrid.SetForceLod0(forceLod0);
                    g_designGrid.SetForceLod0(forceLod0);
                }
            }
            ImGui::Separator();
            ImGui::Text("%.1f ms/frame  (%.1f FPS)",
                1000.0f / io.Framerate, io.Framerate);
            ImGui::End();
        } else {
            g_sidebarSize = {0.f, 0.f};  // no hit area when sidebar is hidden
        }

        // ── Render ────────────────────────────────────────────────────────
        g_renderer.BeginFrame();

        // Pass 1: terrain — opaque, full depth write
        if (g_tilesReady && g_showTerrain) {
            g_terrainPass.Begin(g_renderer.Context(), view, proj);
            for (const auto& item : g_tileGrid.GetDrawList(camPos))
                g_terrainPass.DrawMesh(g_renderer.Context(), *item.mesh, item.lod);
            g_terrainPass.End();
        }

        // Pass 2: linework — geometry-shader quad expansion, depth write ON
        if (g_lineworkReady && g_showLinework) {
            const float vpW = static_cast<float>(g_renderer.Width());
            const float vpH = static_cast<float>(g_renderer.Height());
            g_lineworkPass.Begin(g_renderer.Context(), view, proj, vpW, vpH);
            g_lineworkPass.Draw(g_renderer.Context(), g_lineworkMesh);
            g_lineworkPass.End(g_renderer.Context());
        }

        // Pass 3: design surface — two-pass alpha blend, no depth write
        if (g_designReady && g_showDesign) {
            g_designPass.Begin(g_renderer.Context(), view, proj);
            for (const auto& item : g_designGrid.GetDrawList(camPos))
                g_designPass.DrawMesh(g_renderer.Context(), *item.mesh, item.lod);
            g_designPass.End(g_renderer.Context());
        }

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_renderer.EndFrame();
    }

    // ── Shutdown ──────────────────────────────────────────────────────────
    g_gpsSrc.reset();           // join background GPS thread before DX11 teardown
    g_lineworkPass.Shutdown();
    g_designPass.Shutdown();
    g_terrainPass.Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_renderer.Shutdown();
    DestroyWindow(hwnd);
    UnregisterClass(_T("TerrainViewerWnd"), hInstance);

    return 0;
}
