// src/terrain/TerrainPass.cpp
#include "terrain/TerrainPass.h"
#include "DxfTypes.h"       // dxf::TerrainVertex — for input layout offsets

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

bool TerrainPass::Init(ID3D11Device* device)
{
    // ── Load vertex shader from terrain_vs.cso ────────────────────────────
    auto vsBytes = LoadCso(L"terrain_vs.cso");
    if (vsBytes.empty()) return false;

    HRESULT hr = device->CreateVertexShader(
        vsBytes.data(), vsBytes.size(),
        nullptr, m_vs.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Load pixel shader from terrain_ps.cso ────────────────────────────
    auto psBytes = LoadCso(L"terrain_ps.cso");
    if (psBytes.empty()) return false;

    hr = device->CreatePixelShader(
        psBytes.data(), psBytes.size(),
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
        vsBytes.data(), vsBytes.size(),
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

    cbd.ByteWidth = sizeof(TileDataConstants); // 32 bytes — multiple of 16
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

void TerrainPass::DrawMesh(ID3D11DeviceContext* ctx, const Mesh& mesh, int /*lod*/)
{
    // ── Update per-tile TileData cbuffer ──────────────────────────────────
    {
        D3D11_MAPPED_SUBRESOURCE ms{};
        ctx->Map(m_tileDataCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        auto* cb  = static_cast<TileDataConstants*>(ms.pData);
        cb->lodTint     = { 1.0f, 1.0f, 1.0f };
        cb->opacity     = m_opacity;
        cb->overlayColor = m_overlayColor;
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
