#pragma once
// src/terrain/TerrainPass.h
// Terrain render pass: compiles terrain.hlsl, manages cbuffers, issues draw calls.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "terrain/Mesh.h"

using Microsoft::WRL::ComPtr;

class TerrainPass
{
public:
    // Compile shaders and create all pipeline objects.
    bool Init(ID3D11Device* device);

    // Multi-tile rendering: call Begin once, DrawMesh once per tile, End once.
    void Begin(ID3D11DeviceContext* ctx,
               const DirectX::XMMATRIX& view,
               const DirectX::XMMATRIX& proj);
    // lod: 0/1/2 — used to set lodTint in the TileData cbuffer.
    void DrawMesh(ID3D11DeviceContext* ctx, const Mesh& mesh, int lod);
    void End();

    // Toggle the LOD colour overlay (LOD0=green, LOD1=yellow, LOD2=red).
    void SetShowLodColour(bool show) { m_showLodColour = show; }
    bool GetShowLodColour()    const { return m_showLodColour; }

    // Single-tile convenience — delegates to Begin/DrawMesh(lod=0)/End.
    void Render(ID3D11DeviceContext* ctx,
                const Mesh& mesh,
                const DirectX::XMMATRIX& view,
                const DirectX::XMMATRIX& proj);

    void Shutdown();

private:
    // Matches cbuffer MVP : register(b0) in terrain.hlsl
    struct alignas(16) MVPConstants
    {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4X4 view;
        DirectX::XMFLOAT4X4 proj;
    };

    // Matches cbuffer Light : register(b1) in terrain.hlsl
    struct alignas(16) LightConstants
    {
        DirectX::XMFLOAT3 lightDir; float _p0;
        DirectX::XMFLOAT3 ambient;  float _p1;
        DirectX::XMFLOAT3 diffuse;  float _p2;
    };

    // Matches cbuffer TileData : register(b2) in terrain.hlsl
    struct alignas(16) TileDataConstants
    {
        DirectX::XMFLOAT3 lodTint;  float _tp;
    };

    ComPtr<ID3D11VertexShader>      m_vs;
    ComPtr<ID3D11PixelShader>       m_ps;
    ComPtr<ID3D11InputLayout>       m_layout;
    ComPtr<ID3D11Buffer>            m_mvpCB;
    ComPtr<ID3D11Buffer>            m_lightCB;
    ComPtr<ID3D11Buffer>            m_tileDataCB;
    ComPtr<ID3D11RasterizerState>   m_rsState;
    ComPtr<ID3D11DepthStencilState> m_dsState;

    bool m_showLodColour = false;
};
