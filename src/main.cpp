#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <tchar.h>

#include "renderer/Renderer.h"
#include "app/Camera.h"
#include "terrain/Config.h"
#include "terrain/GpuBudget.h"
#include "terrain/DesignPass.h"
#include "terrain/TerrainPass.h"
#include "terrain/TileGrid.h"
#include "DxfParser.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <atomic>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// Widen a narrow CMake compile-definition string literal.
#define TO_WIDE_(s) L##s
#define TO_WIDE(s)  TO_WIDE_(s)

static const char kTerrainDxfPath[] = TERRAIN_DXF_STR;
static const char kDesignDxfPath[]  = DESIGN_DXF_STR;

// ── Globals ───────────────────────────────────────────────────────────────────

static Renderer    g_renderer;
static Camera      g_camera;
static GpuBudget   g_budget(static_cast<size_t>(terrain::GPU_BUDGET_MB) * 1024 * 1024);
static TileGrid    g_tileGrid;
static TileGrid    g_designGrid;
static TerrainPass g_terrainPass;
static DesignPass  g_designPass;
static bool        g_running      = true;
static bool        g_tilesReady   = false;
static bool        g_designReady  = false;
static std::string g_statusMsg;
static std::string g_designMsg;

// ── Mouse state ───────────────────────────────────────────────────────────────

static POINT g_lastMousePos = {0, 0};
static bool  g_lmbDown      = false;
static bool  g_rmbDown      = false;
static bool  g_mmbDown      = false;

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
    g_tilesReady = true;
    g_statusMsg  = std::to_string(g_tileGrid.TileCount()) + " tiles  "
                 + std::to_string(result.faceCount)       + " faces";

    // ── Default camera: pivot = terrain centre, radius = half-diagonal ────
    const auto  centre = g_tileGrid.SceneCentre();
    const float radius = g_tileGrid.SceneRadius();
    g_camera.SetPivot(centre.x, centre.y, centre.z);
    g_camera.SetSpherical(radius, 270.0f, 33.7f);
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

    g_designReady = true;
    g_designMsg   = std::to_string(g_designGrid.TileCount()) + " tiles  "
                  + std::to_string(result.faceCount)         + " faces";
}

// ── Entry point ───────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    // ── Window ────────────────────────────────────────────────────────────
    WNDCLASSEX wc{};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
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

        // ── ImGui ─────────────────────────────────────────────────────────
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        {
            const auto p = g_camera.Pivot();
            ImGui::Begin("Terrain Viewer — Phase 4");
            ImGui::Text("LMB=orbit  RMB/MMB=pan  Wheel=zoom");
            ImGui::Separator();
            ImGui::Text("pivot (%.1f, %.1f, %.1f)", p.x, p.y, p.z);
            ImGui::Text("r=%.0f m  az=%.0f deg  el=%.1f deg",
                g_camera.Radius(), g_camera.Azimuth(), g_camera.Elevation());
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
        }

        // ── Render ────────────────────────────────────────────────────────
        g_renderer.BeginFrame();

        // Pass 1: terrain — opaque, full depth write
        if (g_tilesReady) {
            g_terrainPass.Begin(g_renderer.Context(), view, proj);
            for (const auto& item : g_tileGrid.GetDrawList(camPos))
                g_terrainPass.DrawMesh(g_renderer.Context(), *item.mesh, item.lod);
            g_terrainPass.End();
        }

        // Pass 2: design surface — two-pass alpha blend, no depth write
        if (g_designReady) {
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
