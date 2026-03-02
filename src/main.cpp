#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <tchar.h>

#include "renderer/Renderer.h"
#include "app/Camera.h"
#include "terrain/Mesh.h"
#include "terrain/TerrainPass.h"
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

static const char kDxfPathNarrow[] = TERRAIN_DXF_STR;

// ── Globals ───────────────────────────────────────────────────────────────────

static Renderer    g_renderer;
static Camera      g_camera;
static Mesh        g_mesh;
static TerrainPass g_terrainPass;
static bool        g_running      = true;
static bool        g_terrainReady = false;
static std::string g_statusMsg;

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
    fs::path dxfPath = kDxfPathNarrow;

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

    // Load the first available lod0 tile.
    for (auto& entry : fs::directory_iterator(cacheDir)) {
        const auto name = entry.path().filename().string();
        if (name.find("_lod0.bin") == std::string::npos) continue;

        if (g_mesh.Load(g_renderer.Device(), entry.path())) {
            g_statusMsg = "Tile: " + name +
                          "  faces: " + std::to_string(result.faceCount);
            g_terrainReady = true;
        } else {
            g_statusMsg = "GPU upload failed: " + name;
        }
        break;
    }

    if (!g_terrainReady) {
        g_statusMsg = "No loadable tile found in cache.";
        return;
    }

    // ── Default camera: pivot = tile AABB centre, eye = pivot + (0, -300, 200) ──
    // Spherical parameters from offset (0, -300, 200):
    //   radius    = sqrt(300² + 200²) ≈ 360.6 m
    //   azimuth   = atan2(-300, 0)   = 270°  (pointing in −Y direction)
    //   elevation = asin(200/360.6)  ≈  33.7°
    const auto c = g_mesh.AabbCentre();
    g_camera.SetPivot(c.x, c.y, c.z);
    g_camera.SetSpherical(360.6f, 270.0f, 33.7f);
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

    // ── Terrain (parse + GPU upload + default camera) ─────────────────────
    LoadTerrain();

    // ── TerrainPass ───────────────────────────────────────────────────────
    if (!g_terrainPass.Init(g_renderer.Device())) {
        MessageBox(hwnd, _T("TerrainPass::Init failed (shader compile error?)."),
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

        // ImGui
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        {
            const auto p = g_camera.Pivot();
            ImGui::Begin("Terrain Viewer — Phase 2");
            ImGui::Text("LMB=orbit  RMB/MMB=pan  Wheel=zoom");
            ImGui::Separator();
            ImGui::Text("pivot (%.1f, %.1f, %.1f)", p.x, p.y, p.z);
            ImGui::Text("r=%.0f m  az=%.0f deg  el=%.1f deg",
                g_camera.Radius(), g_camera.Azimuth(), g_camera.Elevation());
            ImGui::Separator();
            ImGui::Text("%s", g_statusMsg.c_str());
            ImGui::Text("%.1f ms/frame  (%.1f FPS)",
                1000.0f / io.Framerate, io.Framerate);
            ImGui::End();
        }

        // ── Render ────────────────────────────────────────────────────────
        const float aspect = static_cast<float>(g_renderer.Width()) /
                             static_cast<float>(g_renderer.Height());
        const auto view = g_camera.ViewMatrix();
        const auto proj = g_camera.ProjMatrix(aspect);

        g_renderer.BeginFrame();

        if (g_terrainReady)
            g_terrainPass.Render(g_renderer.Context(), g_mesh, view, proj);

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_renderer.EndFrame();
    }

    // ── Shutdown ──────────────────────────────────────────────────────────
    g_terrainPass.Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_renderer.Shutdown();
    DestroyWindow(hwnd);
    UnregisterClass(_T("TerrainViewerWnd"), hInstance);

    return 0;
}
