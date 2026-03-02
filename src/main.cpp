#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tchar.h>
#include <DirectXMath.h>

#include "renderer/Renderer.h"
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
using namespace DirectX;

// Widen a narrow CMake compile-definition string literal.
#define TO_WIDE_(s) L##s
#define TO_WIDE(s)  TO_WIDE_(s)

// TERRAIN_DXF_STR is injected by CMake as a narrow string literal.
static const char kDxfPathNarrow[] = TERRAIN_DXF_STR;

// ── Globals ───────────────────────────────────────────────────────────────────

static Renderer    g_renderer;
static Mesh        g_mesh;
static TerrainPass g_terrainPass;
static bool        g_running      = true;
static bool        g_terrainReady = false;
static std::string g_statusMsg;

// Camera matrices (static for Phase 2 S1 — no user-controllable camera yet).
static XMMATRIX g_view;
static XMMATRIX g_proj;

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

    // Cache in %TEMP%\TerrainViewer\tiles\<dxf stem>
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    fs::path cacheDir = fs::path(tmp) / L"TerrainViewer" / L"tiles" / dxfPath.stem();

    std::atomic<float> progress{0.0f};
    auto result = dxf::parseToCache(dxfPath, cacheDir, progress);

    if (result.faceCount == 0 && result.tileCount == 0) {
        g_statusMsg = "Parse produced no tiles.";
        return;
    }

    // Pick the first available lod0 tile.
    for (auto& entry : fs::directory_iterator(cacheDir)) {
        const auto name = entry.path().filename().string();
        if (name.find("_lod0.bin") == std::string::npos) continue;

        if (g_mesh.Load(g_renderer.Device(), entry.path())) {
            g_statusMsg = "Loaded tile: " + name +
                          " (" + std::to_string(result.faceCount) + " total faces)";
            g_terrainReady = true;
        } else {
            g_statusMsg = "GPU upload failed for: " + name;
        }
        break;
    }

    if (!g_terrainReady)
        g_statusMsg = "No loadable tile found in cache.";
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
    const int winW = rc.right  - rc.left;
    const int winH = rc.bottom - rc.top;

    if (!g_renderer.Init(hwnd, winW, winH)) {
        MessageBox(hwnd, _T("D3D11 initialisation failed."), _T("Error"),
                   MB_OK | MB_ICONERROR);
        return 1;
    }

    // ── Camera (fixed for Phase 2 S1) ────────────────────────────────────
    // Eye 120 m above terrain, 100 m south, looking at terrain centre.
    g_view = XMMatrixLookAtRH(
        XMVectorSet( 25.0f, -100.0f, 120.0f, 0.0f),   // eye
        XMVectorSet( 25.0f,   25.0f,   8.0f, 0.0f),   // look-at
        XMVectorSet(  0.0f,    0.0f,   1.0f, 0.0f));  // up (Z-up)

    g_proj = XMMatrixPerspectiveFovRH(
        XMConvertToRadians(60.0f),
        static_cast<float>(winW) / static_cast<float>(winH),
        0.5f, 5000.0f);

    // ── Terrain (parse + GPU upload) ──────────────────────────────────────
    LoadTerrain();

    // ── TerrainPass ───────────────────────────────────────────────────────
    if (!g_terrainPass.Init(g_renderer.Device())) {
        MessageBox(hwnd, _T("TerrainPass::Init failed (shader compile error?)."),
                   _T("Error"), MB_OK | MB_ICONERROR);
        // Non-fatal: ImGui overlay will still run.
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

        ImGui::Begin("Terrain Viewer — Phase 2");
        ImGui::Text("DX11 + terrain mesh rendering");
        ImGui::Separator();
        ImGui::Text("%s", g_statusMsg.c_str());
        ImGui::Text("%.1f ms/frame  (%.1f FPS)",
            1000.0f / io.Framerate, io.Framerate);
        ImGui::End();

        // Frame
        g_renderer.BeginFrame();

        if (g_terrainReady)
            g_terrainPass.Render(g_renderer.Context(), g_mesh, g_view, g_proj);

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
