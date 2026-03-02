// src/terrain/LineworkMesh.cpp
#include "terrain/LineworkMesh.h"

#include <array>
#include <cstddef>

bool LineworkMesh::Load(ID3D11Device* device,
                        const std::vector<dxf::ParsedPolyline>& polylines)
{
    // ── Build CPU-side vertex and index arrays ─────────────────────────────
    // Vertex buffer: packed float3 positions (12 bytes each).
    // Index buffer:  consecutive pairs — one pair per line segment.
    //
    // For a polyline with N vertices (indices 0..N-1 within the polyline),
    // we add N-1 segment pairs: (base+0, base+1), (base+1, base+2), ...
    // 'base' is the first vertex of this polyline in the combined VB.

    std::vector<float>    vbData;   // x0,y0,z0, x1,y1,z1, ...
    std::vector<uint32_t> ibData;   // i0a,i0b, i1a,i1b, ...

    for (const auto& poly : polylines) {
        if (poly.verts.size() < 2) continue;

        const uint32_t base = static_cast<uint32_t>(vbData.size() / 3);

        for (const auto& v : poly.verts) {
            vbData.push_back(v[0]);
            vbData.push_back(v[1]);
            vbData.push_back(v[2]);
        }

        for (size_t i = 0; i + 1 < poly.verts.size(); ++i) {
            ibData.push_back(base + static_cast<uint32_t>(i));
            ibData.push_back(base + static_cast<uint32_t>(i + 1));
        }
    }

    if (ibData.empty()) return false;

    m_vertCount  = static_cast<uint32_t>(vbData.size() / 3);
    m_indexCount = static_cast<uint32_t>(ibData.size());

    // ── Create immutable vertex buffer (POSITION only, 12-byte stride) ────
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth  = static_cast<UINT>(vbData.size() * sizeof(float));
    vbd.Usage      = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags  = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbInit{ vbData.data(), 0, 0 };
    HRESULT hr = device->CreateBuffer(&vbd, &vbInit, m_vb.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Create immutable index buffer (uint32) ─────────────────────────────
    D3D11_BUFFER_DESC ibd{};
    ibd.ByteWidth  = static_cast<UINT>(ibData.size() * sizeof(uint32_t));
    ibd.Usage      = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags  = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibInit{ ibData.data(), 0, 0 };
    hr = device->CreateBuffer(&ibd, &ibInit, m_ib.GetAddressOf());
    if (FAILED(hr)) { m_vb.Reset(); return false; }

    return true;
}

void LineworkMesh::Draw(ID3D11DeviceContext* ctx) const
{
    if (!IsValid()) return;

    const UINT stride = sizeof(float) * 3;   // 12 bytes: float3 position
    const UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, m_vb.GetAddressOf(), &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ctx->DrawIndexed(m_indexCount, 0, 0);
}
