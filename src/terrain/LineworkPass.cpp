// src/terrain/LineworkPass.cpp
#include "terrain/LineworkPass.h"

#include <filesystem>
#include <fstream>
#include <vector>

using namespace DirectX;
namespace fs = std::filesystem;

// ── Load a compiled shader object (.cso) from the exe directory ──────────────
static std::vector<uint8_t> LoadCso(const wchar_t* filename)
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const fs::path csoPath = fs::path(exePath).parent_path() / filename;
    std::ifstream f(csoPath, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto size = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return f ? data : std::vector<uint8_t>{};
}

bool LineworkPass::Init(ID3D11Device* device)
{
    // ── Load vertex shader from linework_vs.cso ───────────────────────────
    auto vsBytes = LoadCso(L"linework_vs.cso");
    if (vsBytes.empty()) return false;

    HRESULT hr = device->CreateVertexShader(
        vsBytes.data(), vsBytes.size(),
        nullptr, m_vs.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Load geometry shader from linework_gs.cso ─────────────────────────
    auto gsBytes = LoadCso(L"linework_gs.cso");
    if (gsBytes.empty()) return false;

    hr = device->CreateGeometryShader(
        gsBytes.data(), gsBytes.size(),
        nullptr, m_gs.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Load pixel shader from linework_ps.cso ────────────────────────────
    auto psBytes = LoadCso(L"linework_ps.cso");
    if (psBytes.empty()) return false;

    hr = device->CreatePixelShader(
        psBytes.data(), psBytes.size(),
        nullptr, m_ps.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Input layout — POSITION + COLOR (float3 each, 24-byte stride) ────
    const D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device->CreateInputLayout(
        layoutDesc, ARRAYSIZE(layoutDesc),
        vsBytes.data(), vsBytes.size(),
        m_layout.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Constant buffers ──────────────────────────────────────────────────
    D3D11_BUFFER_DESC cbd{};
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    cbd.ByteWidth = sizeof(MVPConstants);       // 192 bytes
    hr = device->CreateBuffer(&cbd, nullptr, m_mvpCB.GetAddressOf());
    if (FAILED(hr)) return false;

    cbd.ByteWidth = sizeof(LineDataConstants);  // 16 bytes
    hr = device->CreateBuffer(&cbd, nullptr, m_lineDataCB.GetAddressOf());
    if (FAILED(hr)) return false;

    cbd.ByteWidth = sizeof(LineAlphaConstants); // 16 bytes
    hr = device->CreateBuffer(&cbd, nullptr, m_lineAlphaCB.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Rasterizer: solid, no culling ─────────────────────────────────────
    // The GS generates quads whose winding depends on line direction; CULL_NONE
    // ensures both orientations rasterize correctly.
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rd, m_rsState.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Depth stencil: opaque path — depth test+write ON ─────────────────
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable    = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc      = D3D11_COMPARISON_LESS;
    hr = device->CreateDepthStencilState(&dsd, m_dsOpaque.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Depth stencil: transparent path — depth test ON, write OFF ────────
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    hr = device->CreateDepthStencilState(&dsd, m_dsTransparent.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Blend: SRC_ALPHA / INV_SRC_ALPHA (used when opacity < 1) ─────────
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&bd, m_blendState.GetAddressOf());
    if (FAILED(hr)) return false;

    // Default world matrix = identity; overridden via SetWorldMatrix if the
    // linework DXF has a different $EXTMIN to the terrain scene origin.
    XMStoreFloat4x4(&m_worldMatrix, XMMatrixIdentity());

    return true;
}

void LineworkPass::Begin(ID3D11DeviceContext* ctx,
                         const XMMATRIX& view,
                         const XMMATRIX& proj,
                         float viewportW, float viewportH)
{
    // ── Update MVP cbuffer ────────────────────────────────────────────────
    {
        D3D11_MAPPED_SUBRESOURCE ms{};
        ctx->Map(m_mvpCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        auto* cb = static_cast<MVPConstants*>(ms.pData);
        XMStoreFloat4x4(&cb->world, XMLoadFloat4x4(&m_worldMatrix));
        XMStoreFloat4x4(&cb->view,  view);
        XMStoreFloat4x4(&cb->proj,  proj);
        ctx->Unmap(m_mvpCB.Get(), 0);
    }

    // ── Update LineData cbuffer ───────────────────────────────────────────
    {
        D3D11_MAPPED_SUBRESOURCE ms{};
        ctx->Map(m_lineDataCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        auto* cb = static_cast<LineDataConstants*>(ms.pData);
        cb->viewportSize = { viewportW, viewportH };
        cb->_p0 = 0.0f;
        cb->_p1 = 0.0f;
        ctx->Unmap(m_lineDataCB.Get(), 0);
    }

    // ── Update LineAlpha cbuffer (PS b2) ─────────────────────────────────
    {
        D3D11_MAPPED_SUBRESOURCE ms{};
        ctx->Map(m_lineAlphaCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        auto* cb   = static_cast<LineAlphaConstants*>(ms.pData);
        cb->opacity = m_opacity;
        cb->_la0    = 0.0f; cb->_la1 = 0.0f; cb->_la2 = 0.0f;
        ctx->Unmap(m_lineAlphaCB.Get(), 0);
    }

    // ── Bind pipeline ─────────────────────────────────────────────────────
    ctx->IASetInputLayout(m_layout.Get());
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->GSSetShader(m_gs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);

    // MVP → VS b0; LineData → GS b1; LineAlpha → PS b2.
    ctx->VSSetConstantBuffers(0, 1, m_mvpCB.GetAddressOf());
    ctx->GSSetConstantBuffers(1, 1, m_lineDataCB.GetAddressOf());
    ctx->PSSetConstantBuffers(2, 1, m_lineAlphaCB.GetAddressOf());

    ctx->RSSetState(m_rsState.Get());

    if (m_opacity >= 1.0f) {
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(m_dsOpaque.Get(), 0);
    } else {
        ctx->OMSetBlendState(m_blendState.Get(), nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(m_dsTransparent.Get(), 0);
    }
}

void LineworkPass::Draw(ID3D11DeviceContext* ctx, const LineworkMesh& mesh)
{
    if (mesh.IsValid()) mesh.Draw(ctx);
}

void LineworkPass::End(ID3D11DeviceContext* ctx)
{
    // Unbind the geometry shader.  If not cleared, the GS would remain active
    // for subsequent TerrainPass and DesignPass draws, corrupting their output.
    ctx->GSSetShader(nullptr, nullptr, 0);
    // Restore opaque blend so subsequent passes are unaffected.
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
}

void LineworkPass::Shutdown()
{
    m_blendState.Reset();
    m_dsTransparent.Reset();
    m_dsOpaque.Reset();
    m_rsState.Reset();
    m_lineAlphaCB.Reset();
    m_lineDataCB.Reset();
    m_mvpCB.Reset();
    m_layout.Reset();
    m_ps.Reset();
    m_gs.Reset();
    m_vs.Reset();
}
