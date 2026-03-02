#pragma once
// src/terrain/Mesh.h
// Single tile mesh: immutable D3D11 vertex + index buffers loaded from a TLET .bin file.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <filesystem>

using Microsoft::WRL::ComPtr;

class Mesh
{
public:
    // Load a tile_*_lod0.bin (TLET format) into immutable GPU buffers.
    // Also computes the vertex AABB centre for camera placement.
    // Returns false and leaves the object invalid if the file is missing or malformed.
    bool Load(ID3D11Device* device, const std::filesystem::path& binPath);

    // Bind VB + IB and issue a DrawIndexed call.
    void Draw(ID3D11DeviceContext* ctx) const;

    bool IsValid() const { return m_indexCount > 0; }

    // Centre of the axis-aligned bounding box of all vertex positions.
    // Valid after a successful Load(); returns (0,0,0) otherwise.
    DirectX::XMFLOAT3 AabbCentre() const { return m_aabbCentre; }

private:
    ComPtr<ID3D11Buffer>  m_vb;
    ComPtr<ID3D11Buffer>  m_ib;
    UINT                  m_indexCount  = 0;
    DirectX::XMFLOAT3     m_aabbCentre  = {0.0f, 0.0f, 0.0f};
};
