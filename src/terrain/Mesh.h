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

    // Axis-aligned bounding box of all vertex positions.
    // All three are valid after a successful Load(); return (0,0,0) otherwise.
    DirectX::XMFLOAT3 AabbCentre() const { return m_aabbCentre; }
    DirectX::XMFLOAT3 AabbMin()    const { return m_aabbMin; }
    DirectX::XMFLOAT3 AabbMax()    const { return m_aabbMax; }

    // Diagnostic accessors — populated by Load(), used for ImGui readout.
    UINT                  VertCount()      const { return m_vertCount; }
    UINT                  IndexCount()     const { return m_indexCount; }
    DirectX::XMFLOAT3     DebugVert(int i) const { return m_debugVerts[i]; }

private:
    ComPtr<ID3D11Buffer>  m_vb;
    ComPtr<ID3D11Buffer>  m_ib;
    UINT                  m_indexCount    = 0;
    UINT                  m_vertCount     = 0;
    DirectX::XMFLOAT3     m_aabbCentre    = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3     m_aabbMin       = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3     m_aabbMax       = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3     m_debugVerts[3] = {};
};
