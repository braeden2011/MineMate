#pragma once
// src/terrain/DesignPass.h
// Design surface render pass: two-pass alpha blend using design.hlsl.
//
// Render order per tile:
//   Pass A — CullFront: draws back faces first
//   Pass B — CullBack:  draws front faces on top
// Both passes: blend ON (SRC_ALPHA / INV_SRC_ALPHA), depth write OFF.
// DepthFunc: LESS_EQUAL so design renders correctly on coincident terrain.
// No depth bias — bias was previously negative, causing design to always
// appear in front of terrain regardless of actual geometry depth.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "terrain/Mesh.h"

using Microsoft::WRL::ComPtr;

class DesignPass
{
public:
    // Compile design.hlsl and create all pipeline objects.
    bool Init(ID3D11Device* device);

    // Multi-tile rendering: call Begin once, DrawMesh once per tile, End once.
    void Begin(ID3D11DeviceContext* ctx,
               const DirectX::XMMATRIX& view,
               const DirectX::XMMATRIX& proj);

    // Issues two draw calls per tile (Pass A then Pass B).
    // lod: 0/1/2 — used to set lodTint in the TileData cbuffer.
    void DrawMesh(ID3D11DeviceContext* ctx, const Mesh& mesh, int lod);

    // Restores opaque blend state so subsequent passes (e.g. ImGui) are unaffected.
    void End(ID3D11DeviceContext* ctx);

    // Toggle the LOD colour overlay (LOD0=green, LOD1=yellow, LOD2=red).
    void SetShowLodColour(bool show) { m_showLodColour = show; }
    bool GetShowLodColour()    const { return m_showLodColour; }

    // Design surface opacity (0=transparent, 1=opaque). Default 0.6.
    void  SetOpacity(float v) { m_opacity = v; }
    float GetOpacity()  const { return m_opacity; }

    // Override the world matrix written to the MVP cbuffer (default = identity).
    // Use XMMatrixTranslation(dx, dy, dz) to align a design grid whose DXF
    // $EXTMIN differs from the terrain scene origin.
    void SetWorldMatrix(const DirectX::XMMATRIX& w) {
        DirectX::XMStoreFloat4x4(&m_worldMatrix, w);
    }

    void Shutdown();

private:
    // Matches cbuffer MVP : register(b0) in design.hlsl
    struct alignas(16) MVPConstants
    {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4X4 view;
        DirectX::XMFLOAT4X4 proj;
    };

    // Matches cbuffer Light : register(b1) in design.hlsl
    struct alignas(16) LightConstants
    {
        DirectX::XMFLOAT3 lightDir; float _p0;
        DirectX::XMFLOAT3 ambient;  float _p1;
        DirectX::XMFLOAT3 diffuse;  float _p2;
    };

    // Matches cbuffer TileData : register(b2) in design.hlsl
    struct alignas(16) TileDataConstants
    {
        DirectX::XMFLOAT3 lodTint;
        float             opacity;  // hardcoded 0.6 in Phase 4; slider added Phase 8
    };

    ComPtr<ID3D11VertexShader>      m_vs;
    ComPtr<ID3D11PixelShader>       m_ps;
    ComPtr<ID3D11InputLayout>       m_layout;
    ComPtr<ID3D11Buffer>            m_mvpCB;
    ComPtr<ID3D11Buffer>            m_lightCB;
    ComPtr<ID3D11Buffer>            m_tileDataCB;
    ComPtr<ID3D11RasterizerState>   m_rsFront;    // CullFront + depth bias — Pass A
    ComPtr<ID3D11RasterizerState>   m_rsBack;     // CullBack  + depth bias — Pass B
    ComPtr<ID3D11BlendState>        m_blendState; // SRC_ALPHA / INV_SRC_ALPHA
    ComPtr<ID3D11DepthStencilState> m_dsState;    // depth test ON, depth write OFF

    bool                        m_showLodColour = false;
    float                       m_opacity = 0.6f;
    DirectX::XMFLOAT4X4         m_worldMatrix;  // set via SetWorldMatrix; default identity
};
