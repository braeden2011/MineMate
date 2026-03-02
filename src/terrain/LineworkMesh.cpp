// src/terrain/LineworkMesh.cpp
#include "terrain/LineworkMesh.h"

#include <array>
#include <cmath>
#include <cstddef>

// ── ACI → linear RGB ─────────────────────────────────────────────────────────
// Converts an AutoCAD Color Index (1–255) to a linear RGB triple [0,1].
// ACI 256 = BYLAYER → uses a mid-grey default.
// Formula for spectral range 10–249:
//   group   = (aci - 10) / 10   (0..23, hue = group × 15°)
//   variant = (aci - 10) % 10   (alternating saturation 1.0/0.5, falling value)
// Special colours 1–9 and grey ramp 250–255 are hardcoded.

static std::array<float, 3> AciToRgb(int aci)
{
    // Default / BYLAYER / out-of-range
    if (aci <= 0 || aci == 256 || aci > 255)
        return {0.75f, 0.75f, 0.75f};

    // Colours 1–9: fixed
    static const float kFixed[9][3] = {
        {1.00f, 0.00f, 0.00f},  // 1  red
        {1.00f, 1.00f, 0.00f},  // 2  yellow
        {0.00f, 1.00f, 0.00f},  // 3  green
        {0.00f, 1.00f, 1.00f},  // 4  cyan
        {0.00f, 0.00f, 1.00f},  // 5  blue
        {1.00f, 0.00f, 1.00f},  // 6  magenta
        {1.00f, 1.00f, 1.00f},  // 7  white
        {0.498f,0.498f,0.498f}, // 8  dark grey
        {0.749f,0.749f,0.749f}, // 9  light grey
    };
    if (aci >= 1 && aci <= 9)
        return {kFixed[aci-1][0], kFixed[aci-1][1], kFixed[aci-1][2]};

    // Greyscale ramp 250–255
    if (aci >= 250) {
        float g = 0.2f + (aci - 250) * (0.8f / 5.0f);
        return {g, g, g};
    }

    // Spectral range 10–249
    static const float kSat[10] = {1.f,0.5f,1.f,0.5f,1.f,0.5f,1.f,0.5f,1.f,0.5f};
    static const float kVal[10] = {1.f,1.f,0.82f,0.82f,0.63f,0.63f,0.45f,0.45f,0.26f,0.26f};

    const int   group   = (aci - 10) / 10;
    const int   variant = (aci - 10) % 10;
    const float hue     = group * 15.0f;   // degrees, 0..345
    const float s       = kSat[variant];
    const float v       = kVal[variant];

    // HSV → RGB
    const float h = hue / 60.0f;
    const int   i = static_cast<int>(h);
    const float f = h - static_cast<float>(i);
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));

    switch (i % 6) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}

bool LineworkMesh::Load(ID3D11Device* device,
                        const std::vector<dxf::ParsedPolyline>& polylines)
{
    // ── Build CPU-side vertex and index arrays ─────────────────────────────
    // Vertex buffer: interleaved float3 pos + float3 color (24 bytes each).
    // Index buffer:  consecutive pairs — one pair per line segment.
    //
    // For a polyline with N vertices (indices 0..N-1 within the polyline),
    // we add N-1 segment pairs: (base+0, base+1), (base+1, base+2), ...
    // 'base' is the first vertex of this polyline in the combined VB.
    // Colour is uniform per polyline (derived from ParsedPolyline::colorAci).

    std::vector<float>    vbData;   // x,y,z,r,g,b repeated per vertex
    std::vector<uint32_t> ibData;   // i0a,i0b, i1a,i1b, ...

    for (const auto& poly : polylines) {
        if (poly.verts.size() < 2) continue;

        const uint32_t base = static_cast<uint32_t>(vbData.size() / 6);
        const auto     rgb  = AciToRgb(poly.colorAci);

        for (const auto& v : poly.verts) {
            vbData.push_back(v[0]);
            vbData.push_back(v[1]);
            vbData.push_back(v[2]);
            vbData.push_back(rgb[0]);
            vbData.push_back(rgb[1]);
            vbData.push_back(rgb[2]);
        }

        for (size_t i = 0; i + 1 < poly.verts.size(); ++i) {
            ibData.push_back(base + static_cast<uint32_t>(i));
            ibData.push_back(base + static_cast<uint32_t>(i + 1));
        }
    }

    if (ibData.empty()) return false;

    m_vertCount  = static_cast<uint32_t>(vbData.size() / 6);
    m_indexCount = static_cast<uint32_t>(ibData.size());

    // ── Create immutable vertex buffer (POSITION + COLOR, 24-byte stride) ──
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

    const UINT stride = sizeof(float) * 6;   // 24 bytes: float3 position + float3 color
    const UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, m_vb.GetAddressOf(), &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ctx->DrawIndexed(m_indexCount, 0, 0);
}
