#pragma once
// src/terrain/LineworkPass.h
// Linework render pass — geometry shader expands LINELIST segments into
// screen-aligned quads (LINEWORK_WIDTH_PX wide, defined in Config.h).
//
// Pipeline stages:
//   VS — MVP transform (b0).
//   GS — line→quad expansion, viewport-aware width (b1).
//   PS — solid white (Phase 5); per-layer colour added Phase 8.
//
// State:
//   Rasterizer : solid, CULL_NONE (GS winding depends on line direction).
//   Depth      : test ON, write ON (linework writes depth so design blends over it).
//   Blend      : opaque (default, nullptr).
//
// IMPORTANT: End() calls GSSetShader(nullptr) to unbind the geometry shader.
// Failing to do this would corrupt subsequent TerrainPass / DesignPass draws.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "terrain/LineworkMesh.h"

using Microsoft::WRL::ComPtr;

class LineworkPass
{
public:
    // Compile linework.hlsl and create all pipeline objects.
    bool Init(ID3D11Device* device);

    // Bind pipeline; update MVP (b0) and LineData (b1) cbuffers.
    // viewportW / viewportH used to compute per-pixel line width in the GS.
    void Begin(ID3D11DeviceContext* ctx,
               const DirectX::XMMATRIX& view,
               const DirectX::XMMATRIX& proj,
               float viewportW, float viewportH);

    // Issue a single DrawIndexed call for the full linework mesh.
    void Draw(ID3D11DeviceContext* ctx, const LineworkMesh& mesh);

    // Unbinds the GS stage so subsequent passes are not affected.
    void End(ID3D11DeviceContext* ctx);

    // Override the world matrix written to the MVP cbuffer (default = identity).
    // Use XMMatrixTranslation(dx, dy, dz) to align linework whose DXF $EXTMIN
    // differs from the terrain scene origin.
    void SetWorldMatrix(const DirectX::XMMATRIX& w) {
        DirectX::XMStoreFloat4x4(&m_worldMatrix, w);
    }

    void Shutdown();

private:
    // Matches cbuffer MVP : register(b0) in linework.hlsl
    struct alignas(16) MVPConstants
    {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4X4 view;
        DirectX::XMFLOAT4X4 proj;
    };

    // Matches cbuffer LineData : register(b1) in linework.hlsl
    struct alignas(16) LineDataConstants
    {
        DirectX::XMFLOAT2 viewportSize;
        float _p0, _p1;
    };

    ComPtr<ID3D11VertexShader>      m_vs;
    ComPtr<ID3D11GeometryShader>    m_gs;
    ComPtr<ID3D11PixelShader>       m_ps;
    ComPtr<ID3D11InputLayout>       m_layout;
    ComPtr<ID3D11Buffer>            m_mvpCB;
    ComPtr<ID3D11Buffer>            m_lineDataCB;
    ComPtr<ID3D11RasterizerState>   m_rsState;   // solid, CULL_NONE
    ComPtr<ID3D11DepthStencilState> m_dsState;   // depth test ON, depth write ON
    DirectX::XMFLOAT4X4             m_worldMatrix;  // set via SetWorldMatrix; default identity
};
