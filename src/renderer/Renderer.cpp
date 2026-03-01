#include "Renderer.h"
#include <stdexcept>

// Clear colour: RGB(45, 55, 72) → linear approximation
static constexpr float kClearColor[4] = {
    45.0f  / 255.0f,
    55.0f  / 255.0f,
    72.0f  / 255.0f,
    1.0f
};

bool Renderer::Init(HWND hwnd, int width, int height)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = static_cast<UINT>(width);
    sd.BufferDesc.Height                  = static_cast<UINT>(height);
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.SampleDesc.Quality                 = 0;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &sd,
        m_swapChain.GetAddressOf(),
        m_device.GetAddressOf(),
        &featureLevel,
        m_context.GetAddressOf()
    );

    if (FAILED(hr))
        return false;

    return CreateRenderTarget();
}

bool Renderer::CreateRenderTarget()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()))))
        return false;

    if (FAILED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr,
            m_rtv.GetAddressOf())))
        return false;

    return true;
}

void Renderer::ReleaseRenderTarget()
{
    m_rtv.Reset();
}

void Renderer::OnResize(int width, int height)
{
    if (!m_swapChain)
        return;

    ReleaseRenderTarget();
    m_swapChain->ResizeBuffers(0,
        static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
}

void Renderer::BeginFrame()
{
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    m_context->ClearRenderTargetView(m_rtv.Get(), kClearColor);
}

void Renderer::EndFrame()
{
    m_swapChain->Present(1, 0);
}

void Renderer::Shutdown()
{
    ReleaseRenderTarget();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();
}
