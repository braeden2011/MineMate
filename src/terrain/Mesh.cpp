// src/terrain/Mesh.cpp
#include "terrain/Mesh.h"
#include "DxfTypes.h"   // dxf::TerrainVertex — layout must match GPU input layout

#include <cfloat>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

bool Mesh::Load(ID3D11Device* device, const fs::path& binPath)
{
    std::ifstream f(binPath, std::ios::binary);
    if (!f) return false;

    uint32_t magic, version, lod, vertCount, indexCount;
    f.read(reinterpret_cast<char*>(&magic),      4);
    f.read(reinterpret_cast<char*>(&version),    4);
    f.read(reinterpret_cast<char*>(&lod),        4);
    f.read(reinterpret_cast<char*>(&vertCount),  4);
    f.read(reinterpret_cast<char*>(&indexCount), 4);

    if (!f || magic != 0x544C4554u || version != 1u) return false;
    if (vertCount == 0 || indexCount == 0)           return false;

    std::vector<dxf::TerrainVertex> verts(vertCount);
    f.read(reinterpret_cast<char*>(verts.data()),
           static_cast<std::streamsize>(vertCount * sizeof(dxf::TerrainVertex)));

    std::vector<uint32_t> indices(indexCount);
    f.read(reinterpret_cast<char*>(indices.data()),
           static_cast<std::streamsize>(indexCount * sizeof(uint32_t)));

    if (!f) return false;

    // ── Compute AABB centre for camera placement ──────────────────────────
    float xMin =  FLT_MAX, yMin =  FLT_MAX, zMin =  FLT_MAX;
    float xMax = -FLT_MAX, yMax = -FLT_MAX, zMax = -FLT_MAX;
    for (const auto& v : verts) {
        if (v.px < xMin) xMin = v.px;  if (v.px > xMax) xMax = v.px;
        if (v.py < yMin) yMin = v.py;  if (v.py > yMax) yMax = v.py;
        if (v.pz < zMin) zMin = v.pz;  if (v.pz > zMax) zMax = v.pz;
    }
    m_aabbCentre = {
        (xMin + xMax) * 0.5f,
        (yMin + yMax) * 0.5f,
        (zMin + zMax) * 0.5f
    };

    // ── Immutable vertex buffer ───────────────────────────────────────────
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = static_cast<UINT>(vertCount * sizeof(dxf::TerrainVertex));
    vbd.Usage     = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd{ verts.data(), 0, 0 };
    if (FAILED(device->CreateBuffer(&vbd, &vsd, m_vb.GetAddressOf()))) return false;

    // ── Immutable index buffer (uint32) ───────────────────────────────────
    D3D11_BUFFER_DESC ibd{};
    ibd.ByteWidth = static_cast<UINT>(indexCount * sizeof(uint32_t));
    ibd.Usage     = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd{ indices.data(), 0, 0 };
    if (FAILED(device->CreateBuffer(&ibd, &isd, m_ib.GetAddressOf()))) return false;

    m_indexCount = indexCount;
    return true;
}

void Mesh::Draw(ID3D11DeviceContext* ctx) const
{
    UINT stride = static_cast<UINT>(sizeof(dxf::TerrainVertex));  // 28 bytes
    UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, m_vb.GetAddressOf(), &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->DrawIndexed(m_indexCount, 0, 0);
}
