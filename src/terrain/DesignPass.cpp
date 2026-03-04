// src/terrain/DesignPass.cpp
#include "terrain/DesignPass.h"
#include "DxfTypes.h"       // dxf::TerrainVertex — for input layout offsets

#include <d3dcompiler.h>

using namespace DirectX;

#define TO_WIDE_(s) L##s
#define TO_WIDE(s)  TO_WIDE_(s)

static const wchar_t kShaderPath[] = TO_WIDE(SHADERS_DIR_STR) L"/design.hlsl";

bool DesignPass::Init(ID3D11Device* device)
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

    // ── Input layout — same TerrainVertex format as TerrainPass ─────────
    // float3 pos  (offset  0) → POSITION
    // float3 nrm  (offset 12) → NORMAL
    // float  color (offset 24) → COLOR
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

    cbd.ByteWidth = sizeof(MVPConstants);       // 192 bytes
    hr = device->CreateBuffer(&cbd, nullptr, m_mvpCB.GetAddressOf());
    if (FAILED(hr)) return false;

    cbd.ByteWidth = sizeof(LightConstants);     // 48 bytes
    hr = device->CreateBuffer(&cbd, nullptr, m_lightCB.GetAddressOf());
    if (FAILED(hr)) return false;

    cbd.ByteWidth = sizeof(TileDataConstants);  // 16 bytes
    hr = device->CreateBuffer(&cbd, nullptr, m_tileDataCB.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Rasterizer: front-cull — Pass A ───────────────────────────────────
    // No depth bias. Terrain always writes depth (TerrainPass fix), so the
    // LESS_EQUAL depth stencil state correctly hides design behind terrain.
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_FRONT;
    rd.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rd, m_rsFront.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Rasterizer: back-cull — Pass B ────────────────────────────────────
    rd.CullMode = D3D11_CULL_BACK;
    hr = device->CreateRasterizerState(&rd, m_rsBack.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Blend: SRC_ALPHA / INV_SRC_ALPHA ──────────────────────────────────
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

    // ── Depth stencil: test depth, NO depth write ─────────────────────────
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable    = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;  // handles coincident surfaces
    hr = device->CreateDepthStencilState(&dsd, m_dsState.GetAddressOf());
    if (FAILED(hr)) return false;

    // Default world matrix = identity; overridden via SetWorldMatrix if the
    // design DXF has a different $EXTMIN to the terrain scene origin.
    XMStoreFloat4x4(&m_worldMatrix, XMMatrixIdentity());

    return true;
}

void DesignPass::Begin(ID3D11DeviceContext* ctx,
                       const XMMATRIX& view,
                       const XMMATRIX& proj)
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

    // ── Update Light cbuffer (same light as terrain) ──────────────────────
    {
        D3D11_MAPPED_SUBRESOURCE ms{};
        ctx->Map(m_lightCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        auto* cb = static_cast<LightConstants*>(ms.pData);
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

    // Blend and depth stencil are fixed for the whole pass.
    // Rasterizer state switches per DrawMesh (Pass A vs Pass B).
    ctx->OMSetBlendState(m_blendState.Get(), nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(m_dsState.Get(), 0);
}

void DesignPass::DrawMesh(ID3D11DeviceContext* ctx, const Mesh& mesh, int /*lod*/)
{
    if (!mesh.IsValid()) return;

    // ── Update per-tile TileData cbuffer ──────────────────────────────────
    {
        D3D11_MAPPED_SUBRESOURCE ms{};
        ctx->Map(m_tileDataCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        auto* cb = static_cast<TileDataConstants*>(ms.pData);
        cb->lodTint = { 1.0f, 1.0f, 1.0f };
        cb->opacity = m_opacity;
        ctx->Unmap(m_tileDataCB.Get(), 0);
    }

    // ── Pass A: cull front faces (renders interior / back faces first) ────
    ctx->RSSetState(m_rsFront.Get());
    mesh.Draw(ctx);

    // ── Pass B: cull back faces (renders exterior / front faces on top) ───
    ctx->RSSetState(m_rsBack.Get());
    mesh.Draw(ctx);
}

void DesignPass::End(ID3D11DeviceContext* ctx)
{
    // Restore opaque blend so subsequent passes (ImGui) are not affected.
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
}

void DesignPass::Shutdown()
{
    m_dsState.Reset();
    m_blendState.Reset();
    m_rsBack.Reset();
    m_rsFront.Reset();
    m_tileDataCB.Reset();
    m_lightCB.Reset();
    m_mvpCB.Reset();
    m_layout.Reset();
    m_ps.Reset();
    m_vs.Reset();
}
