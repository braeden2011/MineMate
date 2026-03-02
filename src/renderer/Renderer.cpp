#include "Renderer.h"

// Clear colour: RGB(45, 55, 72) expressed as linear floats
static constexpr float kClearColor[4] = { 0.176f, 0.216f, 0.282f, 1.0f };

bool Renderer::Init(HWND hwnd, int width, int height)
{
    m_width  = width;
    m_height = height;

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

    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL featureLevel;

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
        m_context.GetAddressOf());

    if (FAILED(hr)) return false;

    return CreateRenderTarget();
}

bool Renderer::CreateRenderTarget()
{
    // ── Back buffer RTV ───────────────────────────────────────────────────
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()))))
        return false;

    if (FAILED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr,
            m_rtv.GetAddressOf())))
        return false;

    // ── Depth-stencil texture + DSV ───────────────────────────────────────
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width            = static_cast<UINT>(m_width);
    dd.Height           = static_cast<UINT>(m_height);
    dd.MipLevels        = 1;
    dd.ArraySize        = 1;
    dd.Format           = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.Usage            = D3D11_USAGE_DEFAULT;
    dd.BindFlags        = D3D11_BIND_DEPTH_STENCIL;

    ComPtr<ID3D11Texture2D> depthTex;
    if (FAILED(m_device->CreateTexture2D(&dd, nullptr, depthTex.GetAddressOf())))
        return false;

    if (FAILED(m_device->CreateDepthStencilView(depthTex.Get(), nullptr,
            m_dsv.GetAddressOf())))
        return false;

    return true;
}

void Renderer::ReleaseRenderTarget()
{
    m_dsv.Reset();
    m_rtv.Reset();
}

void Renderer::OnResize(int width, int height)
{
    if (!m_swapChain) return;

    m_width  = width;
    m_height = height;

    ReleaseRenderTarget();
    m_swapChain->ResizeBuffers(0,
        static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
}

void Renderer::BeginFrame()
{
    // Bind render targets (colour + depth)
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), m_dsv.Get());

    // Clear
    m_context->ClearRenderTargetView(m_rtv.Get(), kClearColor);
    m_context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Full-window viewport
    D3D11_VIEWPORT vp{};
    vp.Width    = static_cast<float>(m_width);
    vp.Height   = static_cast<float>(m_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
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
