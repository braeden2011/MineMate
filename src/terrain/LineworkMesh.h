#pragma once
// src/terrain/LineworkMesh.h
// GPU vertex/index buffers for linework polylines.
//
// Topology: D3D11_PRIMITIVE_TOPOLOGY_LINELIST — each pair of indices is one segment.
// Vertex format: float3 position + float3 color (24 bytes, POSITION+COLOR semantics).
//   offset  0: POSITION  R32G32B32_FLOAT
//   offset 12: COLOR     R32G32B32_FLOAT  (linear RGB, from ACI colour code)
// Index format:  uint32 (architecture rule — never uint16).
//
// Vertices are in scene space (origin-offset already applied by the DXF parser).
// Colour is uniform per polyline, derived from ParsedPolyline::colorAci via AciToRgb.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "DxfTypes.h"   // dxf::ParsedPolyline

using Microsoft::WRL::ComPtr;

class LineworkMesh
{
public:
    // Build immutable VB + IB from parsed polylines.
    // Polylines with fewer than 2 vertices are silently skipped.
    // Returns true if at least one line segment was created.
    bool Load(ID3D11Device* device,
              const std::vector<dxf::ParsedPolyline>& polylines);

    // Bind VB/IB and issue DrawIndexed with LINELIST topology.
    void Draw(ID3D11DeviceContext* ctx) const;

    bool     IsValid()      const { return m_indexCount > 0; }
    uint32_t VertCount()    const { return m_vertCount;      }
    uint32_t SegmentCount() const { return m_indexCount / 2; }

private:
    ComPtr<ID3D11Buffer> m_vb;
    ComPtr<ID3D11Buffer> m_ib;
    uint32_t             m_vertCount  = 0;
    uint32_t             m_indexCount = 0;  // always even (line segment pairs)
};
