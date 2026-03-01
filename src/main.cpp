#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tchar.h>

#include "renderer/Renderer.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

static Renderer g_renderer;
static bool     g_running = true;

// Forward declaration required by imgui_impl_win32
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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    // Register window class
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
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd)
        return 1;

    // Initialise renderer
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (!g_renderer.Init(hwnd, rc.right - rc.left, rc.bottom - rc.top))
    {
        MessageBox(hwnd, _T("D3D11 initialisation failed."), _T("Error"), MB_OK | MB_ICONERROR);
        return 1;
    }

    // Initialise Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_renderer.Device(), g_renderer.Context());

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Message loop
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

        if (!g_running)
            break;

        // Begin frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ImGui test window
        ImGui::Begin("Phase 0");
        ImGui::Text("Terrain Viewer \xe2\x80\x94 Phase 0");
        ImGui::Separator();
        ImGui::Text("DX11 window + ImGui rendering OK");
        ImGui::Text("Application average %.1f ms/frame (%.1f FPS)",
            1000.0f / io.Framerate, io.Framerate);
        ImGui::End();

        // Render
        g_renderer.BeginFrame();
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_renderer.EndFrame();
    }

    // Shutdown
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_renderer.Shutdown();
    DestroyWindow(hwnd);
    UnregisterClass(_T("TerrainViewerWnd"), hInstance);

    return 0;
}
