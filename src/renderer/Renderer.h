#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class Renderer
{
public:
    bool Init(HWND hwnd, int width, int height);
    void Shutdown();

    // Clear RTV + DSV, set viewport, bind render targets.
    void BeginFrame();
    // Present.
    void EndFrame();

    ID3D11Device*        Device()  const { return m_device.Get(); }
    ID3D11DeviceContext* Context() const { return m_context.Get(); }
    IDXGISwapChain*      SwapChain() const { return m_swapChain.Get(); }

    int Width()  const { return m_width; }
    int Height() const { return m_height; }

    void OnResize(int width, int height);

private:
    bool CreateRenderTarget();
    void ReleaseRenderTarget();

    ComPtr<ID3D11Device>            m_device;
    ComPtr<ID3D11DeviceContext>     m_context;
    ComPtr<IDXGISwapChain>          m_swapChain;
    ComPtr<ID3D11RenderTargetView>  m_rtv;
    ComPtr<ID3D11DepthStencilView>  m_dsv;

    int m_width  = 0;
    int m_height = 0;
};
