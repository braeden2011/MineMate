// src/terrain/LineworkPass.cpp
#include "terrain/LineworkPass.h"

#include <d3dcompiler.h>

using namespace DirectX;

#define TO_WIDE_(s) L##s
#define TO_WIDE(s)  TO_WIDE_(s)

static const wchar_t kShaderPath[] = TO_WIDE(SHADERS_DIR_STR) L"/linework.hlsl";

bool LineworkPass::Init(ID3D11Device* device)
{
    // ── Compile vertex shader ─────────────────────────────────────────────
    ComPtr<ID3DBlob> vsBlob, errBlob;
    HRESULT hr = D3DCompileFromFile(
        kShaderPath, nullptr, nullptr,
        "VS", "vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0,
        vsBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        nullptr, m_vs.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Compile geometry shader ───────────────────────────────────────────
    ComPtr<ID3DBlob> gsBlob;
    errBlob.Reset();
    hr = D3DCompileFromFile(
        kShaderPath, nullptr, nullptr,
        "GS", "gs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0,
        gsBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreateGeometryShader(
        gsBlob->GetBufferPointer(), gsBlob->GetBufferSize(),
        nullptr, m_gs.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Compile pixel shader ──────────────────────────────────────────────
    ComPtr<ID3DBlob> psBlob;
    errBlob.Reset();
    hr = D3DCompileFromFile(
        kShaderPath, nullptr, nullptr,
        "PS", "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0,
        psBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
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
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
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

    // ── Rasterizer: solid, no culling ─────────────────────────────────────
    // The GS generates quads whose winding depends on line direction; CULL_NONE
    // ensures both orientations rasterize correctly.
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rd, m_rsState.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Depth stencil: depth test ON, depth write ON ──────────────────────
    // Linework writes depth so terrain behind it is correctly occluded, and so
    // the design surface (rendered after, depth write OFF) blends correctly over it.
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable    = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc      = D3D11_COMPARISON_LESS;
    hr = device->CreateDepthStencilState(&dsd, m_dsState.GetAddressOf());
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

    // ── Bind pipeline ─────────────────────────────────────────────────────
    ctx->IASetInputLayout(m_layout.Get());
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->GSSetShader(m_gs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);

    // MVP bound to VS slot b0; LineData bound to GS slot b1.
    ctx->VSSetConstantBuffers(0, 1, m_mvpCB.GetAddressOf());
    ctx->GSSetConstantBuffers(1, 1, m_lineDataCB.GetAddressOf());

    ctx->RSSetState(m_rsState.Get());
    ctx->OMSetDepthStencilState(m_dsState.Get(), 0);
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
}

void LineworkPass::Shutdown()
{
    m_dsState.Reset();
    m_rsState.Reset();
    m_lineDataCB.Reset();
    m_mvpCB.Reset();
    m_layout.Reset();
    m_ps.Reset();
    m_gs.Reset();
    m_vs.Reset();
}
