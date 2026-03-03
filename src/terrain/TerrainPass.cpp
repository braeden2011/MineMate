// src/terrain/TerrainPass.cpp
#include "terrain/TerrainPass.h"
#include "DxfTypes.h"       // dxf::TerrainVertex — for input layout offsets

#include <d3dcompiler.h>

using namespace DirectX;

// Widen a narrow string literal produced by a CMake compile definition.
// Usage: TO_WIDE(SHADERS_DIR_STR) expands to L"<path>"
#define TO_WIDE_(s) L##s
#define TO_WIDE(s)  TO_WIDE_(s)

static const wchar_t kShaderPath[] = TO_WIDE(SHADERS_DIR_STR) L"/terrain.hlsl";

bool TerrainPass::Init(ID3D11Device* device)
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

    // ── Input layout matching TerrainVertex (28 bytes) ───────────────────
    // float3 pos (offset  0) → POSITION
    // float3 nrm (offset 12) → NORMAL
    // float  color (offset 24) → COLOR (elevation tint, 0.0f in Phase 1)
    const D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32_FLOAT,        0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
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

    cbd.ByteWidth = sizeof(MVPConstants);   // 192 bytes — multiple of 16
    hr = device->CreateBuffer(&cbd, nullptr, m_mvpCB.GetAddressOf());
    if (FAILED(hr)) return false;

    cbd.ByteWidth = sizeof(LightConstants);    // 48 bytes — multiple of 16
    hr = device->CreateBuffer(&cbd, nullptr, m_lightCB.GetAddressOf());
    if (FAILED(hr)) return false;

    cbd.ByteWidth = sizeof(TileDataConstants); // 16 bytes — multiple of 16
    hr = device->CreateBuffer(&cbd, nullptr, m_tileDataCB.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Rasterizer: solid, no culling (winding verified in Phase 2 S2) ───
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rd, m_rsState.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Depth stencil: depth test and write always enabled ───────────────
    // Terrain always writes depth regardless of opacity. The depth buffer
    // records where the terrain surface IS, not whether it is visible.
    // Without this, semi-transparent terrain leaves the depth buffer clear
    // (1.0), causing the design surface's LESS_EQUAL test to always pass
    // and design to appear in front of terrain everywhere.
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable    = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc      = D3D11_COMPARISON_LESS;
    hr = device->CreateDepthStencilState(&dsd, m_dsOpaque.GetAddressOf());
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

    return true;
}

void TerrainPass::Begin(ID3D11DeviceContext* ctx,
                        const XMMATRIX& view,
                        const XMMATRIX& proj)
{
    // ── Update MVP cbuffer (world = identity; all tiles are in scene space) ──
    {
        D3D11_MAPPED_SUBRESOURCE ms{};
        ctx->Map(m_mvpCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        auto* cb = static_cast<MVPConstants*>(ms.pData);
        XMStoreFloat4x4(&cb->world, XMMatrixIdentity());
        XMStoreFloat4x4(&cb->view,  view);
        XMStoreFloat4x4(&cb->proj,  proj);
        ctx->Unmap(m_mvpCB.Get(), 0);
    }

    // ── Update Light cbuffer (fixed directional light) ────────────────────
    {
        D3D11_MAPPED_SUBRESOURCE ms{};
        ctx->Map(m_lightCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        auto* cb = static_cast<LightConstants*>(ms.pData);
        // Light from upper-front-right; lightDir points FROM surface TOWARD light.
        XMStoreFloat3(&cb->lightDir,
            XMVector3Normalize(XMVectorSet(0.3f, 0.5f, 0.8f, 0.0f)));
        cb->_p0     = 0.0f;
        cb->ambient = { 0.15f, 0.18f, 0.22f };
        cb->_p1     = 0.0f;
        cb->diffuse = { 0.75f, 0.70f, 0.65f };
        cb->_p2     = 0.0f;
        ctx->Unmap(m_lightCB.Get(), 0);
    }

    // ── Bind pipeline state ───────────────────────────────────────────────
    ctx->IASetInputLayout(m_layout.Get());
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, m_mvpCB.GetAddressOf());
    ctx->PSSetConstantBuffers(1, 1, m_lightCB.GetAddressOf());
    ctx->PSSetConstantBuffers(2, 1, m_tileDataCB.GetAddressOf());
    ctx->RSSetState(m_rsState.Get());

    // Depth write is always ON — terrain occludes design even when semi-transparent.
    ctx->OMSetDepthStencilState(m_dsOpaque.Get(), 0);
    if (m_opacity >= 1.0f)
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    else
        ctx->OMSetBlendState(m_blendState.Get(), nullptr, 0xffffffff);
}

void TerrainPass::DrawMesh(ID3D11DeviceContext* ctx, const Mesh& mesh, int lod)
{
    // ── Update per-tile TileData cbuffer ──────────────────────────────────
    {
        // LOD colour tints for the debug overlay.
        // When overlay is off all channels are 1.0 — no effect on output.
        static const XMFLOAT3 kTints[3] = {
            { 0.6f, 1.0f, 0.6f },   // LOD0 — green
            { 1.0f, 1.0f, 0.4f },   // LOD1 — yellow
            { 1.0f, 0.5f, 0.5f },   // LOD2 — red
        };
        D3D11_MAPPED_SUBRESOURCE ms{};
        ctx->Map(m_tileDataCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        auto* cb  = static_cast<TileDataConstants*>(ms.pData);
        const int  l = (lod >= 0 && lod < 3) ? lod : 0;
        cb->lodTint = m_showLodColour ? kTints[l] : XMFLOAT3{ 1.0f, 1.0f, 1.0f };
        cb->opacity = m_opacity;
        ctx->Unmap(m_tileDataCB.Get(), 0);
    }
    if (mesh.IsValid()) mesh.Draw(ctx);
}

void TerrainPass::End(ID3D11DeviceContext* ctx)
{
    // Restore opaque blend so subsequent passes (linework, design, ImGui) are unaffected.
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
}

void TerrainPass::Render(ID3D11DeviceContext* ctx,
                         const Mesh& mesh,
                         const XMMATRIX& view,
                         const XMMATRIX& proj)
{
    Begin(ctx, view, proj);
    DrawMesh(ctx, mesh, 0);
    End(ctx);
}

void TerrainPass::Shutdown()
{
    m_blendState.Reset();
    m_dsOpaque.Reset();
    m_rsState.Reset();
    m_tileDataCB.Reset();
    m_lightCB.Reset();
    m_mvpCB.Reset();
    m_layout.Reset();
    m_ps.Reset();
    m_vs.Reset();
}
